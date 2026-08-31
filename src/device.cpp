#include "device.h"

#include <hidusage.h>
#include <hidpi.h>
#include "util.h"

#include <chrono>
#include <regex>
#include <stdexcept>

namespace dev {

double nowSeconds() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

// ------------------------------------------------------------ device names
namespace {

std::string devicePathFor(HANDLE h) {
    UINT size = 0;
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, nullptr, &size) != 0 ||
        size == 0)
        return "";
    std::wstring buf(size + 1, L'\0');
    UINT got = GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, &buf[0], &size);
    if (got == (UINT)-1) return "";
    buf.resize(wcslen(buf.c_str()));
    return narrow(buf);
}

// Ask the device itself for its product string; fall back to the driver
// description Windows recorded in the registry.
std::string friendlyName(const std::string& path) {
    if (path.empty()) return "";

    using GetProductStringFn = BOOLEAN(WINAPI*)(HANDLE, PVOID, ULONG);
    static GetProductStringFn getProductString = [] {
        HMODULE m = LoadLibraryW(L"hid.dll");
        return m ? (GetProductStringFn)(void*)GetProcAddress(
                       m, "HidD_GetProductString")
                 : nullptr;
    }();

    if (getProductString) {
        // Query-only access; sharing everything so we never disturb the device.
        HANDLE h = CreateFileW(widen(path).c_str(), 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            wchar_t buf[256] = {0};
            BOOLEAN ok = getProductString(h, buf, (ULONG)sizeof(buf));
            CloseHandle(h);
            if (ok) {
                std::string n = trim(narrow(buf));
                if (!n.empty()) return n;
            }
        }
    }

    // \\?\HID#VID_x&PID_y#inst#{guid}  ->  HID\VID_x&PID_y\inst
    if (path.size() < 5) return "";
    std::string inst = path.substr(4);
    size_t brace = inst.find("#{");
    if (brace != std::string::npos) inst = inst.substr(0, brace);
    inst = replaceAll(inst, "#", "\\");

    HKEY key = nullptr;
    std::wstring sub = widen("SYSTEM\\CurrentControlSet\\Enum\\" + inst);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ, &key) !=
        ERROR_SUCCESS)
        return "";
    wchar_t val[512];
    // Registry strings are not required to be null-terminated, so ask for one
    // character less than the buffer holds and terminate it ourselves.
    DWORD sz = sizeof(val) - sizeof(wchar_t), type = 0;
    std::string out;
    if (RegQueryValueExW(key, L"DeviceDesc", nullptr, &type, (LPBYTE)val, &sz) ==
            ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        val[sz / sizeof(wchar_t)] = L'\0';
        out = narrow(val);
        // "@hidkbd.inf,%std_kbd%;HID Keyboard Device" -> after the last ';'
        size_t semi = out.rfind(';');
        if (semi != std::string::npos) out = out.substr(semi + 1);
        out = trim(out);
    }
    RegCloseKey(key);
    return out;
}

void parseVidPid(const std::string& path, std::string& vid, std::string& pid) {
    static const std::regex re("VID_([0-9A-Fa-f]{4})&PID_([0-9A-Fa-f]{4})",
                               std::regex::optimize);
    std::smatch m;
    if (std::regex_search(path, m, re)) {
        vid = m[1].str();
        pid = m[2].str();
        for (auto& c : vid) c = (char)toupper((unsigned char)c);
        for (auto& c : pid) c = (char)toupper((unsigned char)c);
    }
}

}  // namespace

std::string InputDevice::hardwareId() const {
    if (!vid.empty()) return "VID_" + vid + "&PID_" + pid;
    if (!path.empty())
        return path.size() > 40 ? path.substr(path.size() - 40) : path;
    return "unknown";
}

std::string InputDevice::display() const {
    if (!name.empty()) return name + "  [" + hardwareId() + "]";
    return hardwareId();
}

