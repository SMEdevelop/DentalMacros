// Runs macros: watches per-device key events, matches them against the
// configured decks, and executes action sequences on a worker thread.
#pragma once
#include "config.h"
#include "device.h"

#include <array>
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
    // Told when a macro asks for a different page of a deck. The app owns
    // which page is in front - it has to, because software buttons still
    // change page with the engine stopped - so the engine only ever asks,
    // and sees the answer when the app hands back a fresh snapshot.
    using PageFn = std::function<void(const std::string& alias, int page)>;

    Engine() = default;
    ~Engine();

    void setLog(LogFn fn);
    void setPageChanger(PageFn fn);
    // The monitor callback fires for EVERY keystroke on every keyboard, so
    // the UI can show a live key trace. Set to nullptr to turn it off.
    void setMonitor(MonitorFn fn);

    // Take a snapshot of the config. Safe to call while running - the next
    // keystroke uses the new bindings.
    void updateConfig(const cfg::Config& c);

    void start();          // throws std::runtime_error with a friendly message
    void stop();
    bool running() const { return running_.load(); }

    // Run a sequence on the worker thread (used by the Test button and by
    // clicking a software button). `alias` says which deck it belongs to, so
    // a page step in the sequence knows whose pages it is talking about.
    void runSequenceAsync(const std::vector<cfg::Step>& steps,
                          const std::string& label,
                          const std::string& alias = "");
    // Run a sequence on the calling thread; throws on the first failing step.
    void runSequence(const std::vector<cfg::Step>& steps);

    // Listen for keystrokes without running macros - used by "press a key"
    // capture in the UI. Returns false if input capture is unavailable.
    bool listening() const { return listenerActive_.load(); }

private:
    struct Job {
        std::vector<cfg::Step> steps;
        std::string label;
        // Which deck fired it, so a "Go to a page" step knows whose pages it
        // is talking about. Empty for a Test run from the editor.
        std::string alias;
    };

    void onKey(const dev::KeyEvent& ev);
    // Resolve a page name or direction against a keyboard. Returns -1 if
    // there is no such page. Takes cfgMu_ itself.
    int resolvePage(const std::string& alias, const std::string& name,
                    const std::string& direction) const;
    void requestPage(const std::string& alias, int page);
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

    // Which virtual keys any configured keyboard could take from another
    // program. The hook reads this for every keystroke in the machine, so it
    // is a flat lookup with no lock: taking cfgMu_ there would put the whole
    // system's typing behind whatever else happens to hold it.
    std::array<std::atomic<bool>, 256> couldBlock_{};

    std::mutex logMu_;
    LogFn log_;
    MonitorFn monitor_;
    PageFn page_;

    // Keys currently holding a page open, and the page each should put back
    // when it is released. Keyed the same way lastDown_ is, so two pads can
    // each hold a page of their own without colliding.
    std::mutex holdMu_;
    std::map<std::pair<std::string, int>, std::pair<std::string, int>> holds_;

    // The deck whose macro the worker is running, for "Go to a page". Only
    // touched from the worker thread between picking a job up and finishing
    // it, so it needs no lock of its own.
    std::string runningAlias_;

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
