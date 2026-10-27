// Runs macros: watches per-device key events, matches them against the
// configured decks, and executes action sequences on a worker thread.
#pragma once
#include "config.h"
#include "device.h"
#include "friendly.h"

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

// How a macro run ended, for the status line. Fired on the worker thread once
// per run, after the last step or the one that stopped it.
struct Outcome {
    bool ok = false;
    std::string button;     // the button's name, or the run's label
    std::string alias;
    int step = 0;           // 1-based step that stopped it; 0 when ok
    fe::Problem problem = fe::Problem::None;
    std::string subject;
    std::string technical;
};

// Decides which key-downs are presses. Two kinds are not: a key the keyboard
// is repeating because it is held, and the second half of a press a keypad
// sent twice. Key-ups always pass - a release has to reach whatever is
// waiting for it, such as a page held open.
class PressFilter {
public:
    // True when this key-down should run nothing.
    bool ignore(const dev::KeyEvent& ev);
    void clear();

    // The longest gap between a key's release and a press of the same key
    // that still counts as that one press bouncing.
    static constexpr double kBounceSeconds = 0.04;

private:
    std::mutex mu_;
    // Keyed by (device path, virtual key), so the same key on two pads is two
    // keys.
    std::map<std::pair<std::string, int>, double> lastDown_;
    std::map<std::pair<std::string, int>, double> lastUp_;
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
    using OutcomeFn = std::function<void(const Outcome&)>;

    Engine() = default;
    ~Engine();

    void setLog(LogFn fn);
    void setPageChanger(PageFn fn);
    void setOutcome(OutcomeFn fn);
    // The monitor callback fires for EVERY keystroke on every keyboard, so
    // the UI can show a live key trace. Set to nullptr to turn it off.
    void setMonitor(MonitorFn fn);

    // Take a snapshot of the config. Safe to call while running - the next
    // keystroke uses the new bindings.
    void updateConfig(const cfg::Config& c);

    // Throws fe::Error (InputBlocked) when Windows won't let it read or block
    // input, and std::runtime_error with a sentence for the user otherwise.
    void start();
    void stop();
    bool running() const { return running_.load(); }

    // Run a sequence on the worker thread (used by the Test button and by
    // clicking a software button). `alias` says which deck it belongs to, so
    // a page step in the sequence knows whose pages it is talking about.
    // `title` is the button's name for the status line; empty uses `label`.
    void runSequenceAsync(const std::vector<cfg::Step>& steps,
                          const std::string& label,
                          const std::string& alias = "",
                          const std::string& title = "");
    // Run a sequence on the calling thread. Throws fe::Error on the first
    // failing step, carrying that step's number.
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
        std::string title;   // the button's name, when there is one
    };

    void onKey(const dev::KeyEvent& ev);
    // Resolve a page name or direction against a keyboard. Returns -1 if
    // there is no such page. Takes cfgMu_ itself.
    int resolvePage(const std::string& alias, const std::string& name,
                    const std::string& direction) const;
    void requestPage(const std::string& alias, int page);
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
    OutcomeFn outcome_;
    void report(const Outcome& o);

    // Keys currently holding a page open, and the page each should put back
    // when it is released. Keyed the same way PressFilter keys presses, so two
    // pads can each hold a page of their own without colliding.
    std::mutex holdMu_;
    std::map<std::pair<std::string, int>, std::pair<std::string, int>> holds_;

    // The deck whose macro the worker is running, for "Go to a page". Only
    // touched from the worker thread between picking a job up and finishing
    // it, so it needs no lock of its own.
    std::string runningAlias_;

    std::mutex jobMu_;
    std::condition_variable jobCv_;
    std::deque<Job> jobs_;

    // Which key-downs are real presses, rather than repeats or bounces.
    PressFilter presses_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> listenerActive_{false};

    dev::RawInputListener listener_;
    dev::BlockingHook hook_;
};

}  // namespace eng