bool isMouseVk(int vk) {
    switch (vk) {
        case VK_LBUTTON:
        case VK_RBUTTON:
        case VK_MBUTTON:
        case VK_XBUTTON1:
        case VK_XBUTTON2:
        case kVkWheelUp:
        case kVkWheelDown:
        case kVkWheelLeft:
        case kVkWheelRight:
            return true;
        default:
            return vk >= kVkMouse6 && vk <= kVkMouseExtraLast;
    }
}

int vkForHidButton(int n) {
    switch (n) {
        case 1: return VK_LBUTTON;
        case 2: return VK_RBUTTON;
        case 3: return VK_MBUTTON;
        case 4: return VK_XBUTTON1;
        case 5: return VK_XBUTTON2;
        default: break;
    }
    if (n < 6 || n > 5 + kMaxExtraButtons) return 0;
    return kVkMouse6 + (n - 6);
}

bool isPrimaryButtonVk(int vk) {
    // Windows lets the buttons be swapped, and the virtual key follows the
    // swap: with the buttons reversed the physical right button is the one
    // that arrives as VK_LBUTTON. Asking the system rather than assuming is
    // the difference between protecting the button the user actually clicks
    // with and protecting the other one.
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    return vk == (swapped ? VK_RBUTTON : VK_LBUTTON);
}

std::vector<InputDevice> listDevices(DeviceKind kind) {
    std::vector<InputDevice> out;
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count,
                              sizeof(RAWINPUTDEVICELIST)) == (UINT)-1 ||
        count == 0)
        return out;
    std::vector<RAWINPUTDEVICELIST> list(count);
    UINT got = GetRawInputDeviceList(list.data(), &count,
                                     sizeof(RAWINPUTDEVICELIST));
    if (got == (UINT)-1) return out;
    list.resize(got);
    const DWORD want =
        kind == DeviceKind::Mouse ? RIM_TYPEMOUSE : RIM_TYPEKEYBOARD;
    for (const auto& d : list) {
        if (d.dwType != want) continue;
        InputDevice dev;
        dev.kind = kind;
        dev.handle = d.hDevice;
        dev.path = devicePathFor(d.hDevice);
        if (dev.path.empty()) continue;
        parseVidPid(dev.path, dev.vid, dev.pid);
        dev.name = friendlyName(dev.path);
        out.push_back(std::move(dev));
    }
    return out;
}

std::vector<InputDevice> listKeyboards() {
    return listDevices(DeviceKind::Keyboard);
}

std::vector<InputDevice> listMice() { return listDevices(DeviceKind::Mouse); }

// --------------------------------------------------------------- listener
namespace {
const wchar_t* kRawWndClass = L"MacroDeckRawInput";
}

RawInputListener::~RawInputListener() { stop(); }

LRESULT CALLBACK RawInputListener::wndProc(HWND hwnd, UINT msg, WPARAM w,
                                              LPARAM l) {
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)l;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, w, l);
    }
    auto* self =
        (RawInputListener*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (self && msg == WM_INPUT) {
        self->onRawInput(l);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

void RawInputListener::onRawInput(LPARAM lparam) {
    UINT size = 0;
    if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER)) != 0 || size == 0)
        return;
    std::vector<BYTE> buf(size);
    if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, buf.data(), &size,
                        sizeof(RAWINPUTHEADER)) != size)
        return;
    auto* raw = (RAWINPUT*)buf.data();
    if (raw->header.dwType != RIM_TYPEKEYBOARD &&
        raw->header.dwType != RIM_TYPEMOUSE &&
        raw->header.dwType != RIM_TYPEHID)
        return;

    // resolve (and cache) the device path off the handle
    std::string path;
    {
        auto it = pathCache_.find((void*)raw->header.hDevice);
        if (it == pathCache_.end()) {
            std::string p = devicePathFor(raw->header.hDevice);
            it = pathCache_.emplace((void*)raw->header.hDevice, p).first;
        }
        path = it->second;
    }

    if (raw->header.dwType == RIM_TYPEMOUSE) {
        emitMouse(raw, path);
        return;
    }

    if (raw->header.dwType == RIM_TYPEHID) {
        emitHidButtons(raw, path);
        return;
    }

    const RAWKEYBOARD& kb = raw->data.keyboard;
    if (kb.VKey == 0 || kb.VKey == 0xFF) return;  // fake / overrun keys

    KeyEvent ev;
    ev.vk = (int)kb.VKey;
    ev.down = (kb.Flags & RI_KEY_BREAK) == 0;
    ev.t = nowSeconds();
    ev.devicePath = path;
    push(ev);
}

