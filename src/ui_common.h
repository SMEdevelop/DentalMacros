// Shared UI plumbing: dark-themed standard controls and a modal window host
// that works without dialog resource templates.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

namespace ui {

// Fonts, cached and DPI-scaled.
HFONT uiFont(int sizePx = 14, bool bold = false);
void releaseFonts();

// Rebuild every cached font at the current DPI scale and hand the new handles
// to each control under `root`. Call after setDpiScale() - the old handles are
// destroyed, so anything still holding one must be told about the change in
// the same breath. Item heights of lists and combos are rescaled too.
void rescaleControlFonts(HWND root);

// Ask Windows for a dark title bar / scrollbars where the OS supports it.
void applyDarkFrame(HWND hwnd);

// ------------------------------------------------------------- control kit
HWND makeLabel(HWND parent, const std::string& text, int x, int y, int w, int h,
               int id = -1, bool dim = false, bool bold = false);
HWND makeEdit(HWND parent, const std::string& text, int x, int y, int w, int h,
              int id, bool multiline = false, bool readOnly = false);
// Button colour roles.
enum class Tone { Normal, Primary, Good, Danger };
void setTone(HWND button, Tone t);

HWND makeButton(HWND parent, const std::string& text, int x, int y, int w,
                int h, int id, bool primary = false);
HWND makeCheck(HWND parent, const std::string& text, int x, int y, int w, int h,
               int id, bool checked);
HWND makeCombo(HWND parent, int x, int y, int w, int h, int id,
               const std::vector<std::string>& items, int selected);
HWND makeList(HWND parent, int x, int y, int w, int h, int id);

// A vertically scrolling container. Put child controls in it exactly as you
// would in any other parent - positions are still design pixels, measured
// from the top of the *content*, not the visible part. The pane forwards
// WM_COMMAND, WM_DRAWITEM, WM_NOTIFY and WM_CTLCOLOR* to its own parent, so a
// dialog handles the controls inside it without knowing they moved.
HWND makeScrollPane(HWND parent, int x, int y, int w, int h, int id);
// Tell the pane how tall its content is (design pixels) so it can decide
// whether a scrollbar is needed. Resets the scroll position to the top.
void setScrollPaneContent(HWND pane, int contentHeightDesignPx);

std::string getText(HWND ctl);
void setText(HWND ctl, const std::string& text);
bool isChecked(HWND ctl);
void setChecked(HWND ctl, bool on);
void setPrimary(HWND button, bool primary);
void setDim(HWND label, bool dim);

// WM_DRAWITEM / WM_CTLCOLOR* handling shared by every window here.
// Returns true if the message was handled.
bool handleDrawItem(HWND hwnd, DRAWITEMSTRUCT* dis);
bool handleCtlColor(UINT msg, WPARAM wp, LPARAM lp, LRESULT& out);
void paintPanelBackground(HWND hwnd, HDC hdc);

// Work area (screen minus taskbar) of the monitor `nearWindow` is on.
RECT workAreaNear(HWND nearWindow);
// The tallest client area, in design pixels, that a popup can have there and
// still fit entirely on screen.
int availableClientHeight(HWND nearWindow);

// --------------------------------------------------------------- modal host
// A modal popup built from ordinary child controls. Subclasses fill in
// build() and onCommand().
class Modal {
public:
    virtual ~Modal();

    // Returns true if the user confirmed. `w`/`h` are in design pixels.
    bool run(HWND parent, const std::string& title, int w, int h);

protected:
    virtual void build() = 0;
    virtual void onCommand(int id, int notify) {}
    virtual bool onOk() { return true; }   // false keeps the dialog open
    virtual void onCancel() {}
    virtual bool onMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& out) {
        return false;
    }
    virtual void onSized(int w, int h) {}

    void close(bool ok);
    void layoutFooter(int okId, const std::string& okText, int cancelId,
                      const std::string& cancelText);

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    int clientW_ = 0, clientH_ = 0;
    bool result_ = false;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    bool done_ = false;
};

// Simple prompts.
void showMessage(HWND parent, const std::string& title, const std::string& text,
                 bool error = false);
bool askYesNo(HWND parent, const std::string& title, const std::string& text);
bool askText(HWND parent, const std::string& title, const std::string& prompt,
             std::string& value, const std::string& hint = "");

// Common control ids.
enum { ID_OK = 1, ID_CANCEL = 2, ID_FIRST_USER = 100 };

}  // namespace ui
