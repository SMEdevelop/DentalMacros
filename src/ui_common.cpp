#include "ui_common.h"

#include "theme.h"
#include "util.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <cwchar>
#include <map>
#include <utility>

using namespace Gdiplus;

namespace ui {
namespace {

const wchar_t* kModalClass = L"DentalMacrosModal";
std::map<std::pair<int, int>, HFONT> g_fonts;     // (px, weight)
std::map<COLORREF, HBRUSH> g_brushes;

// Owner-drawn buttons remember whether they're the primary (accent) action.
const wchar_t* kPropPrimary = L"DentalMacrosPrimary";
const wchar_t* kPropDim = L"DentalMacrosDim";
const wchar_t* kPropTone = L"DentalMacrosTone";
const wchar_t* kPropHot = L"DentalMacrosHot";
const wchar_t* kPropIcon = L"DentalMacrosIcon";   // heap std::string*
const wchar_t* kPropSurface = L"DentalMacrosSurface";
const wchar_t* kPropFramed = L"DentalMacrosFramed";
const wchar_t* kPropPill = L"DentalMacrosPill";       // PillPart + 1
const wchar_t* kPropActive = L"DentalMacrosActive";
const wchar_t* kPropSwatch = L"DentalMacrosSwatch";   // COLORREF | 0x1000000
const wchar_t* kPropChoice = L"DentalMacrosChoice";   // heap std::string*: sub

// Every control records the *design* size of its font (and, for lists and
// combos, of its rows) so all of it can be rebuilt when the window moves to a
// monitor with different scaling. Without this the fonts would be frozen at
// whatever DPI the window happened to be created on.
const wchar_t* kPropFontPx = L"DentalMacrosFontPx";
const wchar_t* kPropFontWeight = L"DentalMacrosFontWeight";
const wchar_t* kPropItemH = L"DentalMacrosItemH";
const wchar_t* kPropError = L"DentalMacrosError";
const wchar_t* kPropChoiceStyle = L"DentalMacrosChoiceStyle";   // tk::Text + 1

void tagFont(HWND ctl, int sizePx, int weight) {
    SetPropW(ctl, kPropFontPx, (HANDLE)(INT_PTR)sizePx);
    SetPropW(ctl, kPropFontWeight, (HANDLE)(INT_PTR)weight);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)uiFontWeight(sizePx, weight), TRUE);
}

tk::TextSpec fontOf(HWND ctl) {
    tk::TextSpec s{14, 20, 400, 0};
    if (HANDLE px = GetPropW(ctl, kPropFontPx)) s.size = (int)(INT_PTR)px;
    if (HANDLE w = GetPropW(ctl, kPropFontWeight)) s.weight = (int)(INT_PTR)w;
    return s;
}

HBRUSH brushFor(COLORREF c) {
    auto it = g_brushes.find(c);
    if (it != g_brushes.end()) return it->second;
    HBRUSH b = CreateSolidBrush(c);
    g_brushes[c] = b;
    return b;
}

Color fromRef(COLORREF c) {
    return Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}

std::wstring windowText(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)std::max(0, n) + 1, L'\0');
    const int got = GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)std::max(0, got));
    return s;
}

// Paint into a memory bitmap, then copy: GDI+ straight onto a control's DC
// flickers.
template <class F>
void buffered(HDC hdc, const RECT& r, F&& draw) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        draw(g, RectF(0, 0, (REAL)w, (REAL)h));
    }
    BitBlt(hdc, r.left, r.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// Hover tracking for owner-drawn buttons: Windows tells a button nothing
// about the pointer, so it is watched here and the button repainted as it
// comes and goes.
LRESULT CALLBACK hoverProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                           UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_MOUSEMOVE:
            if (!GetPropW(hwnd, kPropHot)) {
                SetPropW(hwnd, kPropHot, (HANDLE)1);
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            RemovePropW(hwnd, kPropHot);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCDESTROY:
            if (auto* s = (std::string*)GetPropW(hwnd, kPropIcon)) {
                delete s;
                RemovePropW(hwnd, kPropIcon);
            }
            if (auto* s = (std::string*)GetPropW(hwnd, kPropChoice)) {
                delete s;
                RemovePropW(hwnd, kPropChoice);
            }
            RemovePropW(hwnd, kPropHot);
            RemoveWindowSubclass(hwnd, &hoverProc, 1);
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ---- text boxes and lists: a rounded 2px frame in the non-client area.
//
// The box keeps an inset all round (wider at the sides, and centring a
// single line vertically), so the control's own rectangle sits wholly inside
// the rounded shape and none of its square corners show.
struct Inset { int x, y; };

Inset frameInset(HWND hwnd, int height) {
    wchar_t cls[32] = {0};
    GetClassNameW(hwnd, cls, 31);
    const bool list = _wcsicmp(cls, L"LISTBOX") == 0;
    if (list) return {S(6), S(6)};
    const bool multi = (GetWindowLongPtrW(hwnd, GWL_STYLE) & ES_MULTILINE) != 0;
    if (multi) return {S(12), S(8)};
    // Single line: centre the text.
    int lineH = S(20);
    if (HFONT f = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0)) {
        HDC dc = GetDC(hwnd);
        HGDIOBJ o = SelectObject(dc, f);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, o);
        ReleaseDC(hwnd, dc);
        lineH = tm.tmHeight;
    }
    return {S(12), std::max(S(2), (height - lineH) / 2)};
}

float frameRadius(HWND hwnd, float height) {
    wchar_t cls[32] = {0};
    GetClassNameW(hwnd, cls, 31);
    const float r = _wcsicmp(cls, L"LISTBOX") == 0 ? Sf(12)
                                                   : Sf((float)tk::kRadiusMd);
    return std::min(r, height / 2);
}

void paintFrame(HWND hwnd) {
    RECT wr;
    GetWindowRect(hwnd, &wr);
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) return;
    const Inset in = frameInset(hwnd, h);

    HDC dc = GetWindowDC(hwnd);
    // Only the frame band: the client and any scrollbar are inside the inset.
    ExcludeClipRect(dc, in.x, in.y, w - in.x, h - in.y);
    buffered(dc, RECT{0, 0, w, h}, [&](Graphics& g, const RectF& r) {
        const Tok& k = tok();
        const bool focused = GetFocus() == hwnd;
        const bool enabled = IsWindowEnabled(hwnd) != FALSE;
        SolidBrush behind(fromRef(surfaceBehind(hwnd)));
        g.FillRectangle(&behind, r);
        const float radius = frameRadius(hwnd, r.Height);
        fillRounded(g, r, radius, enabled ? k.surfaceRaised : k.surfaceSunken);
        const bool error = GetPropW(hwnd, kPropError) != nullptr;
        strokeRounded(g, r, radius,
                      error ? k.danger : focused ? k.primary : k.lineStrong,
                      Sf((float)tk::kOutline));
    });
    ReleaseDC(hwnd, dc);
}

