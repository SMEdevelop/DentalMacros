#include "engine.h"

#include "actions.h"
#include "keys.h"
#include "util.h"
#include "vision.h"
#include "winman.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace eng {

Engine::~Engine() { stop(); }

void Engine::setLog(LogFn fn) {
    std::lock_guard<std::mutex> lk(logMu_);
    log_ = std::move(fn);
}

void Engine::setPageChanger(PageFn fn) {
    std::lock_guard<std::mutex> lk(logMu_);
    page_ = std::move(fn);
}

int Engine::resolvePage(const std::string& alias, const std::string& name,
                        const std::string& direction) const {
    std::lock_guard<std::mutex> lk(cfgMu_);
    const cfg::Keyboard* kb = nullptr;
    for (const auto& k : cfg_.keyboards)
        if (k.alias == alias) { kb = &k; break; }
    if (!kb || kb->pages.empty()) return -1;

    const int n = (int)kb->pages.size();
    const int cur = kb->activePage >= 0 && kb->activePage < n ? kb->activePage
                                                             : kb->defaultPage();
    const std::string dir = lower(trim(direction));
    if (dir == "next") return (cur + 1) % n;
    // Adding n before the modulo: C++ leaves a negative operand negative, and
    // page -1 is not a page.
    if (dir == "previous") return (cur - 1 + n) % n;
    if (dir == "default") return kb->defaultPage();
    return kb->pageByName(name);
}

void Engine::requestPage(const std::string& alias, int page) {
    if (page < 0) return;
    PageFn fn;
    {
        std::lock_guard<std::mutex> lk(logMu_);
        fn = page_;
    }
    if (fn) fn(alias, page);
}

void Engine::setMonitor(MonitorFn fn) {
    std::lock_guard<std::mutex> lk(logMu_);
    monitor_ = std::move(fn);
}

void Engine::log(const std::string& msg) {
    LogFn fn;
    {
        std::lock_guard<std::mutex> lk(logMu_);
        fn = log_;
    }
    if (fn) fn(msg);
}

void Engine::updateConfig(const cfg::Config& c) {
    std::lock_guard<std::mutex> lk(cfgMu_);
    cfg_ = c;
    // Recomputed here, once per edit, rather than per keystroke in the hook.
    for (int vk = 0; vk < 256; ++vk)
        couldBlock_[(size_t)vk].store(cfg_.couldBlockVk(vk),
                                      std::memory_order_relaxed);
}

// ------------------------------------------------------------------ input
// Raw Input carries no "this is a repeat" flag, so holding a macro key would
// otherwise re-run its macro at the keyboard's repeat rate - about thirty
// times a second. Track what is currently down instead.
//
// The gap test is a safety net for the case where a key-up never arrives (the
// pad is unplugged mid-press, say): Windows' slowest repeat delay is one
// second, so anything longer than that is certainly a new press, and the key
// un-sticks itself rather than staying dead until the engine restarts.
bool PressFilter::ignore(const dev::KeyEvent& ev) {
    const auto id = std::make_pair(ev.devicePath, ev.vk);
    std::lock_guard<std::mutex> lk(mu_);
    if (!ev.down) {
        lastDown_.erase(id);
        lastUp_[id] = ev.t;
        return false;
    }
    // Some keypads send one press as two, the second starting within a few
    // milliseconds of the first ending - the Huion Keydial's space bar does it
    // on every press, and would run its macro twice. Nobody lets go of a key
    // and presses it again that fast, so a press that close behind its own
    // release is the same press. Not on the wheel, though: a notch is a whole
    // click with its release sent alongside, and a quick flick is a run of
    // notches well inside this window.
    const bool wheel = ev.vk >= dev::kVkWheelUp && ev.vk <= dev::kVkWheelRight;
    if (!wheel) {
        auto up = lastUp_.find(id);
        if (up != lastUp_.end() && ev.t - up->second < kBounceSeconds)
            return true;
    }
    auto it = lastDown_.find(id);
    const bool repeat = it != lastDown_.end() && (ev.t - it->second) < 1.2;
    lastDown_[id] = ev.t;
    return repeat;
}

void PressFilter::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    lastDown_.clear();
    lastUp_.clear();
}

