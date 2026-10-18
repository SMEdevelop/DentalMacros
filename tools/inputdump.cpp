// A raw-input dump, for working out what a mouse sends from its extra buttons.
//
//     build\inputdump.exe            everything except pointer movement
//     build\inputdump.exe 046D       only devices whose path carries that VID
//
// Prints one line per event, and writes the same to build\inputdump.log. It
// registers for every top-level collection any attached device declares - not
// just mouse and keyboard - because the side panel on a gaming mouse usually
// arrives on a collection of its own, sometimes on a vendor page that nothing
// would guess.
//
// Nothing here is part of Dental Macros. It exists to answer "what does this
// device actually send", which has to be settled before deciding where
// support for it belongs.
#include <windows.h>

#include <hidusage.h>
#include <hidpi.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

static FILE* g_log = nullptr;
static std::string g_filter;

static void out(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    std::fflush(stdout);
    if (g_log) {
        std::fputs(buf, g_log);
        std::fflush(g_log);
    }
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr,
                        nullptr);
    return s;
}

static std::string devicePath(HANDLE h) {
    UINT n = 0;
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, nullptr, &n) != 0 || n == 0)
        return std::string();
    std::wstring w(n, L'\0');
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, &w[0], &n) == (UINT)-1)
        return std::string();
    while (!w.empty() && w.back() == L'\0') w.pop_back();
    return narrow(w);
}

// The VID/PID part of the path, so an event line stays readable.
static std::string shortPath(const std::string& p) {
    size_t a = p.find("VID_");
    if (a == std::string::npos) return p.substr(0, 60);
    size_t b = p.find('{');
    return p.substr(a, (b == std::string::npos ? p.size() : b) - a);
}

static bool wanted(const std::string& path) {
    if (g_filter.empty()) return true;
    std::string lp = path, lf = g_filter;
    for (size_t i = 0; i < lp.size(); ++i) lp[i] = (char)tolower(lp[i]);
    for (size_t i = 0; i < lf.size(); ++i) lf[i] = (char)tolower(lf[i]);
    return lp.find(lf) != std::string::npos;
}

static std::unordered_map<void*, std::string> g_paths;

static const std::string& pathFor(HANDLE h) {
    auto it = g_paths.find((void*)h);
    if (it == g_paths.end()) it = g_paths.emplace((void*)h, devicePath(h)).first;
    return it->second;
}

// -------------------------------------------------------------------- HID
static void dumpHid(const RAWINPUT* raw, const std::string& path) {
    const RAWHID& hid = raw->data.hid;
    std::string hex;
    const BYTE* b = hid.bRawData;
    for (DWORD i = 0; i < hid.dwSizeHid && i < 24; ++i) {
        char t[8];
        _snprintf_s(t, sizeof(t), _TRUNCATE, "%02X ", b[i]);
        hex += t;
    }

    out("HID    %-30s size=%lu count=%lu  bytes: %s\n", shortPath(path).c_str(),
        hid.dwSizeHid, hid.dwCount, hex.c_str());

    UINT ppSize = 0;
    if (GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_PREPARSEDDATA, nullptr,
                               &ppSize) != 0 ||
        ppSize == 0)
        return;
    std::vector<BYTE> pp(ppSize);
    if (GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_PREPARSEDDATA, &pp[0],
                               &ppSize) == (UINT)-1)
        return;
    PHIDP_PREPARSED_DATA preparsed = (PHIDP_PREPARSED_DATA)&pp[0];

    HIDP_CAPS caps;
    ZeroMemory(&caps, sizeof(caps));
    if (HidP_GetCaps(preparsed, &caps) != HIDP_STATUS_SUCCESS) return;
    if (caps.NumberInputButtonCaps == 0) {
        out("       (this collection declares no input buttons)\n");
        return;
    }
    std::vector<HIDP_BUTTON_CAPS> bc(caps.NumberInputButtonCaps);
    USHORT n = caps.NumberInputButtonCaps;
    if (HidP_GetButtonCaps(HidP_Input, &bc[0], &n, preparsed) !=
        HIDP_STATUS_SUCCESS)
        return;

    // Every button usage page this collection declares, not only the Button
    // page - a vendor page is exactly the case worth seeing.
    for (USHORT i = 0; i < n; ++i) {
        const USAGE page = bc[i].UsagePage;
        ULONG count = HidP_MaxUsageListLength(HidP_Input, page, preparsed);
        if (count == 0) continue;
        std::vector<USAGE> usages(count);
        NTSTATUS st =
            HidP_GetUsages(HidP_Input, page, 0, &usages[0], &count, preparsed,
                           (PCHAR)hid.bRawData, hid.dwSizeHid);
        if (st != HIDP_STATUS_SUCCESS) {
            out("       page 0x%04X: HidP_GetUsages failed (0x%08X)\n",
                (unsigned)page, (unsigned)st);
            continue;
        }
        std::string list;
        for (ULONG u = 0; u < count; ++u) {
            char t[16];
            _snprintf_s(t, sizeof(t), _TRUNCATE, "%u ", (unsigned)usages[u]);
            list += t;
        }
        out("       page 0x%04X (%s) down: %s\n", (unsigned)page,
            page == HID_USAGE_PAGE_BUTTON ? "Button" : "other",
            count ? list.c_str() : "(none)");
    }
}