LRESULT CALLBACK frameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                           UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_NCCALCSIZE: {
            RECT* rc = wp ? &((NCCALCSIZE_PARAMS*)lp)->rgrc[0] : (RECT*)lp;
            const Inset in = frameInset(hwnd, rc->bottom - rc->top);
            InflateRect(rc, -in.x, -in.y);
            if (rc->right < rc->left) rc->right = rc->left;
            if (rc->bottom < rc->top) rc->bottom = rc->top;
            // The default then takes any scrollbar out of what is left.
            return DefSubclassProc(hwnd, msg, wp, lp);
        }
        case WM_NCPAINT: {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            paintFrame(hwnd);
            return r;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE: {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_FRAME | RDW_INVALIDATE | RDW_NOCHILDREN);
            return r;
        }
        case WM_NCDESTROY:
            RemovePropW(hwnd, kPropFramed);
            RemoveWindowSubclass(hwnd, &frameProc, 2);
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void addFrame(HWND ctl) {
    SetPropW(ctl, kPropFramed, (HANDLE)1);
    SetWindowSubclass(ctl, &frameProc, 2, 0);
    SetWindowPos(ctl, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE);
}

// ---- dropdowns: the closed box is painted here in full; the list that
// drops down is owner-drawn row by row (drawOwnerListItem).
LRESULT CALLBACK comboProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                           UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            buffered(hdc, rc, [&](Graphics& g, const RectF& r) {
                const Tok& k = tok();
                const bool open =
                    SendMessageW(hwnd, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
                const bool focused = GetFocus() == hwnd || open;
                const bool enabled = IsWindowEnabled(hwnd) != FALSE;
                SolidBrush behind(fromRef(surfaceBehind(hwnd)));
                g.FillRectangle(&behind, r);
                const float radius = std::min(Sf((float)tk::kRadiusMd), r.Height / 2);
                fillRounded(g, r, radius,
                            enabled ? k.surfaceRaised : k.surfaceSunken);
                strokeRounded(g, r, radius, focused ? k.primary : k.lineStrong,
                              Sf((float)tk::kOutline));

                std::wstring text;
                const LRESULT sel = SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR) {
                    const LRESULT len = SendMessageW(hwnd, CB_GETLBTEXTLEN, sel, 0);
                    if (len > 0) {
                        std::vector<wchar_t> buf((size_t)len + 1, L'\0');
                        SendMessageW(hwnd, CB_GETLBTEXT, sel, (LPARAM)buf.data());
                        text = buf.data();
                    }
                }
                const float chev = Sf(18);
                RectF tr(r.X + Sf(14), r.Y, r.Width - Sf(14) - chev - Sf(22),
                         r.Height);
                drawSpec(g, text, tr, fontOf(hwnd),
                         enabled ? k.ink : k.inkMuted, Align::Left);
                drawUiIcon(g, "chevron-down",
                           RectF(r.GetRight() - chev - Sf(14),
                                 r.Y + (r.Height - chev) / 2, chev, chev),
                           k.inkMuted);
            });
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP: {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            InvalidateRect(hwnd, nullptr, FALSE);
            return r;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, &comboProc, 3);
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

HFONT uiFontWeight(int sizePx, int weight) {
    if (weight <= 0) weight = 400;
    auto key = std::make_pair(sizePx, weight);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return it->second;
    // GDI knows the weights by number; Segoe UI has faces for 400, 600 and
    // 700, and draws 800 as its bold.
    HFONT f = CreateFontW(-S(sizePx), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    g_fonts[key] = f;
    return f;
}

HFONT uiFont(int sizePx, bool bold) {
    return uiFontWeight(sizePx, bold ? FW_SEMIBOLD : FW_NORMAL);
}

HFONT uiFont(tk::Text style) {
    const tk::TextSpec s = tk::spec(style);
    return uiFontWeight(s.size, s.weight);
}

void releaseFonts() {
    for (auto& kv : g_fonts) DeleteObject(kv.second);
    g_fonts.clear();
    for (auto& kv : g_brushes) DeleteObject(kv.second);
    g_brushes.clear();
}

void applyFrame(HWND hwnd) {
    // Light, always: DWMWA_USE_IMMERSIVE_DARK_MODE off (20 on current
    // builds, 19 on early 1809).
    BOOL dark = FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    // Windows 11 lets the caption take the design system's colours:
    // DWMWA_BORDER_COLOR 34, DWMWA_CAPTION_COLOR 35, DWMWA_TEXT_COLOR 36.
    // Older Windows ignores these and keeps its own light frame.
    COLORREF caption = toColorRef(tok().surfaceRaised);
    COLORREF text = toColorRef(tok().ink);
    COLORREF border = toColorRef(tok().line);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36, &text, sizeof(text));
    DwmSetWindowAttribute(hwnd, 34, &border, sizeof(border));
    SetWindowTheme(hwnd, L"Explorer", nullptr);
}

void setSurface(HWND window, COLORREF c) {
    // +1 in the top byte so that "set to black" and "not set" differ.
    SetPropW(window, kPropSurface, (HANDLE)(INT_PTR)((c & 0xFFFFFF) | 0x1000000));
    InvalidateRect(window, nullptr, TRUE);
}

COLORREF surfaceBehind(HWND ctl) {
    for (HWND h = ctl; h; h = GetParent(h)) {
        const INT_PTR v = (INT_PTR)GetPropW(h, kPropSurface);
        if (v) return (COLORREF)(v & 0xFFFFFF);
        if (!(GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD)) break;
    }
    return toColorRef(tok().surfaceRaised);
}

// ------------------------------------------------------------- control kit
HWND makeLabel(HWND parent, const std::string& text, int x, int y, int w, int h,
               int id, bool dim, bool bold) {
    HWND c = CreateWindowExW(0, L"STATIC", widen(text).c_str(),
                             WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY,
                             S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr),
                             nullptr);
    tagFont(c, bold ? 14 : 13, bold ? FW_SEMIBOLD : FW_NORMAL);
    if (dim) SetPropW(c, kPropDim, (HANDLE)1);
    return c;
}

HWND makeText(HWND parent, const std::string& text, int x, int y, int w, int h,
              tk::Text style, bool muted, int id) {
    HWND c = CreateWindowExW(0, L"STATIC", widen(text).c_str(),
                             WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY |
                                 SS_NOPREFIX,
                             S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr),
                             nullptr);
    setTextStyle(c, style);
    if (muted) SetPropW(c, kPropDim, (HANDLE)1);
    return c;
}

int textHeight(const std::string& text, tk::Text style, int width) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, uiFont(style));
    RECT r{0, 0, S(width), 0};
    std::wstring w = widen(text);
    DrawTextW(dc, w.c_str(), -1, &r,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return (int)((r.bottom - r.top) / dpiScale()) + 2;
}

void setTextStyle(HWND ctl, tk::Text style) {
    const tk::TextSpec s = tk::spec(style);
    tagFont(ctl, s.size, s.weight);
    if (GetPropW(ctl, kPropFramed))
        SetWindowPos(ctl, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE);
    InvalidateRect(ctl, nullptr, TRUE);
}