void Engine::onKey(const dev::KeyEvent& ev) {
    if (presses_.ignore(ev)) return;

    std::string keyName = keys::vkName(ev.vk);

    std::string alias;
    std::vector<cfg::Step> steps;
    bool hasMacro = false;
    // A "Hold for a page" step on this key, resolved while we still have the
    // config in hand: the page to show while it is down, and the one to put
    // back when it comes up.
    std::string holdPage;
    int returnPage = -1;
    {
        std::lock_guard<std::mutex> lk(cfgMu_);
        const cfg::Keyboard* kb = cfg_.keyboardForDevice(ev.devicePath, ev.vk);
        if (kb) {
            alias = kb->alias;
            if (const cfg::Binding* b = kb->findKey(keyName)) {
                hasMacro = !b->steps.empty();
                if (ev.down && hasMacro) steps = b->steps;
                for (const cfg::Step& s : b->steps)
                    if (s.action == "page_hold") {
                        holdPage = s.params.str("page");
                        break;
                    }
            }
            returnPage = kb->activePage;
        }
    }

    // Momentary page switching is answered here rather than on the worker
    // thread, because it has to be undone on key-up: a release that arrived
    // while the press was still queued would put the wrong page back, or
    // none at all.
    if (!alias.empty()) {
        const std::pair<std::string, int> id{ev.devicePath, ev.vk};
        if (ev.down && !holdPage.empty()) {
            bool fresh = false;
            {
                std::lock_guard<std::mutex> lk(holdMu_);
                fresh = holds_.emplace(id, std::make_pair(alias, returnPage))
                            .second;
            }
            if (fresh) requestPage(alias, resolvePage(alias, holdPage, "named"));
        } else if (!ev.down) {
            std::pair<std::string, int> back;
            bool had = false;
            {
                std::lock_guard<std::mutex> lk(holdMu_);
                auto it = holds_.find(id);
                if (it != holds_.end()) {
                    back = it->second;
                    had = true;
                    holds_.erase(it);
                }
            }
            if (had) requestPage(back.first, back.second);
        }
    }

    MonitorFn mon;
    {
        std::lock_guard<std::mutex> lk(logMu_);
        mon = monitor_;
    }
    if (mon) {
        MonitorEvent me;
        me.vk = ev.vk;
        me.down = ev.down;
        me.keyName = keyName;
        me.devicePath = ev.devicePath;
        me.alias = alias;
        me.hasMacro = hasMacro;
        mon(me);
    }

    if (steps.empty()) return;

    Job job;
    job.steps = std::move(steps);
    job.alias = alias;
    job.label = "[" + alias + "] " + keys::displayName(keyName);
    {
        std::lock_guard<std::mutex> lk(jobMu_);
        // Don't let a held-down key pile up hundreds of pending runs.
        if (jobs_.size() > 32) {
            jobs_.pop_front();
            log("Dropping queued macros - they are firing faster than they run.");
        }
        jobs_.push_back(std::move(job));
    }
    jobCv_.notify_one();
}

// ----------------------------------------------------------------- worker
void Engine::workerMain() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(jobMu_);
            jobCv_.wait(lk, [&] { return stopping_.load() || !jobs_.empty(); });
            if (stopping_.load() && jobs_.empty()) return;
            if (jobs_.empty()) continue;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        runningAlias_ = job.alias;
        try {
            log(job.label + " - running " + std::to_string(job.steps.size()) +
                (job.steps.size() == 1 ? " step" : " steps"));
            runSequence(job.steps);
        } catch (const std::exception& e) {
            log(job.label + " - stopped: " + e.what());
        }
        runningAlias_.clear();
    }
}

void Engine::runSequenceAsync(const std::vector<cfg::Step>& steps,
                              const std::string& label,
                              const std::string& alias) {
    if (steps.empty()) return;
    if (!worker_.joinable()) {
        // Test runs work even when the engine isn't started.
        stopping_.store(false);
        worker_ = std::thread([this] { workerMain(); });
    }
    Job job;
    job.steps = steps;
    job.label = label;
    job.alias = alias;
    {
        std::lock_guard<std::mutex> lk(jobMu_);
        jobs_.push_back(std::move(job));
    }
    jobCv_.notify_one();
}

void Engine::releaseInputBlock() {
    if (!inputBlocked_) return;
    BlockInput(FALSE);
    inputBlocked_ = false;
}

void Engine::runSequence(const std::vector<cfg::Step>& steps) {
    // A macro that blocks input and then fails - or that someone wrote
    // without a matching release step - must not leave the user unable to
    // move the mouse. Whatever happens below, the block comes off here.
    struct InputGuard {
        Engine* self;
        ~InputGuard() { self->releaseInputBlock(); }
    } guard{this};

    for (size_t i = 0; i < steps.size(); ++i) {
        try {
            runStep(steps[i]);
        } catch (const std::exception& e) {
            const std::string where = "step " + std::to_string(i + 1) + " (" +
                                      steps[i].summary() + "): " + e.what();
            if (!steps[i].continueOnError()) throw std::runtime_error(where);
            log("  skipped " + where);
        }
    }
}

