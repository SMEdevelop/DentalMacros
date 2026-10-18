// Per-device input capture (Raw Input) + the low-level hooks that block
// events coming from macro devices.
//
// Windows splits the two halves of what we need: Raw Input knows which
// physical device an event came from but cannot swallow it; the
// WH_KEYBOARD_LL and WH_MOUSE_LL hooks can swallow an event but have no idea
// which device produced it. Running both and correlating them gives us both
// properties.
//
// Mice ride the same machinery as keyboards rather than getting a parallel
// one, because the only real difference is the usage page they are registered
// under. A mouse button already has a virtual-key code of its own that no
// keyboard ever produces (VK_LBUTTON and friends), and the wheel is given one
// out of the unassigned range - so a "key" is the same thing to everything
// above this file whether it came from a keyboard, a mouse or a trackpad.
#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dev {

enum class DeviceKind { Keyboard, Mouse };

struct InputDevice {
    DeviceKind kind = DeviceKind::Keyboard;
    HANDLE handle = nullptr;
    std::string path;    // \\?\HID#VID_046D&PID_C539&MI_00#9&...#{guid}
    std::string vid;     // "046D"
    std::string pid;     // "C539"
    // The vendor and product ids exactly as the path spells them, which is
    // what a match string has to contain: "VID_046D&PID_C539" over USB, but
    // "VID&0203f0_PID&854a" from a Bluetooth keypad, whose vendor id carries
    // a prefix saying which body issued it.
    std::string idMatch;
    // A Bluetooth LE keypad's address, lower-case hex ("d8b663890275"); empty
    // for anything else. See bluetoothAddress().
    std::string btAddress;
    std::string name;    // product name, e.g. "USB Receiver" (may be empty)

    // Stable firmware id - survives reboots, driver updates and port changes.
    std::string hardwareId() const;
    // Label for pickers: "USB Receiver  [VID_046D&PID_C539]"
    std::string display() const;
};

// Enumerate attached devices of one kind, resolving product names where
// possible.
std::vector<InputDevice> listDevices(DeviceKind kind);
std::vector<InputDevice> listKeyboards();
std::vector<InputDevice> listMice();

// ---------------------------------------------------------- mouse as keys
// Buttons already have virtual-key codes Windows reserves and no keyboard
// ever sends, so they need nothing invented. The wheel has none, so it takes
// four codes from the block Microsoft documents as unassigned - which keeps
// every table, cache and 256-entry array above this file working unchanged.
constexpr int kVkWheelUp = 0x88;
constexpr int kVkWheelDown = 0x89;
constexpr int kVkWheelLeft = 0x8A;
constexpr int kVkWheelRight = 0x8B;

// Buttons beyond the five Windows names. Raw Input's mouse packet only ever
// reports five, so anything past that arrives as HID button usages from a
// second collection on the same device - which is how the side panel on a
// multi-button mouse usually presents itself. Codes come from the reserved
// 0xC1-0xCF range, which no key table here uses.
constexpr int kVkMouse6 = 0xC1;
constexpr int kMaxExtraButtons = 15;   // buttons 6..20
constexpr int kVkMouseExtraLast = kVkMouse6 + kMaxExtraButtons - 1;

// True for any virtual key that can only have come from a pointing device.
bool isMouseVk(int vk);
// The virtual key for HID button number `n` (1-based as the HID report counts
// them). Buttons 1-5 map onto the codes Windows already has; 6 and up take one
// of the extra codes. Returns 0 for a button past what can be represented.
int vkForHidButton(int n);
// True for the button Windows treats as "primary" - the one that is usually
// left-click, and the one that must stay clickable on a machine with a single
// pointer or there is no way left to turn Dental Macros off.
bool isPrimaryButtonVk(int vk);

// The virtual key Raw Input reports for a key the low-level hook reports as
// `vk`. The two disagree about exactly three keys: Raw Input says Shift, Ctrl
// and Alt, while the hook says which side. Blocking has to match one against
// the other, and bindings are recorded from Raw Input, so the hook's side is
// the one that gives way.
int rawInputVk(int vk);