HWND makeEdit(HWND parent, const std::string& text, int x, int y, int w, int h,
              int id, bool multiline, bool readOnly) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
    if (multiline)
        style |= ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL;
    if (readOnly) style |= ES_READONLY;
    HWND c = CreateWindowExW(0, L"EDIT", widen(text).c_str(), style, S(x), S(y),
                             S(w), S(h), parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    tagFont(c, 14, FW_NORMAL);
    addFrame(c);
    return c;
}

Field makeField(HWND parent, const std::string& label, const std::string& value,
                const std::string& hint, int x, int y, int w, int id, int boxH,
                bool multiline) {
    Field f;
    f.hintText = hint;
    f.label = makeText(parent, label, x, y, w, 20, tk::Text::Label);
    y += 20 + 6;
    f.edit = makeEdit(parent, value, x, y, w, boxH, id, multiline);
    setTextStyle(f.edit, tk::Text::Body);
    y += boxH;
    if (!hint.empty()) {
        y += 6;
        f.hint = makeText(parent, hint, x, y, w,
                          textHeight(hint, tk::Text::Compact, w), tk::Text::Compact,
                          true);
    }
    return f;
}

int fieldHeight(const std::string& hint, int w, int boxH) {
    int h = 20 + 6 + boxH;
    if (!hint.empty()) h += 6 + textHeight(hint, tk::Text::Compact, w);
    return h;
}

void setFieldError(Field& f, const std::string& message) {
    const bool on = !message.empty();
    if (on) SetPropW(f.edit, kPropError, (HANDLE)1);
    else RemovePropW(f.edit, kPropError);
    RedrawWindow(f.edit, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
    if (!f.hint) return;
    if (on) SetPropW(f.hint, kPropError, (HANDLE)1);
    else RemovePropW(f.hint, kPropError);
    setTextStyle(f.hint, on ? tk::Text::CompactBold : tk::Text::Compact);
    setText(f.hint, on ? message : f.hintText);
}

HWND makeButton(HWND parent, const std::string& text, int x, int y, int w,
                int h, int id, bool primary) {
    HWND c = CreateWindowExW(0, L"BUTTON", widen(text).c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr),
                             nullptr);
    // Buttons are always the design system's 600 weight.
    tagFont(c, 14, FW_SEMIBOLD);
    if (primary) {
        SetPropW(c, kPropPrimary, (HANDLE)1);
        SetPropW(c, kPropTone, (HANDLE)(INT_PTR)((int)Tone::Primary + 1));
    }
    SetWindowSubclass(c, &hoverProc, 1, 0);
    return c;
}

void setButtonIcon(HWND button, const std::string& iconId) {
    if (auto* s = (std::string*)GetPropW(button, kPropIcon)) {
        delete s;
        RemovePropW(button, kPropIcon);
    }
    if (!iconId.empty()) SetPropW(button, kPropIcon, new std::string(iconId));
    InvalidateRect(button, nullptr, FALSE);
}

void makePill(HWND button, PillPart part) {
    SetPropW(button, kPropPill, (HANDLE)(INT_PTR)((int)part + 1));
    InvalidateRect(button, nullptr, FALSE);
}

void setPillActive(HWND button, bool active) {
    if (active) SetPropW(button, kPropActive, (HANDLE)1);
    else RemovePropW(button, kPropActive);
    InvalidateRect(button, nullptr, FALSE);
}

HWND makeSwatch(HWND parent, int x, int y, int size, int id,
                const std::string& hex, const std::string& name) {
    // The name is the button's text, so it is what a screen reader says and
    // what the tooltip-less swatch is called everywhere else.
    HWND c = makeButton(parent, name, x, y, size, size, id);
    tk::Rgb rgb;
    tk::parseHex(hex, rgb);
    SetPropW(c, kPropSwatch,
             (HANDLE)(INT_PTR)(RGB(rgb.r, rgb.g, rgb.b) | 0x1000000));
    return c;
}

HWND makeChoice(HWND parent, const std::string& title, const std::string& sub,
                int x, int y, int w, int h, int id) {
    HWND c = makeButton(parent, title, x, y, w, h, id);
    SetPropW(c, kPropChoice, new std::string(sub));
    return c;
}

void setChoiceSelected(HWND choice, bool selected) {
    setPillActive(choice, selected);
}

void setChoiceTitleStyle(HWND choice, tk::Text style) {
    SetPropW(choice, kPropChoiceStyle, (HANDLE)(INT_PTR)((int)style + 1));
    InvalidateRect(choice, nullptr, FALSE);
}

void setSwatchSelected(HWND swatch, bool selected) {
    setPillActive(swatch, selected);
}

void setComboHeight(HWND combo, int designPx) {
    // The closed box is the selection field plus the control's own few
    // pixels of border, so the field is set a little short of the target.
    SetPropW(combo, kPropItemH, (HANDLE)(INT_PTR)designPx);
    SendMessageW(combo, CB_SETITEMHEIGHT, (WPARAM)-1, S(designPx) - S(8));
    InvalidateRect(combo, nullptr, TRUE);
}

void setListRowHeight(HWND list, int designPx) {
    SetPropW(list, kPropItemH, (HANDLE)(INT_PTR)designPx);
    SendMessageW(list, LB_SETITEMHEIGHT, 0, S(designPx));
    InvalidateRect(list, nullptr, TRUE);
}

HWND makeCheck(HWND parent, const std::string& text, int x, int y, int w, int h,
               int id, bool checked) {
    // BS_OWNERDRAW so it matches the design system; the toggle is handled
    // manually.
    HWND c = CreateWindowExW(0, L"BUTTON", widen(text).c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr),
                             nullptr);
    tagFont(c, 14, FW_NORMAL);
    SetPropW(c, L"DentalMacrosCheck", (HANDLE)1);
    SetPropW(c, L"DentalMacrosChecked", (HANDLE)(INT_PTR)(checked ? 1 : 0));
    SetWindowSubclass(c, &hoverProc, 1, 0);
    return c;
}

HWND makeCombo(HWND parent, int x, int y, int w, int h, int id,
               const std::vector<std::string>& items, int selected) {
    HWND c = CreateWindowExW(
        0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
            CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        S(x), S(y), S(w), S(h) * 8, parent, (HMENU)(INT_PTR)id,
        GetModuleHandleW(nullptr), nullptr);
    tagFont(c, 14, FW_NORMAL);
    SetPropW(c, kPropItemH, (HANDLE)(INT_PTR)h);
    SendMessageW(c, CB_SETITEMHEIGHT, (WPARAM)-1, S(h) - S(8));
    SendMessageW(c, CB_SETITEMHEIGHT, 0, S(30));
    for (const auto& s : items)
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)widen(s).c_str());
    if (selected >= 0 && selected < (int)items.size())
        SendMessageW(c, CB_SETCURSEL, (WPARAM)selected, 0);
    SetWindowSubclass(c, &comboProc, 3, 0);
    return c;
}