void RawInputListener::push(const KeyEvent& ev) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        recent_.push_back(ev);
        if (recent_.size() > 64) recent_.pop_front();
    }
    cv_.notify_all();

    if (cb_) cb_(ev);
}

void RawInputListener::emitMouse(const RAWINPUT* raw, const std::string& path) {
    const RAWMOUSE& m = raw->data.mouse;
    const USHORT flags = m.usButtonFlags;
    // Movement carries no button flags at all, and a pointer being moved is
    // the overwhelming majority of what a mouse sends. Leaving early here is
    // what keeps a macro mouse from costing anything while it is just being
    // used as a mouse.
    if (flags == 0) return;

    const double t = nowSeconds();
    auto emit = [&](int vk, bool down) {
        KeyEvent ev;
        ev.vk = vk;
        ev.down = down;
        ev.t = t;
        ev.devicePath = path;
        push(ev);
    };

    struct ButtonFlag { USHORT downFlag, upFlag; int vk; };
    static const ButtonFlag kButtons[] = {
        {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, VK_LBUTTON},
        {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, VK_RBUTTON},
        {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, VK_MBUTTON},
        {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, VK_XBUTTON1},
        {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, VK_XBUTTON2},
    };
    for (const ButtonFlag& b : kButtons) {
        if (flags & b.downFlag) emit(b.vk, true);
        if (flags & b.upFlag) emit(b.vk, false);
    }

    // A wheel notch is a click, not something that can be held: there is no
    // release coming, so both halves are reported together. Without the up,
    // the auto-repeat guard upstream would treat the next notch as a repeat
    // of a key still held and swallow it.
    auto wheel = [&](int positive, int negative) {
        const short delta = (short)m.usButtonData;
        if (delta == 0) return;
        const int vk = delta > 0 ? positive : negative;
        emit(vk, true);
        emit(vk, false);
    };
    if (flags & RI_MOUSE_WHEEL) wheel(kVkWheelUp, kVkWheelDown);
    if (flags & RI_MOUSE_HWHEEL) wheel(kVkWheelRight, kVkWheelLeft);
}

void RawInputListener::emitHidButtons(const RAWINPUT* raw,
                                     const std::string& path) {
    // The report descriptor is what turns bytes into button numbers, and it
    // never changes for a device, so Windows hands it over on request.
    UINT ppSize = 0;
    if (GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_PREPARSEDDATA, nullptr,
                               &ppSize) != 0 ||
        ppSize == 0)
        return;
    std::vector<BYTE> pp(ppSize);
    if (GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_PREPARSEDDATA,
                               pp.data(), &ppSize) == (UINT)-1)
        return;
    auto* preparsed = (PHIDP_PREPARSED_DATA)pp.data();

    HIDP_CAPS caps{};
    if (HidP_GetCaps(preparsed, &caps) != HIDP_STATUS_SUCCESS) return;
    if (caps.NumberInputButtonCaps == 0) return;

    std::vector<HIDP_BUTTON_CAPS> btnCaps(caps.NumberInputButtonCaps);
    USHORT btnCount = caps.NumberInputButtonCaps;
    if (HidP_GetButtonCaps(HidP_Input, btnCaps.data(), &btnCount, preparsed) !=
        HIDP_STATUS_SUCCESS)
        return;

    // Only the Button usage page is a button in the sense meant here. A mouse
    // that also reports, say, consumer-control usages is not offering those as
    // numbered buttons and pretending otherwise would invent keys nobody has.
    std::vector<USHORT> nowDown;
    for (USHORT i = 0; i < btnCount; ++i) {
        if (btnCaps[i].UsagePage != HID_USAGE_PAGE_BUTTON) continue;
        ULONG count = HidP_MaxUsageListLength(HidP_Input,
                                              HID_USAGE_PAGE_BUTTON, preparsed);
        if (count == 0) continue;
        std::vector<USAGE> usages(count);
        if (HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, usages.data(),
                           &count, preparsed,
                           (PCHAR)raw->data.hid.bRawData,
                           raw->data.hid.dwSizeHid) != HIDP_STATUS_SUCCESS)
            continue;
        usages.resize(count);
        for (USAGE u : usages) nowDown.push_back((USHORT)u);
        break;   // one Button page per device is all this needs
    }

    std::vector<USHORT> was;
    {
        auto it = hidDown_.find((void*)raw->header.hDevice);
        if (it != hidDown_.end()) was = it->second;
    }
    hidDown_[(void*)raw->header.hDevice] = nowDown;

    auto has = [](const std::vector<USHORT>& v, USHORT u) {
        for (USHORT x : v)
            if (x == u) return true;
        return false;
    };

    const double t = nowSeconds();
    auto emit = [&](USHORT usage, bool down) {
        // Buttons 1-5 already arrive through the ordinary mouse packet.
        // Reporting them a second time here would fire every macro twice.
        if (usage <= 5) return;
        const int vk = vkForHidButton((int)usage);
        if (!vk) return;
        KeyEvent ev;
        ev.vk = vk;
        ev.down = down;
        ev.t = t;
        ev.devicePath = path;
        push(ev);
    };
    for (USHORT u : nowDown)
        if (!has(was, u)) emit(u, true);
    for (USHORT u : was)
        if (!has(nowDown, u)) emit(u, false);
}

