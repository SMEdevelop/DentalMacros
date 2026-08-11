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
    std::string cls;        // window class, e.g. "Notepad", "CabinetWClass"
    int x = 0, y = 0, w = 0, h = 0;
    bool minimized = false;
    bool maximized = false;
};

std::vector<WindowInfo> listWindows(bool includeUntitled = false);

// Case-insensitive substring on title, exact match on exe and class. Any of
// them may be empty, in which case it is not used. Returns false if nothing
// matched.
//
// Class is the most stable of the three: a window's title changes with its
// contents and its executable can be shared by unrelated windows, but the
// class is fixed by whoever wrote the program. It is what makes a recorded
// click sequence still find its target tomorrow.
bool findWindow(const std::string& title, const std::string& exe,
                const std::string& cls, WindowInfo& out);

// Describe one specific window. Returns false if the handle is not a window.
bool describeWindow(HWND hwnd, WindowInfo& out);
// The top-level window at a screen position - what the user would say they
// clicked on, rather than the child control that received it.
bool windowAtPoint(int screenX, int screenY, WindowInfo& out);

void focusWindow(const WindowInfo& w);
// Focus, then wait until it really is the foreground window. Returns false if
// it never got there within the timeout.
bool activateAndWait(const WindowInfo& w, double timeoutSec);
void moveResize(const WindowInfo& w, bool hasX, int x, bool hasY, int y,
                bool hasW, int width, bool hasH, int height);
void minimizeWindow(const WindowInfo& w);
void maximizeWindow(const WindowInfo& w);
void restoreWindow(const WindowInfo& w);   // un-maximise / un-minimise

// Layouts are JSON files under <layoutsDir>/<name>.json.
bool saveLayout(const std::string& name, const std::string& layoutsDir,
                std::string* error = nullptr);
int restoreLayout(const std::string& name, const std::string& layoutsDir,
                  std::string* error = nullptr);
std::vector<std::string> listLayouts(const std::string& layoutsDir);
bool deleteLayout(const std::string& name, const std::string& layoutsDir);

}  // namespace win