// ----------------------------------------------------------------- window
static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m != WM_INPUT) return DefWindowProcW(h, m, w, l);

    UINT size = 0;
    if (GetRawInputData((HRAWINPUT)l, RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER)) != 0 ||
        size == 0)
        return DefWindowProcW(h, m, w, l);
    std::vector<BYTE> buf(size);
    if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &buf[0], &size,
                        sizeof(RAWINPUTHEADER)) != size)
        return DefWindowProcW(h, m, w, l);
    RAWINPUT* raw = (RAWINPUT*)&buf[0];
    const std::string& path = pathFor(raw->header.hDevice);
    if (!wanted(path)) return DefWindowProcW(h, m, w, l);

    if (raw->header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE& ms = raw->data.mouse;
        if (ms.usButtonFlags == 0)
            return DefWindowProcW(h, m, w, l);  // movement only
        out("MOUSE  %-30s buttonFlags=0x%04X buttonData=%d\n",
            shortPath(path).c_str(), (unsigned)ms.usButtonFlags,
            (int)(short)ms.usButtonData);
    } else if (raw->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& kb = raw->data.keyboard;
        out("KEYBD  %-30s vk=0x%02X (%d) make=0x%02X flags=0x%02X %s\n",
            shortPath(path).c_str(), (unsigned)kb.VKey, (int)kb.VKey,
            (unsigned)kb.MakeCode, (unsigned)kb.Flags,
            (kb.Flags & RI_KEY_BREAK) ? "UP" : "DOWN");
    } else if (raw->header.dwType == RIM_TYPEHID) {
        dumpHid(raw, path);
    }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    if (argc > 1) g_filter = argv[1];
    SetConsoleOutputCP(CP_UTF8);
    fopen_s(&g_log, "build\\inputdump.log", "w");

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"InputDumpWnd";
    RegisterClassExW(&wc);
    // Not HWND_MESSAGE: a message-only window never receives WM_INPUT.
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"inputdump", 0, 0, 0, 0, 0,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        out("CreateWindow failed: %lu\n", GetLastError());
        return 1;
    }

    // ---- what is attached, and every collection each device declares
    UINT count = 0;
    GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST));
    std::vector<RAWINPUTDEVICELIST> list(count ? count : 1);
    if (count)
        GetRawInputDeviceList(&list[0], &count, sizeof(RAWINPUTDEVICELIST));
    list.resize(count);

    out("---- devices ---------------------------------------------------\n");
    std::vector<std::pair<USHORT, USHORT> > pairs;
    for (size_t i = 0; i < list.size(); ++i) {
        const std::string p = devicePath(list[i].hDevice);
        RID_DEVICE_INFO info;
        ZeroMemory(&info, sizeof(info));
        info.cbSize = sizeof(info);
        UINT sz = sizeof(info);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info,
                                   &sz) == (UINT)-1)
            continue;
        USHORT page = 0, usage = 0;
        const char* kind = "?";
        if (info.dwType == RIM_TYPEMOUSE) {
            kind = "mouse";
            page = 0x01;
            usage = 0x02;
        } else if (info.dwType == RIM_TYPEKEYBOARD) {
            kind = "keyboard";
            page = 0x01;
            usage = 0x06;
        } else {
            kind = "hid";
            page = info.hid.usUsagePage;
            usage = info.hid.usUsage;
        }
        bool seen = false;
        for (size_t k = 0; k < pairs.size(); ++k)
            if (pairs[k].first == page && pairs[k].second == usage) seen = true;
        if (!seen) pairs.push_back(std::make_pair(page, usage));
        if (wanted(p))
            out("  %-8s page=0x%04X usage=0x%02X  %s\n", kind, (unsigned)page,
                (unsigned)usage, p.c_str());
    }

    std::vector<RAWINPUTDEVICE> rid;
    for (size_t i = 0; i < pairs.size(); ++i) {
        RAWINPUTDEVICE r;
        ZeroMemory(&r, sizeof(r));
        r.usUsagePage = pairs[i].first;
        r.usUsage = pairs[i].second;
        r.dwFlags = RIDEV_INPUTSINK;  // keep receiving without focus
        r.hwndTarget = hwnd;
        rid.push_back(r);
    }
    if (rid.empty() || !RegisterRawInputDevices(&rid[0], (UINT)rid.size(),
                                                sizeof(RAWINPUTDEVICE))) {
        out("RegisterRawInputDevices failed: %lu\n", GetLastError());
        return 1;
    }
    out("---- listening on %u collections. Press the buttons.\n",
        (unsigned)rid.size());
    out("---- events ----------------------------------------------------\n");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
