// The icon picker: the library laid out as a grid you click.
//
// Separate from iconlib.cpp on purpose. The library itself is data plus
// drawing and depends on nothing but GDI+, so the preview tool and the tests
// can link it without dragging the whole dialog layer in behind it.
#include "iconlib.h"

#include "dialogs.h"
#include "theme.h"
#include "ui_common.h"
#include "util.h"

#include <string>
#include <vector>
#include <windowsx.h>

using namespace Gdiplus;

namespace ui {

// The picker needs the list; iconLibrary() is the public way to it.
namespace {
const std::vector<LibIcon>& lib() { return iconLibrary(); }
}  // namespace

namespace {

// Design-pixel metrics. Eleven across keeps the whole library on a small
// laptop screen without scrolling, with room for "Microphone" under its icon.
constexpr int kCols = 11;
constexpr int kCellW = 80;
constexpr int kCell = 72;       // the height; the design system's 64 and a bit
constexpr int kCellGap = 6;
constexpr int kPad = tk::kSpace6;          // 32
constexpr int kGridTop = kPad + 34 + tk::kSpace5;

int rowsNeeded() { return ((int)lib().size() + kCols - 1) / kCols; }
int sheetW() { return kPad * 2 + kCols * kCellW + (kCols - 1) * kCellGap; }
int sheetH() {
    const int rows = rowsNeeded();
    return kGridTop + rows * kCell + (rows - 1) * kCellGap + tk::kSpace6 +
           tk::kTargetBig + kPad;
}

class IconPicker : public Modal {
public:
    explicit IconPicker(const std::string& current) : value_(current) {
        const std::string id = libraryIconId(current);
        for (size_t i = 0; i < lib().size(); ++i)
            if (id == lib()[i].id) sel_ = (int)i;
    }
    std::string value() const { return value_; }
    bool browseRequested() const { return browse_; }

protected:
    void build() override {
        makeText(hwnd_, "Choose an icon", kPad, kPad, sheetW() - kPad * 2, 34,
                 tk::Text::Title);
        // Four buttons along the bottom: the two other ways out - none at
        // all, and a picture of your own - then Cancel and the main one.
        const int y = footerTop();
        const int bw = (sheetW() - kPad * 2 - tk::kSpace3 * 3) / 4;
        struct B { const char* text; int id; Tone tone; tk::Text style; };
        const B buttons[] = {
            {"No icon", ID_NONE, Tone::Quiet, tk::Text::Button},
            {"Use a picture file", ID_BROWSE, Tone::Secondary, tk::Text::Button},
            {"Cancel", ID_CANCEL, Tone::Quiet, tk::Text::Button},
            {"Use this icon", ID_OK, Tone::Primary, tk::Text::ButtonBig},
        };
        for (int i = 0; i < 4; ++i) {
            HWND b = makeButton(hwnd_, buttons[i].text,
                                kPad + i * (bw + tk::kSpace3), y, bw,
                                tk::kTargetBig, buttons[i].id);
            setTone(b, buttons[i].tone);
            setTextStyle(b, buttons[i].style);
            if (buttons[i].id == ID_OK) ok_ = b;
        }
        updateOk();
    }

    void onCommand(int id, int) override {
        if (id == ID_BROWSE) { browse_ = true; close(true); }
        if (id == ID_NONE) { value_.clear(); close(true); }
    }

    bool onOk() override {
        if (sel_ < 0) return false;
        value_ = libraryIconRef(lib()[(size_t)sel_].id);
        return true;
    }

