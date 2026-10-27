#include "simple.h"

#include "friendly.h"
#include "keys.h"
#include "util.h"

#include <cmath>
#include <cstdio>

namespace smp {
namespace {

bool isClickOrPause(const cfg::Step& s) {
    return s.action == "click" || s.action == "sleep";
}

std::string clicksText(size_t clicks, const std::string& where) {
    return "Click " + where + " (" + std::to_string(clicks) +
           (clicks == 1 ? " click)" : " clicks)");
}

// The text of a "type" step, on one line and short enough for the list.
std::string typedPreview(const std::string& text) {
    std::string flat;
    for (char c : text) {
        if (c == '\r') continue;
        flat += c == '\n' ? ' ' : c;
    }
    flat = trim(flat);
    // Cut on a character boundary: never through the middle of a UTF-8
    // sequence, whose continuation bytes are 10xxxxxx.
    const size_t limit = 34;
    if (flat.size() > limit) {
        size_t cut = limit;
        while (cut > 0 && ((unsigned char)flat[cut] & 0xC0) == 0x80) --cut;
        // Between words, unless that would throw away most of it.
        const size_t space = flat.rfind(' ', cut);
        if (space != std::string::npos && space > limit / 2) cut = space;
        flat = trim(flat.substr(0, cut)) + "\xE2\x80\xA6";
    }
    return flat;
}

void addAdvanced(std::vector<Row>& rows, size_t index) {
    if (!rows.empty() && rows.back().kind == Kind::Advanced &&
        rows.back().first + rows.back().count == index) {
        ++rows.back().count;
    } else {
        Row r;
        r.kind = Kind::Advanced;
        r.first = index;
        r.count = 1;
        rows.push_back(r);
    }
}

}  // namespace

std::string waitText(double seconds) {
    if (std::fabs(seconds - 0.5) < 1e-9) return "Wait half a second";
    if (std::fabs(seconds - 1.0) < 1e-9) return "Wait 1 second";
    if (std::fabs(seconds - std::round(seconds)) < 1e-9)
        return "Wait " + std::to_string((long long)std::round(seconds)) +
               " seconds";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", seconds);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return "Wait " + s + " seconds";
}

std::vector<Row> rowsFor(const std::vector<cfg::Step>& steps) {
    std::vector<Row> rows;
    const size_t n = steps.size();
    size_t i = 0;
    while (i < n) {
        const cfg::Step& s = steps[i];

        // A recorded sequence: lock, clicks and pauses, release.
        if (s.action == "window_prepare") {
            size_t j = i + 1, clicks = 0;
            while (j < n && isClickOrPause(steps[j])) {
                if (steps[j].action == "click") ++clicks;
                ++j;
            }
            if (j < n && steps[j].action == "window_release" && clicks > 0) {
                const std::string exe = s.params.str("exe");
                Row r;
                r.kind = Kind::Clicks;
                r.first = i;
                r.count = j - i + 1;
                r.text = clicksText(clicks, exe.empty()
                                                ? std::string("in the recorded window")
                                                : "in " + fe::programName(exe));
                rows.push_back(r);
                i = j + 1;
                continue;
            }
            addAdvanced(rows, i);
            ++i;
            continue;
        }

        // Clicks with no lock: the pauses between them belong to them, a
        // pause after the last one is a Wait of its own.
        if (s.action == "click") {
            size_t j = i, last = i, clicks = 0;
            while (j < n && isClickOrPause(steps[j])) {
                if (steps[j].action == "click") {
                    last = j;
                    ++clicks;
                }
                ++j;
            }
            Row r;
            r.kind = Kind::Clicks;
            r.first = i;
            r.count = last - i + 1;
            r.text = clicksText(clicks, "on the screen");
            rows.push_back(r);
            i = last + 1;
            continue;
        }

        Row r;
        r.first = i;
        r.count = 1;
        if (s.action == "type") {
            r.kind = Kind::Type;
            r.text = "Type \"" + typedPreview(s.params.str("text")) + "\"";
        } else if (s.action == "keys") {
            r.kind = Kind::Keys;
            const std::string combo = trim(s.params.str("combo"));
            r.text = combo.empty() ? "Press keys"
                                   : "Press " + keys::prettyCombo(combo);
        } else if (s.action == "sleep") {
            r.kind = Kind::Wait;
            r.text = waitText(s.params.num("seconds", 1.0));
        } else {
            addAdvanced(rows, i);
            ++i;
            continue;
        }
        rows.push_back(r);
        ++i;
    }

    for (Row& r : rows)
        if (r.kind == Kind::Advanced)
            r.text = "+ " + std::to_string(r.count) +
                     (r.count == 1 ? " advanced step" : " advanced steps") +
                     " (double-click to see)";
    return rows;
}

int rowOfStep(const std::vector<Row>& rows, size_t index) {
    for (size_t r = 0; r < rows.size(); ++r)
        if (index >= rows[r].first && index < rows[r].first + rows[r].count)
            return (int)r;
    return -1;
}

}  // namespace smp