HWND makeList(HWND parent, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(
        0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
        S(x), S(y), S(w), S(h), parent, (HMENU)(INT_PTR)id,
        GetModuleHandleW(nullptr), nullptr);
    tagFont(c, 14, FW_NORMAL);
    SetPropW(c, kPropItemH, (HANDLE)(INT_PTR)26);
    SendMessageW(c, LB_SETITEMHEIGHT, 0, S(26));
    addFrame(c);
    return c;
}

// ------------------------------------------------------------------ banner
namespace {

constexpr wchar_t kBannerClass[] = L"DentalMacrosBanner";

LRESULT CALLBACK bannerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_SETTEXT: {
            LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
            InvalidateRect(hwnd, nullptr, FALSE);
            return r;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int len = GetWindowTextLengthW(hwnd);
            std::wstring text((size_t)len + 1, L'\0');
            GetWindowTextW(hwnd, text.data(), len + 1);
            text.resize((size_t)len);
            const BannerKind kind =
                (BannerKind)(GetWindowLongPtrW(hwnd, GWLP_USERDATA) - 1);
            buffered(hdc, rc, [&](Graphics& g, const RectF& r) {
                SolidBrush behind(fromRef(surfaceBehind(hwnd)));
                g.FillRectangle(&behind, r);
                paintBanner(g, r, kind, text, 0);
            });
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BannerKind toKind(BannerTone t) {
    switch (t) {
        case BannerTone::Danger: return BannerKind::Danger;
        case BannerTone::Warning: return BannerKind::Warning;
        case BannerTone::Info: break;
    }
    return BannerKind::Info;
}

}  // namespace

HWND makeBanner(HWND parent, BannerTone tone, const std::string& text, int x,
                int y, int w) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &bannerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kBannerClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }
    HWND c = CreateWindowExW(0, kBannerClass, widen(text).c_str(),
                             WS_CHILD | WS_VISIBLE, S(x), S(y), S(w),
                             S(bannerHeight(text, w)), parent, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(c, GWLP_USERDATA, (LONG_PTR)toKind(tone) + 1);
    return c;
}

int bannerHeight(const std::string& text, int w) {
    // paintBanner: 16 in, a 22px icon, 12 gap, the text, 16 in; 17px bold.
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, uiFontWeight(17, 700));
    RECT r{0, 0, S(std::max(80, w - 66)), 0};
    std::wstring t = widen(text);
    DrawTextW(dc, t.c_str(), -1, &r,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    const int textH = (int)((r.bottom - r.top) / dpiScale()) + 2;
    return std::max(64, textH + 28);
}

void setBannerTone(HWND banner, BannerTone tone) {
    SetWindowLongPtrW(banner, GWLP_USERDATA, (LONG_PTR)toKind(tone) + 1);
    InvalidateRect(banner, nullptr, FALSE);
}

// ------------------------------------------------------------- DPI rescaling
namespace {

BOOL CALLBACK rescaleOne(HWND ctl, LPARAM) {
    if (HANDLE px = GetPropW(ctl, kPropFontPx)) {
        const int sizePx = (int)(INT_PTR)px;
        int weight = (int)(INT_PTR)GetPropW(ctl, kPropFontWeight);
        SendMessageW(ctl, WM_SETFONT, (WPARAM)uiFontWeight(sizePx, weight), TRUE);
    }
    if (HANDLE ih = GetPropW(ctl, kPropItemH)) {
        const int h = (int)(INT_PTR)ih;
        wchar_t cls[32] = {0};
        GetClassNameW(ctl, cls, 31);
        if (_wcsicmp(cls, L"COMBOBOX") == 0) {
            SendMessageW(ctl, CB_SETITEMHEIGHT, (WPARAM)-1, S(h) - S(8));
            SendMessageW(ctl, CB_SETITEMHEIGHT, 0, S(30));
        } else if (_wcsicmp(cls, L"LISTBOX") == 0) {
            SendMessageW(ctl, LB_SETITEMHEIGHT, 0, S(h));
        }
    }
    if (GetPropW(ctl, kPropFramed))
        SetWindowPos(ctl, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE);
    InvalidateRect(ctl, nullptr, TRUE);
    return TRUE;
}

}  // namespace

void rescaleControlFonts(HWND root) {
    // releaseFonts() destroys the handles the controls are still holding, so
    // the walk that hands out the replacements has to follow immediately -
    // no message pumping in between.
    releaseFonts();
    EnumChildWindows(root, &rescaleOne, 0);
    InvalidateRect(root, nullptr, TRUE);
}

// ----------------------------------------------------------- scrolling pane
namespace {

const wchar_t* kPaneClass = L"DentalMacrosPane";
const wchar_t* kPropContentH = L"DentalMacrosContentH";   // device px
const wchar_t* kPropScrollY = L"DentalMacrosScrollY";     // device px, +1 encoded

int paneScrollY(HWND pane) {
    return (int)(INT_PTR)GetPropW(pane, kPropScrollY);
}

void paneSetScrollY(HWND pane, int y) {
    RECT rc;
    GetClientRect(pane, &rc);
    const int viewH = rc.bottom;
    const int contentH = (int)(INT_PTR)GetPropW(pane, kPropContentH);
    const int maxY = std::max(0, contentH - viewH);
    y = std::max(0, std::min(y, maxY));

    const int old = paneScrollY(pane);
    if (y == old) return;
    SetPropW(pane, kPropScrollY, (HANDLE)(INT_PTR)y);
    // SW_SCROLLCHILDREN moves the controls with the surface, which is exactly
    // what we want - they keep their own painting and hit-testing.
    ScrollWindowEx(pane, 0, old - y, nullptr, nullptr, nullptr, nullptr,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    SetScrollPos(pane, SB_VERT, y, TRUE);
    UpdateWindow(pane);
}

LRESULT CALLBACK paneProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            paintPanelBackground(hwnd, (HDC)wp);
            return 1;

        // The controls inside belong to the dialog as far as it is concerned.
        case WM_COMMAND:
        case WM_NOTIFY:
        case WM_DRAWITEM:
        case WM_MEASUREITEM:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
            return SendMessageW(GetParent(hwnd), msg, wp, lp);

        case WM_VSCROLL: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int line = S(24), page = std::max(line, (int)rc.bottom - line);
            int y = paneScrollY(hwnd);
            switch (LOWORD(wp)) {
                case SB_LINEUP:   y -= line; break;
                case SB_LINEDOWN: y += line; break;
                case SB_PAGEUP:   y -= page; break;
                case SB_PAGEDOWN: y += page; break;
                case SB_TOP:      y = 0; break;
                case SB_BOTTOM:   y = INT_MAX / 2; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: {
                    SCROLLINFO si{sizeof(si), SIF_TRACKPOS, 0, 0, 0, 0, 0};
                    if (GetScrollInfo(hwnd, SB_VERT, &si)) y = si.nTrackPos;
                    break;
                }
                default: return 0;
            }
            paneSetScrollY(hwnd, y);
            return 0;
        }