    bool onMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& out) override {
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            paint(dc);
            EndPaint(hwnd_, &ps);
            out = 0;
            return true;
        }
        if (msg == WM_ERASEBKGND) { out = 1; return true; }
        if (msg == WM_MOUSEMOVE) {
            int h = hit(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (h != hover_) { hover_ = h; InvalidateRect(hwnd_, nullptr, FALSE); }
            if (!tracking_) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd_, 0};
                tracking_ = TrackMouseEvent(&t) != FALSE;
            }
            out = 0;
            return true;
        }
        if (msg == WM_MOUSELEAVE) {
            tracking_ = false;
            if (hover_ >= 0) { hover_ = -1; InvalidateRect(hwnd_, nullptr, FALSE); }
            out = 0;
            return true;
        }
        if (msg == WM_LBUTTONDOWN) {
            int h = hit(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (h >= 0) {
                sel_ = h;
                updateOk();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            out = 0;
            return true;
        }
        if (msg == WM_LBUTTONDBLCLK) {
            int h = hit(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (h >= 0) { sel_ = h; close(onOk()); }
            out = 0;
            return true;
        }
        return false;
    }

private:
    enum { ID_BROWSE = ID_FIRST_USER, ID_NONE };

    void updateOk() {
        if (!ok_) return;
        EnableWindow(ok_, sel_ >= 0);
        setText(ok_, sel_ >= 0 ? "Use this icon" : "Pick an icon first");
    }

    RECT cellRect(int i) const {
        const int r = i / kCols, c = i % kCols;
        RECT rc;
        rc.left = S(kPad + c * (kCellW + kCellGap));
        rc.top = S(kGridTop + r * (kCell + kCellGap));
        rc.right = rc.left + S(kCellW);
        rc.bottom = rc.top + S(kCell);
        return rc;
    }

    int hit(POINT p) const {
        for (size_t i = 0; i < lib().size(); ++i) {
            RECT rc = cellRect((int)i);
            if (p.x >= rc.left && p.x < rc.right && p.y >= rc.top &&
                p.y < rc.bottom)
                return (int)i;
        }
        return -1;
    }

    void paint(HDC hdc) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        {
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            const Tok& k = tok();
            SolidBrush bg(k.surfaceRaised);
            g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

            const float radius = Sf((float)tk::kRadiusMd);
            for (size_t i = 0; i < lib().size(); ++i) {
                RECT cr = cellRect((int)i);
                RectF r((REAL)cr.left, (REAL)cr.top,
                        (REAL)(cr.right - cr.left), (REAL)(cr.bottom - cr.top));
                const bool on = sel_ == (int)i;
                const bool hov = hover_ == (int)i;
                if (on) {
                    fillRounded(g, r, radius, k.primaryTint);
                    strokeRounded(g, r, radius, k.primary, Sf((float)tk::kOutline));
                } else if (hov) {
                    fillRounded(g, r, radius, k.surfaceHover);
                }

                const float isz = Sf(30.0f);
                RectF box(r.X + (r.Width - isz) / 2, r.Y + Sf(10.0f), isz, isz);
                drawLibraryIcon(g, lib()[i].id, box, on ? k.primaryInk : k.ink);

                // The label may use the gap beside the cell too.
                RectF lab(r.X - Sf(3.0f), r.GetBottom() - Sf(26.0f),
                          r.Width + Sf(6.0f), Sf(20.0f));
                drawText(g, widen(lib()[i].label), lab, 13,
                         on ? k.primaryInk : k.inkMuted, Align::Center, on);

                // The tick, not only the colour, says which one is picked.
                if (on) {
                    const float d = Sf(20.0f);
                    RectF dot(r.GetRight() - d - Sf(4.0f), r.Y + Sf(4.0f), d, d);
                    SolidBrush fill(k.primary);
                    g.FillEllipse(&fill, dot);
                    RectF tick(dot.X + Sf(3.0f), dot.Y + Sf(3.0f), d - Sf(6.0f),
                               d - Sf(6.0f));
                    drawUiIcon(g, "check", tick, k.onPrimary);
                }
            }
        }
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    std::string value_;
    int sel_ = -1;
    int hover_ = -1;
    bool tracking_ = false;
    bool browse_ = false;
    HWND ok_ = nullptr;
};

}  // namespace

bool pickIconDialog(HWND parent, std::string& iconField) {
    IconPicker dlg(iconField);
    if (!dlg.run(parent, "Choose an icon", sheetW(), sheetH())) return false;
    if (dlg.browseRequested()) {
        // Hand straight over to the file picker, keeping whatever path was
        // already set as its starting point.
        std::string rel = isLibraryIcon(iconField) ? std::string() : iconField;
        if (!chooseImageFile(parent, rel)) return false;
        iconField = rel;
        return true;
    }
    iconField = dlg.value();
    return true;
}

}  // namespace ui