// ------------------------------------------------------------------ steps
namespace {

bool hasNum(const js::Value& p, const char* key) {
    const js::Value* v = p.find(key);
    return v && (v->isNum() || (v->isStr() && !trim(v->s).empty()));
}

int numInt(const js::Value& p, const char* key, int def = 0) {
    const js::Value* v = p.find(key);
    if (!v) return def;
    if (v->isNum()) return (int)std::llround(v->n);
    if (v->isStr()) {
        std::string t = trim(v->s);
        if (t.empty()) return def;
        return (int)strtol(t.c_str(), nullptr, 10);
    }
    return def;
}

double numDouble(const js::Value& p, const char* key, double def) {
    const js::Value* v = p.find(key);
    if (!v) return def;
    if (v->isNum()) return v->n;
    if (v->isStr()) {
        std::string t = trim(v->s);
        if (t.empty()) return def;
        char* end = nullptr;
        double d = strtod(t.c_str(), &end);
        if (end != t.c_str()) return d;
    }
    return def;
}

bool boolOf(const js::Value& p, const char* key, bool def) {
    const js::Value* v = p.find(key);
    if (!v) return def;
    if (v->isBool()) return v->b;
    if (v->isStr()) {
        std::string t = lower(trim(v->s));
        if (t == "true" || t == "yes" || t == "1") return true;
        if (t == "false" || t == "no" || t == "0") return false;
    }
    if (v->isNum()) return v->n != 0;
    return def;
}

win::WindowInfo requireWindow(const js::Value& p) {
    std::string title = trim(p.str("title")), exe = trim(p.str("exe")),
                cls = trim(p.str("class"));
    if (title.empty() && exe.empty() && cls.empty())
        throw std::runtime_error(
            "give a window title, a program name or a window class");
    win::WindowInfo w;
    if (!win::findWindow(title, exe, cls, w)) {
        std::string who;
        if (!cls.empty()) who = "class " + cls;
        else if (!title.empty()) who = "title containing \"" + title + "\"";
        else who = "program " + exe;
        throw std::runtime_error("no open window with " + who);
    }
    return w;
}

}  // namespace

