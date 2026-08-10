#include "device.h"
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

std::string KeyboardDevice::hardwareId() const {
    if (!vid.empty()) return "VID_" + vid + "&PID_" + pid;
    if (!path.empty())
        return path.size() > 40 ? path.substr(path.size() - 40) : path;
    return "unknown";
}

std::string KeyboardDevice::display() const {
    if (!name.empty()) return name + "  [" + hardwareId() + "]";
    return hardwareId();
}

std::vector<KeyboardDevice> listKeyboards() {
    std::vector<KeyboardDevice> out;
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
    for (const auto& d : list) {
        if (d.dwType != RIM_TYPEKEYBOARD) continue;
        KeyboardDevice kb;
        kb.handle = d.hDevice;
        kb.path = devicePathFor(d.hDevice);
        if (kb.path.empty()) continue;
        parseVidPid(kb.path, kb.vid, kb.pid);
        kb.name = friendlyName(kb.path);
        out.push_back(std::move(kb));
    }
    return out;
}

// --------------------------------------------------------------- listener
namespace {
const wchar_t* kRawWndClass = L"MacroDeckRawInput";
}

RawKeyboardListener::~RawKeyboardListener() { stop(); }

LRESULT CALLBACK RawKeyboardListener::wndProc(HWND hwnd, UINT msg, WPARAM w,
                                              LPARAM l) {
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)l;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, w, l);
    }
    auto* self =
        (RawKeyboardListener*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
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

void RawKeyboardListener::onRawInput(LPARAM lparam) {
    UINT size = 0;
    if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER)) != 0 || size == 0)
        return;
    std::vector<BYTE> buf(size);
    if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, buf.data(), &size,
                        sizeof(RAWINPUTHEADER)) != size)
        return;
    auto* raw = (RAWINPUT*)buf.data();
    if (raw->header.dwType != RIM_TYPEKEYBOARD) return;

    const RAWKEYBOARD& kb = raw->data.keyboard;
    if (kb.VKey == 0 || kb.VKey == 0xFF) return;  // fake / overrun keys

    KeyEvent ev;
    ev.vk = (int)kb.VKey;
    ev.down = (kb.Flags & RI_KEY_BREAK) == 0;
    ev.t = nowSeconds();
    {
        // resolve (and cache) the device path off the handle
        auto it = pathCache_.find((void*)raw->header.hDevice);
        if (it == pathCache_.end()) {
            std::string p = devicePathFor(raw->header.hDevice);
            it = pathCache_.emplace((void*)raw->header.hDevice, p).first;
        }
        ev.devicePath = it->second;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        recent_.push_back(ev);
        if (recent_.size() > 64) recent_.pop_front();
    }
    cv_.notify_all();

    if (cb_) cb_(ev);
}

std::string RawKeyboardListener::findDevice(int vk, bool down, int timeoutMs) {
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

void RawKeyboardListener::start(Callback cb) {
    if (running_.load()) return;
    cb_ = std::move(cb);
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

void RawKeyboardListener::threadMain() {
    threadId_ = GetCurrentThreadId();

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &RawKeyboardListener::wndProc;
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

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;           // generic desktop
    rid.usUsage = 0x06;               // keyboard
    rid.dwFlags = RIDEV_INPUTSINK;    // keep receiving when not focused
    rid.hwndTarget = hwnd_;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
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
    RAWINPUTDEVICE off{};
    off.usUsagePage = 0x01;
    off.usUsage = 0x06;
    off.dwFlags = RIDEV_REMOVE;
    off.hwndTarget = nullptr;
    RegisterRawInputDevices(&off, 1, sizeof(off));
    hwnd_ = nullptr;
    running_.store(false);
}

void RawKeyboardListener::stop() {
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
    RawKeyboardListener* listener = nullptr;
    BlockingHook::ShouldBlock pred;
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
            if (state.listener && state.pred) {
                std::string path = state.listener->findDevice(
                    (int)info->vkCode, down, state.timeoutMs);
                // If correlation fails we deliberately pass the key through:
                // a dropped keystroke is far worse than an unblocked one.
                if (!path.empty() && state.pred(path)) return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

void BlockingHook::start(RawKeyboardListener* listener, ShouldBlock pred,
                         int correlateTimeoutMs) {
    if (running_.load()) return;
    listener_ = listener;
    pred_ = std::move(pred);
    timeoutMs_ = correlateTimeoutMs;
    {
        std::lock_guard<std::mutex> lk(g_hookInstanceMu);
        g_hookState.listener = listener_;
        g_hookState.pred = pred_;
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
    if (readyEvent_) SetEvent(readyEvent_);
    if (!hook_) return;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
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
