// Runs macros: watches per-device key events, matches them against the
// configured decks, and executes action sequences on a worker thread.
#pragma once
#include "config.h"
#include "device.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace eng {

struct MonitorEvent {
    int vk = 0;
    bool down = false;
    std::string keyName;
    std::string devicePath;
    std::string alias;      // configured keyboard, empty if unmatched
    bool hasMacro = false;
};

class Engine {
public:
    using LogFn = std::function<void(const std::string&)>;
    using MonitorFn = std::function<void(const MonitorEvent&)>;

    Engine() = default;
    ~Engine();

    void setLog(LogFn fn);
    // The monitor callback fires for EVERY keystroke on every keyboard, so
    // the UI can show a live key trace. Set to nullptr to turn it off.
    void setMonitor(MonitorFn fn);

    // Take a snapshot of the config. Safe to call while running - the next
    // keystroke uses the new bindings.
    void updateConfig(const cfg::Config& c);

    void start();          // throws std::runtime_error with a friendly message
    void stop();
    bool running() const { return running_.load(); }

    // Run a sequence on the worker thread (used by the Test button).
    void runSequenceAsync(const std::vector<cfg::Step>& steps,
                          const std::string& label);
    // Run a sequence on the calling thread; throws on the first failing step.
    void runSequence(const std::vector<cfg::Step>& steps);

    // Listen for keystrokes without running macros - used by "press a key"
    // capture in the UI. Returns false if input capture is unavailable.
    bool listening() const { return listenerActive_.load(); }

private:
    struct Job {
        std::vector<cfg::Step> steps;
        std::string label;
    };

    void onKey(const dev::KeyEvent& ev);
    // True when this key-down is the keyboard repeating a key the user is
    // still holding, rather than a fresh press.
    bool isAutoRepeat(const dev::KeyEvent& ev);
    void workerMain();
    void runStep(const cfg::Step& step);
    void log(const std::string& msg);

    // Whatever a "Lock a window in place" step changed, so the matching
    // release step can undo it. Only ever touched from the thread running the
    // sequence.
    struct LockedWindow {
        bool active = false;
        HWND hwnd = nullptr;
        RECT rect{};          // where it was before we moved it
        bool wasMaximized = false;
    };
    LockedWindow locked_;
    bool inputBlocked_ = false;
    // Runs at the end of every sequence, however it ended. Leaving a window
    // in the wrong place is a nuisance; leaving the user's keyboard and mouse
    // blocked is not recoverable without killing the process.
    void releaseInputBlock();

    mutable std::mutex cfgMu_;
    cfg::Config cfg_;

    std::mutex logMu_;
    LogFn log_;
    MonitorFn monitor_;

    std::mutex jobMu_;
    std::condition_variable jobCv_;
    std::deque<Job> jobs_;

    // Last key-down time per (device, virtual key), used to tell a held key's
    // repeats apart from real presses.
    std::mutex heldMu_;
    std::map<std::pair<std::string, int>, double> lastDown_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> listenerActive_{false};

    dev::RawKeyboardListener listener_;
    dev::BlockingHook hook_;
};

}  // namespace eng