void Engine::runStep(const cfg::Step& s) {
    const js::Value& p = s.params;
    const std::string& a = s.action;

    double defaultConfidence;
    std::string layoutsDir;
    {
        std::lock_guard<std::mutex> lk(cfgMu_);
        defaultConfidence = cfg_.settings.matchConfidence;
        layoutsDir = cfg_.settings.layoutsDir;
    }

    if (a == "type") {
        act::typeText(p.str("text"), numInt(p, "interval_ms", 0));
        return;
    }
    if (a == "keys") {
        act::pressCombo(p.str("combo"), numInt(p, "hold_ms", 0));
        return;
    }
    if (a == "click") {
        bool hasPos = hasNum(p, "x") && hasNum(p, "y");
        act::click(hasPos, numInt(p, "x"), numInt(p, "y"),
                   p.str("button", "left"), boolOf(p, "double", false));
        return;
    }
    if (a == "move_mouse") {
        act::moveMouse(numInt(p, "x"), numInt(p, "y"));
        return;
    }
    if (a == "scroll") {
        act::scroll(numInt(p, "amount", 0));
        return;
    }
    if (a == "sleep") {
        double sec = numDouble(p, "seconds", 1.0);
        if (sec > 0)
            std::this_thread::sleep_for(
                std::chrono::duration<double>(std::min(sec, 600.0)));
        return;
    }
    if (a == "run") {
        act::runCommand(p.str("cmd"), boolOf(p, "wait", false));
        return;
    }
    if (a == "open") {
        act::openPath(p.str("target"));
        return;
    }
    if (a == "click_image" || a == "wait_image") {
        std::string image = p.str("image");
        if (trim(image).empty()) throw std::runtime_error("no image chosen");
        double conf = numDouble(p, "confidence", defaultConfidence);
        if (conf <= 0 || conf > 1) conf = defaultConfidence;
        double timeout = numDouble(p, "timeout", a == "click_image" ? 5.0 : 10.0);
        bool required = boolOf(p, "required", true);

        vis::Match m;
        std::string err;
        bool found = vis::waitForImage(image, timeout, conf, nullptr, m, &err);
        if (!found) {
            if (!required) {
                log("  image not found (continuing): " + baseName(image));
                return;
            }
            throw std::runtime_error(
                err.empty() ? ("could not find " + baseName(image) +
                               " on screen within " +
                               std::to_string((int)timeout) + "s")
                            : err);
        }
        if (a == "click_image")
            act::click(true, m.x + numInt(p, "offset_x", 0),
                       m.y + numInt(p, "offset_y", 0), p.str("button", "left"),
                       boolOf(p, "double", false));
        return;
    }
    if (a == "window_focus") {
        win::focusWindow(requireWindow(p));
        return;
    }
    if (a == "window_prepare") {
        win::WindowInfo w = requireWindow(p);

        // Bring it forward and actually wait for it. Clicking before the
        // window arrives sends the clicks to whatever was in front.
        if (!win::activateAndWait(w, numDouble(p, "activate_timeout", 2.0)))
            log("  \"" + w.title + "\" did not come to the front in time - "
                "carrying on anyway");

        // Remember where it is now, so the release step can put it back.
        locked_ = LockedWindow{};
        locked_.active = true;
        locked_.hwnd = w.hwnd;
        locked_.rect = {(LONG)w.x, (LONG)w.y, (LONG)(w.x + w.w),
                        (LONG)(w.y + w.h)};
        locked_.wasMaximized = w.maximized;

        if (boolOf(p, "block_input", true)) {
            // Windows refuses this unless the process is elevated. Say so
            // rather than pretending the sequence is protected when it is not.
            if (BlockInput(TRUE)) {
                inputBlocked_ = true;
            } else {
                log("  could not block input (Windows only allows that when "
                    "MacroDeck runs as administrator) - keep your hands off "
                    "the mouse while this runs");
            }
        }

        // Un-maximise before moving: a maximised window ignores a move.
        if (w.maximized) win::restoreWindow(w);

        const bool hasW = hasNum(p, "w"), hasH = hasNum(p, "h");
        const bool hasX = hasNum(p, "x"), hasY = hasNum(p, "y");
        if (hasX || hasY || hasW || hasH)
            win::moveResize(w, hasX, numInt(p, "x"), hasY, numInt(p, "y"),
                            hasW, numInt(p, "w"), hasH, numInt(p, "h"));

        const int settle = numInt(p, "settle_ms", 300);
        if (settle > 0)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(std::min(settle, 60000)));
        return;
    }
    if (a == "window_release") {
        if (boolOf(p, "restore_window", true) && locked_.active &&
            IsWindow(locked_.hwnd)) {
            win::WindowInfo w;
            if (win::describeWindow(locked_.hwnd, w)) {
                if (locked_.wasMaximized) {
                    win::maximizeWindow(w);
                } else {
                    win::moveResize(w, true, locked_.rect.left, true,
                                    locked_.rect.top, true,
                                    locked_.rect.right - locked_.rect.left,
                                    true,
                                    locked_.rect.bottom - locked_.rect.top);
                }
            }
        }
        locked_ = LockedWindow{};
        if (boolOf(p, "unblock_input", true)) releaseInputBlock();
        return;
    }
    if (a == "window_move") {
        win::moveResize(requireWindow(p), hasNum(p, "x"), numInt(p, "x"),
                        hasNum(p, "y"), numInt(p, "y"), hasNum(p, "w"),
                        numInt(p, "w"), hasNum(p, "h"), numInt(p, "h"));
        return;
    }
    if (a == "window_minimize") {
        win::minimizeWindow(requireWindow(p));
        return;
    }
    if (a == "window_maximize") {
        win::maximizeWindow(requireWindow(p));
        return;
    }
    if (a == "window_save") {
        std::string name = trim(p.str("layout"));
        if (name.empty()) throw std::runtime_error("give the layout a name");
        std::string err;
        if (!win::saveLayout(name, layoutsDir, &err))
            throw std::runtime_error(err);
        log("  window layout \"" + name + "\" saved");
        return;
    }
    if (a == "window_restore") {
        std::string name = trim(p.str("layout"));
        if (name.empty()) throw std::runtime_error("give the layout a name");
        std::string err;
        int n = win::restoreLayout(name, layoutsDir, &err);
        if (n == 0 && !err.empty()) throw std::runtime_error(err);
        log("  window layout \"" + name + "\": " + std::to_string(n) +
            " window(s) repositioned");
        return;
    }

    if (a == "page_goto") {
        if (runningAlias_.empty())
            throw std::runtime_error(
                "a page step only means something on a deck's own key");
        const std::string dir = trim(p.str("direction", "named"));
        const std::string name = trim(p.str("page"));
        int target = resolvePage(runningAlias_, name, dir);
        if (target < 0)
            throw std::runtime_error(
                name.empty() ? "which page? name one, or pick a direction"
                             : "there is no page called \"" + name + "\"");
        requestPage(runningAlias_, target);
        return;
    }

    // The switch itself happened on key-down, and the switch back happens on
    // key-up, both in onKey where the release can be seen. Reaching it here
    // means the sequence ran - a Test run, or a key that carries other steps
    // alongside it - and there is nothing left to do.
    if (a == "page_hold") return;

    throw std::runtime_error("unknown action \"" + a + "\"");
}

