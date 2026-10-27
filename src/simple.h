// Simple mode's view of a button: its steps grouped into the handful of
// things Simple mode knows how to make - type, press keys, recorded clicks,
// wait - with everything else gathered into "advanced steps" rows that are
// shown, kept and never touched.
//
// Pure: no windows, so the self-test can read every grouping.
#pragma once
#include "config.h"

#include <string>
#include <vector>

namespace smp {

enum class Kind {
    Type,       // one "type" step
    Keys,       // one "keys" step
    Wait,       // one "sleep" step
    Clicks,     // a recorded click sequence: lock, clicks and pauses, release
                // - or clicks and the pauses between them, with no lock
    Advanced,   // a run of steps Simple mode doesn't make
};

struct Row {
    Kind kind = Kind::Advanced;
    size_t first = 0;   // index of its first step
    size_t count = 0;   // how many steps it covers
    std::string text;   // what the list shows
};

// Every step belongs to exactly one row, in order, so a row's range can be
// removed or replaced without disturbing its neighbours.
std::vector<Row> rowsFor(const std::vector<cfg::Step>& steps);

// The row holding step `index`, or -1.
int rowOfStep(const std::vector<Row>& rows, size_t index);

// "Wait half a second", "Wait 1 second", "Wait 1.5 seconds".
std::string waitText(double seconds);

}  // namespace smp