        case WM_MOUSEWHEEL:
            paneSetScrollY(hwnd,
                           paneScrollY(hwnd) -
                               GET_WHEEL_DELTA_WPARAM(wp) * S(24) / WHEEL_DELTA);
            return 0;

        case WM_DESTROY:
            RemovePropW(hwnd, kPropContentH);
            RemovePropW(hwnd, kPropScrollY);
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND makeScrollPane(HWND parent, int x, int y, int w, int h, int id) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &paneProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kPaneClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }
    HWND pane = CreateWindowExW(
        WS_EX_CONTROLPARENT, kPaneClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL, S(x), S(y), S(w),
        S(h), parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    if (pane) SetWindowTheme(pane, L"Explorer", nullptr);
    return pane;
}

void setScrollPaneContent(HWND pane, int contentHeightDesignPx) {
    if (!pane) return;
    const int contentH = S(contentHeightDesignPx);
    SetPropW(pane, kPropContentH, (HANDLE)(INT_PTR)contentH);
    SetPropW(pane, kPropScrollY, (HANDLE)(INT_PTR)0);

    RECT rc;
    GetClientRect(pane, &rc);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, contentH - 1);
    si.nPage = (UINT)std::max(1L, rc.bottom);
    si.nPos = 0;
    SetScrollInfo(pane, SB_VERT, &si, TRUE);
    ShowScrollBar(pane, SB_VERT, contentH > rc.bottom);
    InvalidateRect(pane, nullptr, TRUE);
}

void scrollPaneToShow(HWND pane, HWND child) {
    if (!pane || !child) return;
    RECT pr, cr;
    GetClientRect(pane, &pr);
    GetWindowRect(child, &cr);
    MapWindowPoints(nullptr, pane, (POINT*)&cr, 2);
    const int margin = S(24);
    if (cr.top < margin || cr.bottom > pr.bottom - margin)
        paneSetScrollY(pane, paneScrollY(pane) + cr.top - margin);
}

std::string getText(HWND ctl) {
    int n = GetWindowTextLengthW(ctl);
    if (n <= 0) return "";
    std::wstring buf((size_t)n + 1, L'\0');
    int got = GetWindowTextW(ctl, &buf[0], n + 1);
    buf.resize((size_t)std::max(0, got));
    return narrow(buf);
}

void setText(HWND ctl, const std::string& text) {
    SetWindowTextW(ctl, widen(text).c_str());
}

bool isChecked(HWND ctl) {
    return GetPropW(ctl, L"DentalMacrosChecked") != nullptr;
}

void setChecked(HWND ctl, bool on) {
    SetPropW(ctl, L"DentalMacrosChecked", (HANDLE)(INT_PTR)(on ? 1 : 0));
    if (!on) RemovePropW(ctl, L"DentalMacrosChecked");
    InvalidateRect(ctl, nullptr, TRUE);
}

void setPrimary(HWND button, bool primary) {
    setTone(button, primary ? Tone::Primary : Tone::Quiet);
}

void setTone(HWND button, Tone t) {
    SetPropW(button, kPropTone, (HANDLE)(INT_PTR)((int)t + 1));
    if (t == Tone::Primary) SetPropW(button, kPropPrimary, (HANDLE)1);
    else RemovePropW(button, kPropPrimary);
    InvalidateRect(button, nullptr, TRUE);
}

void setDim(HWND label, bool dim) {
    if (dim) SetPropW(label, kPropDim, (HANDLE)1);
    else RemovePropW(label, kPropDim);
    InvalidateRect(label, nullptr, TRUE);
}

