// Screen capture + local template matching.
//
// This replaces the OpenCV dependency the Python version carried: normalised
// cross-correlation (the same metric as cv2.TM_CCOEFF_NORMED) computed over an
// image pyramid, so a full-screen search stays in the low tens of
// milliseconds. Nothing leaves the machine.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace vis {

struct Match {
    int x = 0;            // centre, absolute virtual-screen coordinates
    int y = 0;
    double confidence = 0;
    int w = 0;            // template size
    int h = 0;
};

struct Capture {
    int originX = 0, originY = 0;
    int width = 0, height = 0;
    std::vector<uint32_t> bgra;   // row-major, top-down
    bool valid() const { return width > 0 && height > 0 && !bgra.empty(); }
};

// Grab the whole virtual desktop, one monitor, or an explicit rectangle.
bool captureScreen(Capture& out, std::string* error = nullptr);
bool captureRegion(int x, int y, int w, int h, Capture& out,
                   std::string* error = nullptr);
bool captureMonitor(int index, Capture& out, std::string* error = nullptr);
int monitorCount();
bool monitorRect(int index, RECT& out);

// Search. `region` may be null for the whole virtual screen.
//
// `fatal` (optional) reports whether a failure is permanent - a missing file,
// an undecodable or featureless template, one bigger than the search area.
// Waiting on any of those can only ever time out, so waitForImage gives up at
// once instead of retrying for the whole timeout.
std::vector<Match> findAll(const std::string& templatePath, double confidence,
                           const RECT* region, int limit,
                           std::string* error = nullptr, bool* fatal = nullptr);
bool findImage(const std::string& templatePath, double confidence,
               const RECT* region, Match& out, std::string* error = nullptr,
               bool* fatal = nullptr);
bool waitForImage(const std::string& templatePath, double timeoutSec,
                  double confidence, const RECT* region, Match& out,
                  std::string* error = nullptr);

// Save a crop of a capture as a PNG (used by the snipping tool).
bool savePng(const std::string& path, const Capture& cap, RECT crop,
             std::string* error = nullptr);

// Size of a template image on disk, for UI display. Returns false if the
// file can't be read.
bool imageSize(const std::string& path, int& w, int& h);

void clearTemplateCache();

}  // namespace vis