std::string RawInputListener::findDevice(int vk, bool down, int timeoutMs) {
    std::unique_lock<std::mutex> lk(mu_);
    const double deadline = nowSeconds() + timeoutMs / 1000.0;
    while (true) {
        const double now = nowSeconds();
        for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
            if (now - it->t > 0.25) break;  // older than this can't be ours
            if (!it->consumedByHook && it->vk == vk && it->down == down) {
                it->consumedByHook = true;
                return it->devicePath;
            }
        }
        double remain = deadline - now;
        if (remain <= 0) return "";
        cv_.wait_for(lk, std::chrono::duration<double>(remain));
    }
}

void RawInputListener::start(Callback cb, bool mice) {
    if (running_.load()) return;
    cb_ = std::move(cb);
    wantMice_ = mice;
    startError_.clear();
    readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    running_.store(true);
    thread_ = std::thread([this] { threadMain(); });
    WaitForSingleObject(readyEvent_, 5000);
    CloseHandle(readyEvent_);
    readyEvent_ = nullptr;
    if (!hwnd_) {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
        throw std::runtime_error(startError_.empty()
                                     ? "raw input listener failed to start"
                                     : startError_);
    }
}

void RawInputListener::threadMain() {
    threadId_ = GetCurrentThreadId();

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &RawInputListener::wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kRawWndClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    // A message-only window (HWND_MESSAGE) does NOT receive WM_INPUT, so this
    // is a real - but never shown - top-level window.
    hwnd_ = CreateWindowExW(0, kRawWndClass, L"MacroDeck", 0, 0, 0, 0, 0,
                            nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        startError_ = "CreateWindow failed: " + lastErrorMessage();
        if (readyEvent_) SetEvent(readyEvent_);
        return;
    }

    RAWINPUTDEVICE rid[3]{};
    rid[0].usUsagePage = 0x01;           // generic desktop
    rid[0].usUsage = 0x06;               // keyboard
    rid[0].dwFlags = RIDEV_INPUTSINK;    // keep receiving when not focused
    rid[0].hwndTarget = hwnd_;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x02;               // mouse - a precision trackpad too
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[1].hwndTarget = hwnd_;
    // The side panel on a multi-button mouse is usually a second collection
    // on the same device, reported as a generic gamepad-shaped HID rather
    // than as more mouse buttons - Windows' mouse packet stops at five. This
    // is what makes buttons 6 and up reachable at all.
    rid[2].usUsagePage = 0x01;
    rid[2].usUsage = 0x05;               // game pad: where vendors put them
    rid[2].dwFlags = RIDEV_INPUTSINK;
    rid[2].hwndTarget = hwnd_;
    const UINT ridCount = wantMice_ ? 3u : 1u;
    if (!RegisterRawInputDevices(rid, ridCount, sizeof(rid[0]))) {
        startError_ = "RegisterRawInputDevices failed: " + lastErrorMessage();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        if (readyEvent_) SetEvent(readyEvent_);
        return;
    }

    if (readyEvent_) SetEvent(readyEvent_);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Unregister so the device stops feeding a dead window.
    RAWINPUTDEVICE off[3]{};
    off[0].usUsagePage = 0x01;
    off[0].usUsage = 0x06;
    off[0].dwFlags = RIDEV_REMOVE;
    off[0].hwndTarget = nullptr;
    off[1].usUsagePage = 0x01;
    off[1].usUsage = 0x02;
    off[1].dwFlags = RIDEV_REMOVE;
    off[1].hwndTarget = nullptr;
    off[2].usUsagePage = 0x01;
    off[2].usUsage = 0x05;
    off[2].dwFlags = RIDEV_REMOVE;
    off[2].hwndTarget = nullptr;
    RegisterRawInputDevices(off, wantMice_ ? 3u : 1u, sizeof(off[0]));
    hwnd_ = nullptr;
    running_.store(false);
}