// ------------------------------------------------------------ owner drawing
namespace {

ButtonKind kindOf(HWND h) {
    const int raw = (int)(INT_PTR)GetPropW(h, kPropTone);
    const Tone t = raw > 0 ? (Tone)(raw - 1)
                           : (GetPropW(h, kPropPrimary) ? Tone::Primary
                                                        : Tone::Quiet);
    switch (t) {
        case Tone::Primary: return ButtonKind::Primary;
        case Tone::Secondary: return ButtonKind::Secondary;
        case Tone::Success: return ButtonKind::Success;
        case Tone::Danger: return ButtonKind::Danger;
        default: return ButtonKind::Quiet;
    }
}

void drawCheck(DRAWITEMSTRUCT* dis) {
    const bool on = GetPropW(dis->hwndItem, L"DentalMacrosChecked") != nullptr;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0 &&
                         (dis->itemState & ODS_NOFOCUSRECT) == 0;
    const std::wstring text = windowText(dis->hwndItem);
    const tk::TextSpec font = fontOf(dis->hwndItem);
    const Color behind = fromRef(surfaceBehind(dis->hwndItem));

    buffered(dis->hDC, dis->rcItem, [&](Graphics& g, const RectF& r) {
        const Tok& k = tok();
        SolidBrush bg(behind);
        g.FillRectangle(&bg, r);
        // Room for the focus ring around the box, inside the control.
        const float ring = Sf((float)(tk::kFocusRing + tk::kFocusGap));
        const float box = std::min(Sf(24), r.Height - ring * 2);
        RectF b(r.X + ring, r.Y + (r.Height - box) / 2, box, box);
        // A quarter of the box: 6px on the full 24px box, and still square
        // rather than round on the shorter rows older screens use.
        const float radius = box * 0.25f;
        if (focused) {
            RectF fr(b.X - ring, b.Y - ring, b.Width + ring * 2, b.Height + ring * 2);
            strokeRounded(g, fr, radius + ring, k.focusRing,
                          Sf((float)tk::kFocusRing));
        }
        if (on && !disabled) {
            fillRounded(g, b, radius, k.primary);
            RectF tick(b.X + box * 0.12f, b.Y + box * 0.12f, box * 0.76f,
                       box * 0.76f);
            drawUiIcon(g, "check", tick, k.onPrimary);
        } else {
            fillRounded(g, b, radius, disabled ? k.surfaceSunken : k.surfaceRaised);
            strokeRounded(g, b, radius, k.lineStrong, Sf((float)tk::kOutline));
            if (on) {
                RectF tick(b.X + box * 0.12f, b.Y + box * 0.12f, box * 0.76f,
                           box * 0.76f);
                drawUiIcon(g, "check", tick, k.inkMuted);
            }
        }
        RectF tr(b.GetRight() + Sf(12), r.Y, r.GetRight() - b.GetRight() - Sf(12),
                 r.Height);
        drawSpec(g, text, tr, font, disabled ? k.inkMuted : k.ink, Align::Left);
    });
}

void drawSwatch(DRAWITEMSTRUCT* dis, COLORREF c, const ButtonState& st) {
    const bool chosen = GetPropW(dis->hwndItem, kPropActive) != nullptr;
    const Color behind = fromRef(surfaceBehind(dis->hwndItem));
    buffered(dis->hDC, dis->rcItem, [&](Graphics& g, const RectF& r) {
        const Tok& k = tok();
        SolidBrush bg(behind);
        g.FillRectangle(&bg, r);
        const float side = std::min(r.Width, r.Height);
        RectF box(r.X + (r.Width - side) / 2, r.Y + (r.Height - side) / 2, side,
                  side);
        // Ring (focus, or hover, or chosen), a gap, then the colour.
        const float ring = Sf((float)tk::kFocusRing);
        const float gap = Sf((float)tk::kFocusGap);
        const Color ringC = st.focused ? k.focusRing
                            : chosen   ? k.ink
                            : st.hover ? k.primary
                                       : Color(0, 0, 0, 0);
        if (ringC.GetA()) {
            Pen p(ringC, ring);
            g.DrawEllipse(&p, RectF(box.X + ring / 2, box.Y + ring / 2,
                                    side - ring, side - ring));
        }
        const float in = ring + gap;
        RectF dot(box.X + in, box.Y + in, side - in * 2, side - in * 2);
        SolidBrush fill(fromRef(c));
        g.FillEllipse(&fill, dot);
        if (chosen) {
            const float t = dot.Width * 0.62f;
            drawUiIcon(g, "check",
                       RectF(dot.X + (dot.Width - t) / 2, dot.Y + (dot.Height - t) / 2,
                             t, t),
                       k.white);
        }
    });
}

void drawOwnerButton(DRAWITEMSTRUCT* dis) {
    if (GetPropW(dis->hwndItem, L"DentalMacrosCheck") != nullptr) {
        drawCheck(dis);
        return;
    }
    ButtonState st;
    st.disabled = (dis->itemState & ODS_DISABLED) != 0;
    st.pressed = (dis->itemState & ODS_SELECTED) != 0;
    // The ring shows for keyboard focus only: Windows sets ODS_NOFOCUSRECT
    // until the keyboard has been used in the window.
    st.focused = (dis->itemState & ODS_FOCUS) != 0 &&
                 (dis->itemState & ODS_NOFOCUSRECT) == 0;
    st.hover = GetPropW(dis->hwndItem, kPropHot) != nullptr;

    if (const INT_PTR sw = (INT_PTR)GetPropW(dis->hwndItem, kPropSwatch)) {
        drawSwatch(dis, (COLORREF)(sw & 0xFFFFFF), st);
        return;
    }

    const std::wstring text = windowText(dis->hwndItem);
    const tk::TextSpec font = fontOf(dis->hwndItem);
    const ButtonKind kind = kindOf(dis->hwndItem);
    const Color behind = fromRef(surfaceBehind(dis->hwndItem));
    std::string icon;
    if (auto* s = (std::string*)GetPropW(dis->hwndItem, kPropIcon)) icon = *s;

    if (auto* sub = (std::string*)GetPropW(dis->hwndItem, kPropChoice)) {
        ChoiceState cs;
        cs.selected = GetPropW(dis->hwndItem, kPropActive) != nullptr;
        cs.hover = st.hover && !st.disabled;
        cs.focused = st.focused;
        const std::wstring w = widen(*sub);
        buffered(dis->hDC, dis->rcItem, [&](Graphics& g, const RectF& r) {
            const INT_PTR style = (INT_PTR)GetPropW(dis->hwndItem, kPropChoiceStyle);
            paintChoiceTile(g, r, text, w, cs, behind,
                            style ? (tk::Text)(style - 1) : tk::Text::TileLabel);
        });
        return;
    }

    if (const INT_PTR part = (INT_PTR)GetPropW(dis->hwndItem, kPropPill)) {
        const bool active = GetPropW(dis->hwndItem, kPropActive) != nullptr;
        buffered(dis->hDC, dis->rcItem, [&](Graphics& g, const RectF& r) {
            paintPill(g, r, text, active, st.hover && !st.disabled, st.focused,
                      behind, icon, (int)part - 1);
            if (st.disabled) {
                // Greyed: a veil of the colour behind.
                SolidBrush veil(withAlpha(behind, 150));
                g.FillRectangle(&veil, r);
            }
        });
        return;
    }

    buffered(dis->hDC, dis->rcItem, [&](Graphics& g, const RectF& r) {
        paintButton(g, r, text, kind, st, behind, font, icon);
    });
}

void drawOwnerListItem(DRAWITEMSTRUCT* dis, bool isCombo) {
    if ((int)dis->itemID < 0) return;

    // LB_GETTEXT / CB_GETLBTEXT write as much as the item holds and take no
    // buffer size, so the buffer has to be sized from the item itself. Items
    // are built from macros.json, which users are invited to hand-edit, and a
    // fixed buffer here would be theirs to overrun.
    const UINT lenMsg = isCombo ? CB_GETLBTEXTLEN : LB_GETTEXTLEN;
    const UINT getMsg = isCombo ? CB_GETLBTEXT : LB_GETTEXT;
    LRESULT len = SendMessageW(dis->hwndItem, lenMsg, dis->itemID, 0);
    if (len == LB_ERR || len < 0) len = 0;
    std::vector<wchar_t> buf((size_t)len + 1, L'\0');
    if (len > 0) {
        LRESULT got = SendMessageW(dis->hwndItem, getMsg, dis->itemID,
                                   (LPARAM)buf.data());
        if (got == LB_ERR || got < 0) got = 0;
        buf[(size_t)std::min<LRESULT>(got, len)] = L'\0';
    }
    const std::wstring text = buf.data();
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    const tk::TextSpec font = fontOf(dis->hwndItem);

    buffered(dis->hDC, dis->rcItem, [&](Graphics& g, const RectF& r) {
        const Tok& k = tok();
        SolidBrush bg(k.surfaceRaised);
        g.FillRectangle(&bg, r);
        if (selected) {
            // A list row is chosen by shape as well as colour: the outline.
            RectF sel(r.X + Sf(1), r.Y + Sf(1), r.Width - Sf(2), r.Height - Sf(2));
            const float radius = std::min(Sf((float)tk::kRadiusSm), sel.Height / 2);
            fillRounded(g, sel, radius, k.primaryTint);
            if (!isCombo)
                strokeRounded(g, sel, radius, k.primary, Sf((float)tk::kOutline));
        }
        RectF tr(r.X + Sf(12), r.Y, r.Width - Sf(18), r.Height);
        drawSpec(g, text, tr, font, k.ink, Align::Left);
    });
}

}  // namespace

bool handleDrawItem(HWND, DRAWITEMSTRUCT* dis) {
    if (!dis) return false;
    if (dis->CtlType == ODT_BUTTON) {
        drawOwnerButton(dis);
        return true;
    }
    if (dis->CtlType == ODT_COMBOBOX) {
        // The closed box paints itself (comboProc); only the rows that drop
        // down come here.
        if (dis->itemState & ODS_COMBOBOXEDIT) return true;
        drawOwnerListItem(dis, true);
        return true;
    }
    if (dis->CtlType == ODT_LISTBOX) {
        drawOwnerListItem(dis, false);
        return true;
    }
    return false;
}

