// Window management: find, focus, move/resize, and save/restore layouts.
#pragma once
#include <string>
#include <vector>
#include <windows.h>

namespace win {

struct WindowInfo {
    HWND hwnd = nullptr;
    std::string title;
    std::string exe;        // image name only, e.g. "notepad.exe"
    int x = 0, y = 0, w = 0, h = 0;
    bool minimized = false;
    bool maximized = false;
};

std::vector<WindowInfo> listWindows(bool includeUntitled = false);

// Case-insensitive substring on title, exact match on exe. Either may be
// empty. Returns false if nothing matched.
bool findWindow(const std::string& title, const std::string& exe,
                WindowInfo& out);

void focusWindow(const WindowInfo& w);
void moveResize(const WindowInfo& w, bool hasX, int x, bool hasY, int y,
                bool hasW, int width, bool hasH, int height);
void minimizeWindow(const WindowInfo& w);
void maximizeWindow(const WindowInfo& w);

// Layouts are JSON files under <layoutsDir>/<name>.json.
bool saveLayout(const std::string& name, const std::string& layoutsDir,
                std::string* error = nullptr);
int restoreLayout(const std::string& name, const std::string& layoutsDir,
                  std::string* error = nullptr);
std::vector<std::string> listLayouts(const std::string& layoutsDir);
bool deleteLayout(const std::string& name, const std::string& layoutsDir);

}  // namespace win