void RawInputListener::stop() {
    if (!running_.load()) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    // wake anyone blocked in findDevice so the hook thread can exit too
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    cb_ = nullptr;
}

// ------------------------------------------------------------------- hook
namespace {
BlockingHook* g_hookInstance = nullptr;   // WH_KEYBOARD_LL has no user data
std::mutex g_hookInstanceMu;

struct HookState {
    RawInputListener* listener = nullptr;
    BlockingHook::ShouldBlock pred;
    BlockingHook::CouldBlock couldBlock;
    int timeoutMs = 30;
};
HookState g_hookState;
}  // namespace

BlockingHook::~BlockingHook() { stop(); }

LRESULT CALLBACK BlockingHook::hookProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION &&
        (w == WM_KEYDOWN || w == WM_KEYUP || w == WM_SYSKEYDOWN ||
         w == WM_SYSKEYUP)) {
        auto* info = (KBDLLHOOKSTRUCT*)l;
        // Our own SendInput output is flagged injected - never block it, so a
        // macro can't swallow or re-trigger itself.
        if (!(info->flags & LLKHF_INJECTED)) {
            const bool down = (w == WM_KEYDOWN || w == WM_SYSKEYDOWN);
            HookState state;
            {
                std::lock_guard<std::mutex> lk(g_hookInstanceMu);
                state = g_hookState;
            }
            // WH_KEYBOARD_LL is global and synchronous: every keystroke on
            // every keyboard in the machine comes through here, and the app
            // the user is typing into does not see it until this returns.
            // findDevice below waits for the matching raw-input event so the
            // keystroke can be traced to a physical keyboard, and that wait is
            // the whole cost. Paying it on keys nothing could ever block -
            // which on a main keyboard with a handful of macros is very nearly
            // every key typed all day - is what makes typing feel laggy and
            // drops keystrokes outright when Windows loses patience with a
            // slow hook.
            //
            // So: ask the cheap question first, and let ordinary typing past
            // without waiting for anything.
            if (state.couldBlock && !state.couldBlock((int)info->vkCode))
                return CallNextHookEx(nullptr, code, w, l);

            if (state.listener && state.pred) {
                std::string path = state.listener->findDevice(
                    (int)info->vkCode, down, state.timeoutMs);
                // If correlation fails we deliberately pass the key through:
                // a dropped keystroke is far worse than an unblocked one.
                if (!path.empty() && state.pred(path, (int)info->vkCode))
                    return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

LRESULT CALLBACK BlockingHook::mouseProc(int code, WPARAM w, LPARAM l) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, w, l);

    // Movement is the overwhelming majority of what this hook sees, and none
    // of it can ever be blocked. Deciding that with a switch and no lock, no
    // allocation and no correlation wait is the whole reason a macro mouse
    // does not make the pointer feel heavy.
    int vk = 0;
    bool down = false;
    auto* info = (MSLLHOOKSTRUCT*)l;
    switch (w) {
        case WM_LBUTTONDOWN: vk = VK_LBUTTON; down = true; break;
        case WM_LBUTTONUP:   vk = VK_LBUTTON; break;
        case WM_RBUTTONDOWN: vk = VK_RBUTTON; down = true; break;
        case WM_RBUTTONUP:   vk = VK_RBUTTON; break;
        case WM_MBUTTONDOWN: vk = VK_MBUTTON; down = true; break;
        case WM_MBUTTONUP:   vk = VK_MBUTTON; break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            vk = HIWORD(info->mouseData) == XBUTTON2 ? VK_XBUTTON2
                                                     : VK_XBUTTON1;
            down = (w == WM_XBUTTONDOWN);
            break;
        case WM_MOUSEWHEEL:
            vk = (short)HIWORD(info->mouseData) > 0 ? kVkWheelUp
                                                    : kVkWheelDown;
            down = true;
            break;
        case WM_MOUSEHWHEEL:
            vk = (short)HIWORD(info->mouseData) > 0 ? kVkWheelRight
                                                    : kVkWheelLeft;
            down = true;
            break;
        default:
            return CallNextHookEx(nullptr, code, w, l);
    }

    // Our own SendInput output is flagged injected - never block it, so a
    // macro that clicks cannot swallow or re-trigger itself.
    if (info->flags & LLMHF_INJECTED)
        return CallNextHookEx(nullptr, code, w, l);

    HookState state;
    {
        std::lock_guard<std::mutex> lk(g_hookInstanceMu);
        state = g_hookState;
    }
    if (state.couldBlock && !state.couldBlock(vk))
        return CallNextHookEx(nullptr, code, w, l);

    if (state.listener && state.pred) {
        std::string path = state.listener->findDevice(vk, down,
                                                      state.timeoutMs);
        // Same bargain as the keyboard: a click that fails to correlate is
        // passed through, because a pointer that stops working is far worse
        // than a button that also did its ordinary job.
        if (!path.empty() && state.pred(path, vk)) return 1;
    }
    return CallNextHookEx(nullptr, code, w, l);
}

void BlockingHook::start(RawInputListener* listener, ShouldBlock pred,
                         CouldBlock couldBlock, int correlateTimeoutMs,
                         bool mice) {
    if (running_.load()) return;
    listener_ = listener;
    wantMice_ = mice;
    pred_ = std::move(pred);
    couldBlock_ = std::move(couldBlock);
    timeoutMs_ = correlateTimeoutMs;
    {
        std::lock_guard<std::mutex> lk(g_hookInstanceMu);
        g_hookState.listener = listener_;
        g_hookState.pred = pred_;
        g_hookState.couldBlock = couldBlock_;
        g_hookState.timeoutMs = timeoutMs_;
        g_hookInstance = this;
    }
    readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    running_.store(true);
    thread_ = std::thread([this] { threadMain(); });
    WaitForSingleObject(readyEvent_, 5000);
    CloseHandle(readyEvent_);
    readyEvent_ = nullptr;
    if (!hook_) {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
        throw std::runtime_error("could not install the keyboard hook: " +
                                 lastErrorMessage());
    }
}

void BlockingHook::threadMain() {
    threadId_ = GetCurrentThreadId();
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &BlockingHook::hookProc,
                              GetModuleHandleW(nullptr), 0);
    if (wantMice_ && hook_)
        mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, &BlockingHook::mouseProc,
                                       GetModuleHandleW(nullptr), 0);
    if (readyEvent_) SetEvent(readyEvent_);
    if (!hook_) return;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
    running_.store(false);
}

void BlockingHook::stop() {
    if (!running_.load()) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    if (threadId_) PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    {
        std::lock_guard<std::mutex> lk(g_hookInstanceMu);
        if (g_hookInstance == this) {
            g_hookInstance = nullptr;
            g_hookState = HookState{};
        }
    }
}

}  // namespace dev
