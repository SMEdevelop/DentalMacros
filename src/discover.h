// Finding the dental programs on this computer, so first-run setup can say
// "Found Open Dental" - and, more usefully, warn when one runs as
// administrator, which makes Windows drop every keystroke and click sent to it.
//
// Only product names are matched, and only ever to show a row or a warning:
// nothing found here changes a setting.
#pragma once
#include <string>
#include <vector>

namespace discover {

struct DentalProgram {
    std::string name;          // "Open Dental"
    bool running = false;      // has a window open right now
    bool elevated = false;     // running at a level Windows won't send input to
};

// The product a piece of text names - an uninstall entry, a window title, an
// executable - or "" if none. Case, spaces and punctuation are ignored, so
// "OpenDental.exe" and "Open Dental 24.1" both give "Open Dental".
std::string matchProduct(const std::string& text);

// Every product found installed or open, one entry each, open ones first.
std::vector<DentalProgram> findDentalPrograms();

}  // namespace discover