bool handleCtlColor(UINT msg, WPARAM wp, LPARAM lp, LRESULT& out) {
    const Tok& t = tok();
    HDC hdc = (HDC)wp;
    HWND ctl = (HWND)lp;
    switch (msg) {
        case WM_CTLCOLORSTATIC: {
            // Read-only text boxes ask here too; they sit in a raised field.
            wchar_t cls[32] = {0};
            GetClassNameW(ctl, cls, 31);
            const bool edit = _wcsicmp(cls, L"EDIT") == 0;
            const COLORREF bg = edit ? toColorRef(t.surfaceRaised)
                                     : surfaceBehind(ctl);
            const bool dim = GetPropW(ctl, kPropDim) != nullptr;
            const bool error = !edit && GetPropW(ctl, kPropError) != nullptr;
            SetTextColor(hdc, toColorRef(error ? t.dangerInk
                                         : dim ? t.inkMuted : t.ink));
            SetBkColor(hdc, bg);
            SetBkMode(hdc, OPAQUE);
            out = (LRESULT)brushFor(bg);
            return true;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            const COLORREF bg = toColorRef(t.surfaceRaised);
            SetTextColor(hdc, toColorRef(t.ink));
            SetBkColor(hdc, bg);
            out = (LRESULT)brushFor(bg);
            return true;
        }
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORDLG: {
            out = (LRESULT)brushFor(surfaceBehind(ctl));
            return true;
        }
    }
    return false;
}

void paintPanelBackground(HWND hwnd, HDC hdc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, brushFor(surfaceBehind(hwnd)));
}

// --------------------------------------------------------------- modal host
RECT workAreaNear(HWND w) {
    HMONITOR mon = w && IsWindow(w)
                       ? MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST)
                       : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcWork;
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN),
                GetSystemMetrics(SM_CYSCREEN)};
}

int availableClientHeight(HWND nearWindow) {
    RECT work = workAreaNear(nearWindow);
    RECT chrome{0, 0, 100, 100};
    AdjustWindowRectEx(&chrome, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, 0);
    const int overhead = (chrome.bottom - chrome.top) - 100;
    const int devicePx = (work.bottom - work.top) - overhead - S(24);
    return (int)(std::max(S(200), devicePx) / dpiScale());
}

Modal::~Modal() {}

