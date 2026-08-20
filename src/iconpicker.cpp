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

// Design-pixel metrics for the grid.
constexpr int kCols = 8;
constexpr int kCellW = 64;
constexpr int kCellH = 62;
constexpr int kCellGap = 6;
constexpr int kMargin = 16;
constexpr int kTopHint = 34;

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
        layoutFooter(ID_OK, "Use this icon", ID_CANCEL, "Cancel");
        // Two ways out that are not "one of these 55": a picture of your own,
        // and none at all.
        makeButton(hwnd_, "Browse for an image file...", kMargin,
                   clientH_ - 46, 200, 30, ID_BROWSE);
        makeButton(hwnd_, "No icon", kMargin + 208, clientH_ - 46, 90, 30,
                   ID_NONE);
    }

    void onCommand(int id, int) override {
        if (id == ID_BROWSE) { browse_ = true; close(true); }
        if (id == ID_NONE) { value_.clear(); close(true); }
    }

    bool onOk() override {
        if (sel_ >= 0) value_ = libraryIconRef(lib()[(size_t)sel_].id);
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
            out = 0;
            return true;
        }
        if (msg == WM_LBUTTONDOWN) {
            int h = hit(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (h >= 0) { sel_ = h; InvalidateRect(hwnd_, nullptr, FALSE); }
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

    RECT cellRect(int i) const {
        const int r = i / kCols, c = i % kCols;
        RECT rc;
        rc.left = S(kMargin) + c * S(kCellW + kCellGap);
        rc.top = S(kTopHint) + r * S(kCellH + kCellGap);
        rc.right = rc.left + S(kCellW);
        rc.bottom = rc.top + S(kCellH);
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
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            SolidBrush bg(theme().panel);
            g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

            RectF hint((REAL)S(kMargin), (REAL)S(8),
                       (REAL)(rc.right - S(kMargin * 2)), (REAL)S(20));
            drawText(g, L"Pick an icon for this key, or bring your own picture.",
                     hint, 12, theme().textDim, Align::Left);

            for (size_t i = 0; i < lib().size(); ++i) {
                RECT cr = cellRect((int)i);
                RectF r((REAL)cr.left, (REAL)cr.top,
                        (REAL)(cr.right - cr.left), (REAL)(cr.bottom - cr.top));
                const bool on = sel_ == (int)i;
                const bool hov = hover_ == (int)i;
                if (on || hov)
                    fillRounded(g, r, Sf(7.0f),
                                on ? theme().accentDim : theme().tileHover);
                if (on) strokeRounded(g, r, Sf(7.0f), theme().accent, Sf(1.8f));

                RectF box(r.X + (r.Width - Sf(28.0f)) / 2, r.Y + Sf(8.0f),
                          Sf(28.0f), Sf(28.0f));
                drawLibraryIcon(g, lib()[i].id, box, theme().text);

                RectF lab(r.X + Sf(2.0f), r.GetBottom() - Sf(20.0f),
                          r.Width - Sf(4.0f), Sf(16.0f));
                drawText(g, widen(lib()[i].label), lab, 9,
                         on ? theme().text : theme().textDim, Align::Center);
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
    bool browse_ = false;
};

}  // namespace

bool pickIconDialog(HWND parent, std::string& iconField) {
    const int rows = ((int)lib().size() + kCols - 1) / kCols;
    const int w = kMargin * 2 + kCols * kCellW + (kCols - 1) * kCellGap;
    const int h = kTopHint + rows * (kCellH + kCellGap) + 66;

    IconPicker dlg(iconField);
    if (!dlg.run(parent, "Choose an icon", w, h)) return false;
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
