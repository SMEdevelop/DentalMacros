// Per-device keyboard capture (Raw Input) + the low-level hook that blocks
// keys coming from macro keyboards.
//
// Windows splits the two halves of what we need: Raw Input knows which
// physical device a keystroke came from but cannot swallow it; the
// WH_KEYBOARD_LL hook can swallow a keystroke but has no idea which device
// produced it. Running both and correlating them gives us both properties.
#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dev {

struct KeyboardDevice {
    HANDLE handle = nullptr;
    std::string path;    // \\?\HID#VID_046D&PID_C539&MI_00#9&...#{guid}
    std::string vid;     // "046D"
    std::string pid;     // "C539"
    std::string name;    // product name, e.g. "USB Receiver" (may be empty)

    // Stable firmware id - survives reboots, driver updates and port changes.
    std::string hardwareId() const;
    // Label for pickers: "USB Receiver  [VID_046D&PID_C539]"
    std::string display() const;
};

// Enumerate attached keyboards, resolving product names where possible.
std::vector<KeyboardDevice> listKeyboards();

struct KeyEvent {
    int vk = 0;
    bool down = false;
    std::string devicePath;
    double t = 0;             // seconds, monotonic
    bool consumedByHook = false;
};

// ---------------------------------------------------------------- listener
class RawKeyboardListener {
public:
    using Callback = std::function<void(const KeyEvent&)>;

    RawKeyboardListener() = default;
    ~RawKeyboardListener();

    // Throws std::runtime_error if the hidden window can't be created.
    void start(Callback cb);
    void stop();
    bool running() const { return running_.load(); }

    // Called by BlockingHook: wait briefly for the raw event matching a
    // hooked keystroke and return its device path ("" if none arrived).
    std::string findDevice(int vk, bool down, int timeoutMs);

private:
    void threadMain();
    void onRawInput(LPARAM lparam);
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);

    Callback cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    HWND hwnd_ = nullptr;
    DWORD threadId_ = 0;
    HANDLE readyEvent_ = nullptr;
    std::string startError_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<KeyEvent> recent_;
    std::unordered_map<void*, std::string> pathCache_;
};

// ------------------------------------------------------------------- hook
class BlockingHook {
public:
    // Asked once per keystroke: should this key, from this device, be taken
    // away from other programs? The vk is part of the question because a
    // keyboard may be configured to give up only the keys that run macros
    // and keep typing with the rest.
    using ShouldBlock =
        std::function<bool(const std::string& devicePath, int vk)>;
    // Asked first, on the hook thread, for every keystroke on every keyboard
    // in the machine. It must be cheap and it must not block: saying no here
    // is what keeps ordinary typing out of the correlation wait below.
    using CouldBlock = std::function<bool(int vk)>;

    BlockingHook() = default;
    ~BlockingHook();

    void start(RawKeyboardListener* listener, ShouldBlock pred,
               CouldBlock couldBlock = nullptr, int correlateTimeoutMs = 30);
    void stop();
    bool running() const { return running_.load(); }

private:
    void threadMain();
    static LRESULT CALLBACK hookProc(int code, WPARAM w, LPARAM l);

    RawKeyboardListener* listener_ = nullptr;
    ShouldBlock pred_;
    CouldBlock couldBlock_;
    int timeoutMs_ = 30;
    std::thread thread_;
    std::atomic<bool> running_{false};
    HHOOK hook_ = nullptr;
    DWORD threadId_ = 0;
    HANDLE readyEvent_ = nullptr;
};

double nowSeconds();

}  // namespace dev