// ------------------------------------------------------------- media keys
// Media, volume and launcher keys arrive as HID consumer-control usages
// rather than as keystrokes - and on a Bluetooth keypad, so do many of the
// extra buttons. Windows turns a handful of usages into virtual keys of its
// own (volume, tracks, browser back, calculator...) and those keep that code
// here: a binding on "volumeup" then works however the key arrived, and the
// hook can still block the copy Windows makes. Every other usage has no
// virtual key at all and is numbered above the 0-255 range, where nothing
// Windows sends can collide with it. Those can be pressed and bound, but a
// macro cannot type one, and there is no keystroke of Windows' to block.
constexpr int kVkConsumerBase = 0x10000;
// The key code for consumer usage `usage`; 0 for usage 0, which is a report
// saying nothing is pressed.
int vkForConsumerUsage(int usage);

// ------------------------------------------------------ Bluetooth keypads
// A Bluetooth LE keypad's address, lower-case, out of any of its raw-input
// paths; "" for a path that is not Bluetooth LE. A keypad presents several
// inputs - keys, media keys, a mouse, a dial - each with a path of its own,
// and the address is the one part those paths all share.
std::string bluetoothAddress(const std::string& path);
// True when two raw-input paths are inputs of the same physical device, as
// far as can be told from the paths: the same path, or the same Bluetooth
// keypad.
bool sameDevice(const std::string& a, const std::string& b);
// Fill in vid, pid, idMatch and btAddress from a device path.
void parseDevicePath(const std::string& path, InputDevice& out);

struct KeyEvent {
    int vk = 0;
    bool down = false;
    std::string devicePath;
    double t = 0;             // seconds, monotonic
    bool consumedByHook = false;
};

// ---------------------------------------------------------------- listener
class RawInputListener {
public:
    using Callback = std::function<void(const KeyEvent&)>;

    RawInputListener() = default;
    ~RawInputListener();

    // Throws std::runtime_error if the hidden window can't be created.
    // `mice` also registers for pointer input; leave it off and this behaves
    // exactly as it always did, which is what a setup with no macro mouse
    // should cost.
    void start(Callback cb, bool mice = false);
    void stop();
    bool running() const { return running_.load(); }

    // Called by BlockingHook: wait briefly for the raw event matching a
    // hooked keystroke and return its device path ("" if none arrived).
    std::string findDevice(int vk, bool down, int timeoutMs);

private:
    void threadMain();
    void onRawInput(LPARAM lparam);
    // Turn one raw mouse packet into the same shape a keystroke has. A wheel
    // notch has no release of its own, so it reports down and up together:
    // it is a click, not something that can be held.
    void emitMouse(const RAWINPUT* raw, const std::string& path);
    // Everything that arrives as a HID report rather than as a keystroke or a
    // mouse packet: media keys from a consumer-control input, and buttons 6
    // and up from a mouse's side panel (Windows' mouse packet stops at five).
    // Reports what changed since that input's last report.
    void emitHidButtons(const RAWINPUT* raw, const std::string& path);
    void push(const KeyEvent& ev);
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);

    Callback cb_;
    bool wantMice_ = false;
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
    // A HID report says which buttons are down, not which changed, so the
    // previous set has to be remembered to tell a press from a button that
    // was already held. Per device and per report id: a device that splits
    // its buttons across several reports only mentions some of them in each.
    // Stored as USHORT rather than USAGE so this header stays free of
    // hidusage.h, which nothing else here needs.
    std::map<std::pair<void*, int>, std::vector<USHORT>> hidDown_;
    // Each device's report descriptor, fetched on its first report. It never
    // changes for a device, and asking again on every media key is waste.
    std::unordered_map<void*, std::vector<BYTE>> preparsed_;
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

    // `mice` additionally installs WH_MOUSE_LL. Off unless a mouse is
    // actually configured to block: that hook sees every pointer movement in
    // the machine, and installing one nobody needs is not free.
    void start(RawInputListener* listener, ShouldBlock pred,
               CouldBlock couldBlock = nullptr, int correlateTimeoutMs = 30,
               bool mice = false);
    void stop();
    bool running() const { return running_.load(); }

private:
    void threadMain();
    static LRESULT CALLBACK hookProc(int code, WPARAM w, LPARAM l);
    static LRESULT CALLBACK mouseProc(int code, WPARAM w, LPARAM l);

    RawInputListener* listener_ = nullptr;
    ShouldBlock pred_;
    CouldBlock couldBlock_;
    int timeoutMs_ = 30;
    std::thread thread_;
    std::atomic<bool> running_{false};
    HHOOK hook_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    bool wantMice_ = false;
    DWORD threadId_ = 0;
    HANDLE readyEvent_ = nullptr;
};

double nowSeconds();

}  // namespace dev
