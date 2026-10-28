// Shared UI plumbing: standard controls drawn in the SMEdevelop design
// system's style, and a modal window host that works without dialog
// resource templates.
#pragma once
#include "tokens.h"

#include <functional>
#include <string>
#include <vector>
#include <windows.h>

namespace ui {

// Fonts, cached and DPI-scaled. `bold` is the design system's 600.
HFONT uiFont(int sizePx = 14, bool bold = false);
HFONT uiFontWeight(int sizePx, int weight);
HFONT uiFont(tk::Text style);
void releaseFonts();

// Rebuild every cached font at the current DPI scale and hand the new handles
// to each control under `root`. Call after setDpiScale() - the old handles are
// destroyed, so anything still holding one must be told about the change in
// the same breath. Item heights of lists and combos are rescaled too.
void rescaleControlFonts(HWND root);

// A light title bar and frame that match the design system (no dark mode).
void applyFrame(HWND hwnd);

// The colour a window paints behind its controls. Controls take their
// corners and gaps from the nearest window above them that has one set;
// anything with none is on surface-raised, the colour of every panel and
// dialog.
void setSurface(HWND window, COLORREF c);
COLORREF surfaceBehind(HWND ctl);

// ------------------------------------------------------------- control kit
HWND makeLabel(HWND parent, const std::string& text, int x, int y, int w, int h,
               int id = -1, bool dim = false, bool bold = false);
// A label in one of the design system's text styles. Wraps.
HWND makeText(HWND parent, const std::string& text, int x, int y, int w, int h,
              tk::Text style, bool muted = false, int id = -1);
// How tall `text` is in `style`, wrapped at `width` (both design px).
int textHeight(const std::string& text, tk::Text style, int width);

HWND makeEdit(HWND parent, const std::string& text, int x, int y, int w, int h,
              int id, bool multiline = false, bool readOnly = false);

// The design system's TextField: a label above a 48px box, and a hint in
// muted text under it. setFieldError() turns the edge red and swaps the hint
// for what to do next; an empty message puts the hint back.
struct Field {
    HWND label = nullptr, edit = nullptr, hint = nullptr;
    std::string hintText;
};
Field makeField(HWND parent, const std::string& label, const std::string& value,
                const std::string& hint, int x, int y, int w, int id,
                int boxH = tk::kTargetMin, bool multiline = false);
// Height makeField() takes, from the top of the label to the bottom of the
// hint (design px).
int fieldHeight(const std::string& hint, int w, int boxH = tk::kTargetMin);
void setFieldError(Field& f, const std::string& message);

// The design system's five kinds of button. Normal and Good are the names
// older screens use.
enum class Tone {
    Quiet,
    Primary,
    Secondary,
    Success,
    Danger,
    Normal = Quiet,
    Good = Success,
};
void setTone(HWND button, Tone t);

HWND makeButton(HWND parent, const std::string& text, int x, int y, int w,
                int h, int id, bool primary = false);
// An icon drawn before a button's label: an action id or one of theme.h's
// UI icons ("plus", "check"...).
void setButtonIcon(HWND button, const std::string& iconId);
// Change the font of any control to one of the text styles.
void setTextStyle(HWND ctl, tk::Text style);

// Turn a button into a Pill. Several pills side by side make one switch
// ("Simple | Advanced"): the first and last get the rounded ends, and the
// active one is shown in the light blue wash with a denim edge.
enum class PillPart { Whole, First, Middle, Last };
void makePill(HWND button, PillPart part = PillPart::Whole);
void setPillActive(HWND button, bool active);

// A large tile you pick one of (the design system's RoomTile): a title, a
// line or two under it, and a round check that fills when it is picked.
HWND makeChoice(HWND parent, const std::string& title, const std::string& sub,
                int x, int y, int w, int h, int id);
void setChoiceSelected(HWND choice, bool selected);
// The title's text style; tiles start in TileLabel (24px).
void setChoiceTitleStyle(HWND choice, tk::Text style);

// One of the seven key colours, as a round swatch you can click or reach
// with Tab. The chosen one carries a tick.
HWND makeSwatch(HWND parent, int x, int y, int size, int id,
                const std::string& hex, const std::string& name);
void setSwatchSelected(HWND swatch, bool selected);

// The design system's Banner: a tinted strip with an icon and bold text,
// for something that needs reading before going on. The text is the
// window's own, so setText() changes it.
enum class BannerTone { Danger, Warning, Info };
HWND makeBanner(HWND parent, BannerTone tone, const std::string& text, int x,
                int y, int w);
// How tall a banner with `text` is at width `w` (design px).
int bannerHeight(const std::string& text, int w);
void setBannerTone(HWND banner, BannerTone tone);

// Row height of an owner-drawn list, in design px.
void setListRowHeight(HWND list, int designPx);
// Height of a dropdown's closed box, in design px.
void setComboHeight(HWND combo, int designPx);

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
// Scroll so that `child` (a control inside the pane) is in view.
void scrollPaneToShow(HWND pane, HWND child);

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
    // Asked before Esc, Cancel or the close box shuts the window. Return
    // false to keep it open - for a window holding work that would be lost.
    virtual bool canCancel() { return true; }
    virtual bool onMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& out) {
        return false;
    }
    virtual void onSized(int w, int h) {}

    void close(bool ok);
    // The footer every sheet shares: 64px buttons along the bottom inside
    // 32px of padding, Quiet `cancelText` on the left and the Primary
    // `okText` on the right. With no `cancelText` the one button spans the
    // width. Returns the Primary button.
    HWND layoutFooter(int okId, const std::string& okText, int cancelId,
                      const std::string& cancelText);
    // The client size in design px, and where the footer starts.
    int designW() const;
    int designH() const;
    int footerTop() const { return designH() - kSheetPad - tk::kTargetBig; }
    static constexpr int kSheetPad = tk::kSpace6;   // 32

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    int clientW_ = 0, clientH_ = 0;
    bool result_ = false;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    bool done_ = false;
};

// ---------------------------------------------------------------- prompts
// A question with two buttons that say what they do:
//   confirm(w, "Delete the 'Insurance note' button?", "", "Delete button",
//           "Keep it", true)
// True if the first one (`doIt`) was chosen. `danger` makes it a Danger
// button, and then Enter and the first focus go to `keep`, so nothing is
// deleted by a stray Enter.
bool confirm(HWND parent, const std::string& question,
             const std::string& detail, const std::string& doIt,
             const std::string& keep = "Cancel", bool danger = false);

enum class NoticeKind { Info, Problem };
// A heading, a sentence or two, and one button.
void notice(HWND parent, const std::string& heading, const std::string& text,
            NoticeKind kind = NoticeKind::Info,
            const std::string& closeText = "Close");

// The older name for notice(): `title` is the heading.
void showMessage(HWND parent, const std::string& title, const std::string& text,
                 bool error = false);
// One line of text: `prompt` is the heading, `label` names the box and
// `okText` what the button does.
bool askText(HWND parent, const std::string& title, const std::string& prompt,
             std::string& value, const std::string& hint = "",
             const std::string& okText = "Save",
             const std::string& label = "Name");

// Common control ids.
enum { ID_OK = 1, ID_CANCEL = 2, ID_FIRST_USER = 100 };

}  // namespace ui