LRESULT CALLBACK Modal::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Modal* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        self = (Modal*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = hwnd;
    } else {
        self = (Modal*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    LRESULT out = 0;
    if (self->onMessage(msg, wp, lp, out)) return out;

    switch (msg) {
        case WM_ERASEBKGND: {
            paintPanelBackground(hwnd, (HDC)wp);
            return 1;
        }
        case WM_DRAWITEM:
            if (handleDrawItem(hwnd, (DRAWITEMSTRUCT*)lp)) return TRUE;
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORDLG:
            if (handleCtlColor(msg, wp, lp, out)) return out;
            break;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            int notify = HIWORD(wp);
            // owner-drawn checkboxes toggle themselves
            HWND ctl = (HWND)lp;
            if (ctl && notify == BN_CLICKED &&
                GetPropW(ctl, L"DentalMacrosCheck") != nullptr)
                setChecked(ctl, !isChecked(ctl));
            if (id == ID_OK && notify == BN_CLICKED) {
                if (self->onOk()) self->close(true);
                return 0;
            }
            if (id == ID_CANCEL && notify == BN_CLICKED) {
                if (self->canCancel()) {
                    self->onCancel();
                    self->close(false);
                }
                return 0;
            }
            self->onCommand(id, notify);
            return 0;
        }
        case WM_SIZE:
            self->clientW_ = LOWORD(lp);
            self->clientH_ = HIWORD(lp);
            self->onSized(self->clientW_, self->clientH_);
            return 0;
        case WM_CLOSE:
            if (self->canCancel()) {
                self->onCancel();
                self->close(false);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Modal::close(bool ok) {
    result_ = ok;
    done_ = true;
}

int Modal::designW() const { return (int)(clientW_ / dpiScale() + 0.5); }
int Modal::designH() const { return (int)(clientH_ / dpiScale() + 0.5); }

HWND Modal::layoutFooter(int okId, const std::string& okText, int cancelId,
                         const std::string& cancelText) {
    const int pad = kSheetPad, y = footerTop();
    const int full = designW() - pad * 2;
    if (cancelText.empty()) {
        HWND ok = makeButton(hwnd_, okText, pad, y, full, tk::kTargetBig, okId,
                             true);
        setTextStyle(ok, tk::Text::ButtonBig);
        return ok;
    }
    const int bw = (full - tk::kSpace3) / 2;
    HWND cancel = makeButton(hwnd_, cancelText, pad, y, bw, tk::kTargetBig,
                             cancelId);
    setTextStyle(cancel, tk::Text::Button);
    HWND ok = makeButton(hwnd_, okText, pad + bw + tk::kSpace3, y, bw,
                         tk::kTargetBig, okId, true);
    setTextStyle(ok, tk::Text::ButtonBig);
    return ok;
}

bool Modal::run(HWND parent, const std::string& title, int w, int h) {
    parent_ = parent;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Modal::wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kModalClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        RegisterClassExW(&wc);
        registered = true;
    }

    RECT rc{0, 0, S(w), S(h)};
    // WS_CLIPCHILDREN: a dialog that repaints itself (a wizard drawing its
    // step badge, say) must not paint over the controls it holds.
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    int fullW = rc.right - rc.left, fullH = rc.bottom - rc.top;

    // Centre on the parent (or the screen if there isn't one).
    RECT pr{};
    if (parent && IsWindow(parent)) GetWindowRect(parent, &pr);
    else {
        pr.left = 0; pr.top = 0;
        pr.right = GetSystemMetrics(SM_CXSCREEN);
        pr.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int x = pr.left + ((pr.right - pr.left) - fullW) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - fullH) / 2;

    // ...but never off the edge of the monitor it lands on. A dialog whose
    // buttons sit below the taskbar cannot be dismissed with the mouse.
    RECT work = workAreaNear(parent ? parent : nullptr);
    fullH = std::min<int>(fullH, work.bottom - work.top);
    fullW = std::min<int>(fullW, work.right - work.left);
    x = std::max<int>(work.left, std::min<int>(x, work.right - fullW));
    y = std::max<int>(work.top, std::min<int>(y, work.bottom - fullH));

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                kModalClass, widen(title).c_str(), style, x, y,
                                fullW, fullH, parent, nullptr,
                                GetModuleHandleW(nullptr), this);
    if (!hwnd) return false;
    hwnd_ = hwnd;
    applyFrame(hwnd);

    RECT cr;
    GetClientRect(hwnd, &cr);
    clientW_ = cr.right;
    clientH_ = cr.bottom;

    build();

    if (parent && IsWindow(parent)) EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (!done_) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            // WM_QUIT: put it back so the outer loop also sees it.
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (msg.message == WM_KEYDOWN &&
            (msg.hwnd == hwnd || IsChild(hwnd, msg.hwnd))) {
            if (msg.wParam == VK_ESCAPE) {
                if (canCancel()) {
                    onCancel();
                    close(false);
                }
                continue;
            }
            if (msg.wParam == VK_RETURN) {
                // Enter inserts a newline in a multi-line edit; elsewhere it
                // confirms the dialog.
                bool multiline = false;
                wchar_t cls[32] = {0};
                GetClassNameW(msg.hwnd, cls, 31);
                if (_wcsicmp(cls, L"EDIT") == 0)
                    multiline = (GetWindowLongPtrW(msg.hwnd, GWL_STYLE) &
                                 ES_MULTILINE) != 0;
                if (!multiline) {
                    if (onOk()) close(true);
                    continue;
                }
            }
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (parent && IsWindow(parent)) {
        EnableWindow(parent, TRUE);
        SetActiveWindow(parent);
    }
    DestroyWindow(hwnd);
    hwnd_ = nullptr;
    return result_;
}

// ---------------------------------------------------------------- prompts
namespace {

// The shared shape of a confirm and a notice: a heading, a sentence or two,
// and a row of big buttons at the bottom with the main one last.
class PromptSheet : public Modal {
public:
    std::string heading, detail, doIt, keep;
    bool danger = false;
    bool problem = false;      // a notice about something that went wrong

    static constexpr int kW = 480;
    static constexpr int kPad = tk::kSpace6;   // 32

    int contentWidth() const { return kW - kPad * 2; }
    int headingX() const { return kPad + (problem ? 36 : 0); }
    int headingW() const { return contentWidth() - (problem ? 36 : 0); }

    int height() const {
        int h = kPad + textHeight(heading, tk::Text::Title, headingW());
        if (!detail.empty())
            h += tk::kSpace3 + textHeight(detail, tk::Text::Body, contentWidth());
        return h + tk::kSpace6 + tk::kTargetBig + kPad;
    }

    void build() override {
        int y = kPad;
        const int hh = textHeight(heading, tk::Text::Title, headingW());
        makeText(hwnd_, heading, headingX(), y, headingW(), hh, tk::Text::Title);
        headingTop_ = y;
        y += hh;
        if (!detail.empty()) {
            y += tk::kSpace3;
            const int dh = textHeight(detail, tk::Text::Body, contentWidth());
            makeText(hwnd_, detail, kPad, y, contentWidth(), dh, tk::Text::Body);
        }

        const int by = (int)(clientH_ / dpiScale()) - kPad - tk::kTargetBig;
        if (keep.empty()) {
            HWND b = makeButton(hwnd_, doIt, kPad, by, contentWidth(),
                                tk::kTargetBig, ID_OK, true);
            setTextStyle(b, tk::Text::ButtonBig);
            SetFocus(b);
        } else {
            const int bw = (contentWidth() - tk::kSpace3) / 2;
            keepBtn_ = makeButton(hwnd_, keep, kPad, by, bw, tk::kTargetBig,
                                  ID_CANCEL);
            setTextStyle(keepBtn_, tk::Text::Button);
            HWND b = makeButton(hwnd_, doIt, kPad + bw + tk::kSpace3, by, bw,
                                tk::kTargetBig, ID_OK);
            setTone(b, danger ? Tone::Danger : Tone::Primary);
            setTextStyle(b, tk::Text::ButtonBig);
            SetFocus(danger ? keepBtn_ : b);
        }
    }

    // Enter presses whichever button has the focus - for a delete, that
    // starts out as the one that keeps things.
    bool onOk() override {
        if (keepBtn_ && GetFocus() == keepBtn_) {
            close(false);
            return false;
        }
        return true;
    }

    bool onMessage(UINT msg, WPARAM, LPARAM, LRESULT& out) override {
        if (msg != WM_PAINT || !problem) return false;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd_, &ps);
        {
            Graphics g(hdc);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            const float sz = Sf(28);
            drawUiIcon(g, "alert",
                       RectF(Sf((float)kPad), Sf((float)headingTop_ + 2), sz, sz),
                       tok().dangerInk);
        }
        EndPaint(hwnd_, &ps);
        out = 0;
        return true;
    }

private:
    HWND keepBtn_ = nullptr;
    int headingTop_ = 0;
};

}  // namespace

bool confirm(HWND parent, const std::string& question, const std::string& detail,
             const std::string& doIt, const std::string& keep, bool danger) {
    PromptSheet p;
    p.heading = question;
    p.detail = detail;
    p.doIt = doIt;
    p.keep = keep.empty() ? "Cancel" : keep;
    p.danger = danger;
    return p.run(parent, "Dental Macros", PromptSheet::kW, p.height());
}

void notice(HWND parent, const std::string& heading, const std::string& text,
            NoticeKind kind, const std::string& closeText) {
    PromptSheet p;
    p.heading = heading;
    p.detail = text;
    p.doIt = closeText.empty() ? "Close" : closeText;
    p.problem = kind == NoticeKind::Problem;
    p.run(parent, "Dental Macros", PromptSheet::kW, p.height());
}

void showMessage(HWND parent, const std::string& title, const std::string& text,
                 bool error) {
    notice(parent, title, text, error ? NoticeKind::Problem : NoticeKind::Info);
}

namespace {
class TextPrompt : public Modal {
public:
    std::string heading, label, hint, value, okText;
    static constexpr int kW = 480;

    int headingH() const {
        return textHeight(heading, tk::Text::Title, kW - kSheetPad * 2);
    }
    int height() const {
        return kSheetPad + headingH() + tk::kSpace5 +
               fieldHeight(hint, kW - kSheetPad * 2) + tk::kSpace6 +
               tk::kTargetBig + kSheetPad;
    }

    void build() override {
        const int w = designW() - kSheetPad * 2;
        int y = kSheetPad;
        makeText(hwnd_, heading, kSheetPad, y, w, headingH(), tk::Text::Title);
        y += headingH() + tk::kSpace5;
        field_ = makeField(hwnd_, label, value, hint, kSheetPad, y, w,
                           ID_FIRST_USER);
        layoutFooter(ID_OK, okText, ID_CANCEL, "Cancel");
        SetFocus(field_.edit);
        SendMessageW(field_.edit, EM_SETSEL, 0, -1);
    }
    bool onOk() override {
        value = trim(getText(field_.edit));
        if (value.empty()) {
            setFieldError(field_, "Type something here first");
            SetFocus(field_.edit);
            return false;
        }
        return true;
    }

private:
    Field field_;
};
}  // namespace

bool askText(HWND parent, const std::string& title, const std::string& prompt,
             std::string& value, const std::string& hint,
             const std::string& okText, const std::string& label) {
    // The window title says what is being changed ("Rename button"), the
    // prompt is the heading, and the box carries its own label.
    TextPrompt p;
    p.heading = prompt;
    p.label = label.empty() ? "Name" : label;
    p.hint = hint;
    p.value = value;
    p.okText = okText.empty() ? "Save" : okText;
    if (!p.run(parent, title, TextPrompt::kW, p.height())) return false;
    value = p.value;
    return true;
}

}  // namespace ui
