// A small always-on-top palette of the macro buttons.
//
// The deck in the main window is an editor: big tiles, an inspector beside
// them, a log underneath. This is the other half - a launcher small enough to
// leave on top of whatever you are actually working in, so a macro can be
// fired with the mouse when the macro pad is out of reach or not plugged in.
//
// It deliberately does not activate when clicked (WS_EX_NOACTIVATE). A macro
// that types or clicks acts on whatever window is in front, so a palette that
// stole focus on the way to firing one would send every keystroke to itself.
#pragma once

#include <climits>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <windows.h>

namespace ui {

// One button, flattened out of a cfg::Binding. Copied rather than pointed at:
// the editor rewrites its bindings vector freely, and this window paints on a
// different message pump's schedule.
struct MiniButton {
    std::string key;
    std::string title;
    std::string icon;         // custom image, relative to the exe
    std::string color;        // "#rrggbb", empty = theme default
    std::string firstAction;  // action id of step 1, for the fallback glyph
    bool hasSteps = false;
};

// Where the palette sits, how big it is, and whether it was open. Saved with
// the config so it comes back where it was left rather than in the middle of
// the screen every launch.
struct MiniPlacement {
    bool visible = false;
    int x = INT_MIN;      // INT_MIN = never placed, pick a corner
    int y = INT_MIN;
    int tile = 38;        // design px per button
    int cols = 5;
};

// Button sizes, all smaller than a desktop icon (48px plus its label).
constexpr int kMiniTileSmall = 30;
constexpr int kMiniTileMedium = 38;
constexpr int kMiniTileLarge = 46;
constexpr int kMiniColsMin = 2;
constexpr int kMiniColsMax = 10;

class MiniDeck {
public:
    // Fire the macro on `key`. Runs on the UI thread; the engine queues the
    // actual steps onto its worker.
    using RunFn = std::function<void(const std::string& key)>;
    using VoidFn = std::function<void()>;

    ~MiniDeck();

    void setRunner(RunFn fn) { run_ = std::move(fn); }
    // "Open MacroDeck" in the context menu.
    void setOpener(VoidFn fn) { open_ = std::move(fn); }
    // Told whenever the user moves or resizes it, so the config can be marked
    // dirty without this window knowing what a config is.
    void setOnPlacementChanged(VoidFn fn) { moved_ = std::move(fn); }

    // `caption` is the keyboard alias shown in the grip strip.
    void setButtons(const std::string& caption, std::vector<MiniButton> buttons);

    void show(HINSTANCE inst);
    void hide();
    void toggle(HINSTANCE inst);
    bool visible() const;
    void destroy();

    // Light a button up because its key was pressed on the pad, so the
    // palette agrees with the deck about what just ran.
    void noteFired(const std::string& key);

    MiniPlacement placement() const;
    void setPlacement(const MiniPlacement& p);

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool ensureWindow(HINSTANCE inst);
    void relayout();          // recompute rects and resize the window
    void paint(HDC hdc);
    void paintButton(void* graphics, size_t index);
    int hitTest(POINT pt) const;   // button index, or -1
    void contextMenu(POINT screenPt);
    void clampOnScreen(int& x, int& y, int w, int h) const;
    void startFlashTimer();

    HWND hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;

    std::string caption_;
    std::vector<MiniButton> buttons_;
    std::vector<RECT> rects_;      // one per button, client coords
    RECT rcGrip_{}, rcClose_{};

    MiniPlacement place_;
    int hover_ = -1;
    bool hoverClose_ = false;
    bool tracking_ = false;

    // Manual dragging. SendMessage(WM_NCLBUTTONDOWN, HTCAPTION) would ask
    // Windows to activate this window on the way into its move loop, which is
    // exactly what WS_EX_NOACTIVATE is here to prevent.
    bool dragging_ = false;
    POINT dragGrab_{};      // cursor offset inside the window when the drag began

    std::map<std::string, DWORD> fired_;   // key -> tick it last ran
    bool flashTimer_ = false;

    RunFn run_;
    VoidFn open_;
    VoidFn moved_;
};

}  // namespace ui
