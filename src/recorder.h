// The part of click recording that decides what gets written: which steps a
// list of recorded clicks turns into, and when a second right-click counts as
// a double-click. No windows and no input here, so the self-test can cover it.
#pragma once
#include "config.h"

#include <string>
#include <vector>

namespace rec {

// The top-level window a click landed in. Recorded positions are absolute
// screen coordinates, so they only land correctly while that window is back
// at the size and position it had during recording.
struct Target {
    bool has = false;
    std::string cls;      // window class - the stable identifier
    std::string exe;
    int x = 0, y = 0, w = 0, h = 0;
    bool maximized = false;

    bool sameWindowAs(const Target& o) const;
    // "OpenDental" for opendental.exe; what the recorder shows the user.
    // Deliberately not the window title, which often carries a patient's name.
    std::string displayName() const;
};

struct Click {
    int x = 0, y = 0;
    bool isDouble = false;
    Target window;
};

enum class Speed { Fast, Normal, Slow };
double pauseSeconds(Speed s);

// One line for the recorder's list: "Click in OpenDental".
std::string describe(const Click& c);

// Should a right-click released at (x, y) at `timeMs` turn the previous one,
// released at (px, py) at `prevTimeMs`, into a double-click? The limits are
// the system's own double-click time and rectangle, passed in so this stays
// free of Win32 calls.
bool makesDouble(int px, int py, unsigned long prevTimeMs, int x, int y,
                 unsigned long timeMs, unsigned long doubleClickMs,
                 int rectW, int rectH);

// The steps a recording becomes: lock the first click's window in place,
// each click with a pause between them, then release the window. No pause
// after the last click - the release, or the next step, follows it at once.
std::vector<cfg::Step> buildSteps(const std::vector<Click>& clicks, Speed speed);

}  // namespace rec
