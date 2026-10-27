#include "recorder.h"

#include "friendly.h"
#include "util.h"

#include <cstdlib>

namespace rec {

bool Target::sameWindowAs(const Target& o) const {
    if (!has || !o.has) return has == o.has;
    return cls == o.cls && iequals(exe, o.exe);
}

std::string Target::displayName() const {
    if (!has) return "";
    if (exe.empty()) return "another program";
    return fe::programName(exe);
}

double pauseSeconds(Speed s) {
    switch (s) {
        case Speed::Fast: return 0.2;
        case Speed::Slow: return 1.5;
        case Speed::Normal:
        default: return 0.5;
    }
}

std::string describe(const Click& c) {
    std::string s = c.isDouble ? "Double-click" : "Click";
    if (c.window.has) s += " in " + c.window.displayName();
    else s += " on the screen (not inside a program)";
    return s;
}

bool makesDouble(int px, int py, unsigned long prevTimeMs, int x, int y,
                 unsigned long timeMs, unsigned long doubleClickMs, int rectW,
                 int rectH) {
    // Unsigned subtraction copes with the tick count wrapping round.
    if (timeMs - prevTimeMs > doubleClickMs) return false;
    return std::abs(x - px) <= rectW / 2 && std::abs(y - py) <= rectH / 2;
}

std::vector<cfg::Step> buildSteps(const std::vector<Click>& clicks,
                                  Speed speed) {
    std::vector<cfg::Step> steps;
    if (clicks.empty()) return steps;

    // The window comes from the first click only, as it always has: later
    // clicks are expected to be inside it, and the recorder says so when one
    // is not.
    const Target& first = clicks.front().window;
    if (first.has) {
        cfg::Step prep;
        prep.action = "window_prepare";
        prep.params = js::Value::object();
        prep.params["class"] = first.cls;
        if (!first.exe.empty()) prep.params["exe"] = first.exe;
        prep.params["x"] = first.x;
        prep.params["y"] = first.y;
        prep.params["w"] = first.w;
        prep.params["h"] = first.h;
        steps.push_back(prep);
    }

    const double pause = pauseSeconds(speed);
    for (size_t i = 0; i < clicks.size(); ++i) {
        cfg::Step click;
        click.action = "click";
        click.params = js::Value::object();
        click.params["x"] = clicks[i].x;
        click.params["y"] = clicks[i].y;
        if (clicks[i].isDouble) click.params["double"] = true;
        steps.push_back(click);

        if (i + 1 < clicks.size()) {
            cfg::Step wait;
            wait.action = "sleep";
            wait.params = js::Value::object();
            wait.params["seconds"] = pause;
            steps.push_back(wait);
        }
    }

    if (first.has) {
        cfg::Step release;
        release.action = "window_release";
        release.params = js::Value::object();
        steps.push_back(release);
    }
    return steps;
}

}  // namespace rec