// ------------------------------------------------------------ start / stop
void Engine::start() {
    if (running_.load()) return;

    cfg::Config snapshot;
    {
        std::lock_guard<std::mutex> lk(cfgMu_);
        snapshot = cfg_;
    }
    if (snapshot.keyboards.empty())
        throw std::runtime_error(
            "There is no macro keyboard to listen to yet. Add one first.");

    stopping_.store(false);
    if (!worker_.joinable()) worker_ = std::thread([this] { workerMain(); });

    // Anything "held" is a leftover from the last run, not a key the user is
    // holding now.
    presses_.clear();

    // Software buttons are clicked, not pressed. If that is all there is,
    // there is nothing to listen to and no reason to register for raw input.
    bool anyPhysical = false;
    bool anyMouse = false;
    for (const auto& k : snapshot.keyboards) {
        if (k.softwareOnly) continue;
        anyPhysical = true;
        if (k.isMouse) anyMouse = true;
    }

    if (anyPhysical) {
        try {
            listener_.start([this](const dev::KeyEvent& ev) { onKey(ev); },
                            anyMouse);
            listenerActive_.store(true);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                std::string("Could not read keyboard input: ") + e.what());
        }
    }

    bool anyBlocking = false;
    for (const auto& k : snapshot.keyboards)
        if (k.block && !k.softwareOnly) anyBlocking = true;

    if (anyBlocking) {
        try {
            hook_.start(&listener_,
                        [this](const std::string& path, int vk) {
                            std::lock_guard<std::mutex> lk(cfgMu_);
                            return cfg_.blockedForKey(path, vk);
                        },
                        // Asked first, for every keystroke in the machine.
                        [this](int vk) {
                            return vk >= 0 && vk < 256 &&
                                   couldBlock_[(size_t)vk].load(
                                       std::memory_order_relaxed);
                        },
                        30, snapshot.anyMouseBlocks());
        } catch (const std::exception& e) {
            listener_.stop();
            listenerActive_.store(false);
            throw std::runtime_error(
                std::string("Could not block macro keys: ") + e.what());
        }
    }

    running_.store(true);
    // Say what is actually happening. With only software buttons configured
    // nothing is being listened to, and claiming otherwise is how someone
    // ends up hunting for latency that is not there.
    log(anyPhysical ? (anyMouse ? "Listening for macro keys and buttons."
                                : "Listening for macro keys.")
                    : "Ready. Nothing is being read from any keyboard.");
    for (const auto& k : snapshot.keyboards) {
        int bound = 0;
        for (const auto& pg : k.pages)
            for (const auto& b : pg.bindings)
                if (!b.steps.empty()) ++bound;
        log("  " + k.alias + " - " + std::to_string(bound) +
            (bound == 1 ? " macro, " : " macros, ") +
            (k.pages.size() > 1
                 ? "across " + std::to_string(k.pages.size()) + " pages, "
                 : "") +
            (k.softwareOnly    ? "clicked on screen, no keyboard involved"
             : !k.block        ? (k.isMouse ? "buttons also work normally"
                                            : "keys also type normally")
             : k.blockAllKeys  ? (k.isMouse
                                      ? "every button blocked from apps"
                                      : "every key blocked from apps")
             : k.isMouse       ? "only macro buttons blocked, the rest still "
                                 "work"
                               : "only macro keys blocked, the rest still type"));
    }
}

void Engine::stop() {
    if (running_.load()) {
        hook_.stop();
        listener_.stop();
        listenerActive_.store(false);
        running_.store(false);
        presses_.clear();
        log("Stopped listening - macro keys type normally again.");
    }
    stopping_.store(true);
    jobCv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

}  // namespace eng
