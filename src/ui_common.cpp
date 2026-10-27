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

namespace ui {
namespace {

const wchar_t* kModalClass = L"DentalMacrosModal";
std::map<std::pair<int, bool>, HFONT> g_fonts;
HBRUSH g_panelBrush = nullptr;
HBRUSH g_inputBrush = nullptr;

// Owner-drawn buttons remember whether they're the primary (accent) action.
const wchar_t* kPropPrimary = L"DentalMacrosPrimary";
const wchar_t* kPropDim = L"DentalMacrosDim";

// Every control records the *design* size of its font (and, for lists and
// combos, of its rows) so all of it can be rebuilt when the window moves to a
// monitor with different scaling. Without this the fonts would be frozen at
// whatever DPI the window happened to be created on.
const wchar_t* kPropFontPx = L"DentalMacrosFontPx";
const wchar_t* kPropFontBold = L"DentalMacrosFontBold";
const wchar_t* kPropItemH = L"DentalMacrosItemH";

void tagFont(HWND ctl, int sizePx, bool bold) {
    SetPropW(ctl, kPropFontPx, (HANDLE)(INT_PTR)sizePx);
    SetPropW(ctl, kPropFontBold, (HANDLE)(INT_PTR)(bold ? 1 : 0));
    SendMessageW(ctl, WM_SETFONT, (WPARAM)uiFont(sizePx, bold), TRUE);
}

HBRUSH panelBrush() {
    if (!g_panelBrush) g_panelBrush = CreateSolidBrush(toColorRef(theme().panel));
    return g_panelBrush;
}
HBRUSH inputBrush() {
    if (!g_inputBrush) g_inputBrush = CreateSolidBrush(toColorRef(theme().bg));
    return g_inputBrush;
}

}  // namespace

HFONT uiFont(int sizePx, bool bold) {
    auto key = std::make_pair(sizePx, bold);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return it->second;
    HFONT f = CreateFontW(-S(sizePx), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
                          FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    g_fonts[key] = f;
    return f;
}

void releaseFonts() {
    for (auto& kv : g_fonts) DeleteObject(kv.second);
    g_fonts.clear();
    if (g_panelBrush) { DeleteObject(g_panelBrush); g_panelBrush = nullptr; }
    if (g_inputBrush) { DeleteObject(g_inputBrush); g_inputBrush = nullptr; }
}

void applyDarkFrame(HWND hwnd) {
    // DWMWA_USE_IMMERSIVE_DARK_MODE. 20 on current builds, 19 on early 1809.
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
}

// ------------------------------------------------------------- control kit
HWND makeLabel(HWND parent, const std::string& text, int x, int y, int w, int h,
               int id, bool dim, bool bold) {
    HWND c = CreateWindowExW(0, L"STATIC", widen(text).c_str(),
                             WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY,
                             S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr),
                             nullptr);
    tagFont(c, bold ? 14 : 13, bold);
    if (dim) SetPropW(c, kPropDim, (HANDLE)1);
    return c;
}

HWND makeEdit(HWND parent, const std::string& text, int x, int y, int w, int h,
              int id, bool multiline, bool readOnly) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
    if (multiline)
        style |= ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL;
    if (readOnly) style |= ES_READONLY;
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", widen(text).c_str(),
                             style, S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr),
                             nullptr);
    tagFont(c, 14, false);
    return c;
}

HWND makeButton(HWND parent, const std::string& text, int x, int y, int w,
                int h, int id, bool primary) {
    HWND c = CreateWindowExW(0, L"BUTTON", widen(text).c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr),
                             nullptr);
    tagFont(c, 14, false);
    if (primary) SetPropW(c, kPropPrimary, (HANDLE)1);
    return c;
}

HWND makeCheck(HWND parent, const std::string& text, int x, int y, int w, int h,
               int id, bool checked) {
    // BS_OWNERDRAW so it matches the theme; the toggle is handled manually.
    HWND c = CreateWindowExW(0, L"BUTTON", widen(text).c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr),
                             nullptr);
    tagFont(c, 14, false);
    SetPropW(c, L"DentalMacrosCheck", (HANDLE)1);
    SetPropW(c, L"DentalMacrosChecked", (HANDLE)(INT_PTR)(checked ? 1 : 0));
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
    tagFont(c, 14, false);
    SetPropW(c, kPropItemH, (HANDLE)(INT_PTR)h);
    SendMessageW(c, CB_SETITEMHEIGHT, (WPARAM)-1, S(h) - S(8));
    SendMessageW(c, CB_SETITEMHEIGHT, 0, S(22));
    for (const auto& s : items)
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)widen(s).c_str());
    if (selected >= 0 && selected < (int)items.size())
        SendMessageW(c, CB_SETCURSEL, (WPARAM)selected, 0);
    return c;
}

HWND makeList(HWND parent, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        S(x), S(y), S(w), S(h), parent, (HMENU)(INT_PTR)id,
        GetModuleHandleW(nullptr), nullptr);
    tagFont(c, 14, false);
    SetPropW(c, kPropItemH, (HANDLE)(INT_PTR)26);
    SendMessageW(c, LB_SETITEMHEIGHT, 0, S(26));
    return c;
}

// ------------------------------------------------------------- DPI rescaling
namespace {

BOOL CALLBACK rescaleOne(HWND ctl, LPARAM) {
    if (HANDLE px = GetPropW(ctl, kPropFontPx)) {
        const int sizePx = (int)(INT_PTR)px;
        const bool bold = GetPropW(ctl, kPropFontBold) != nullptr;
        SendMessageW(ctl, WM_SETFONT, (WPARAM)uiFont(sizePx, bold), TRUE);
    }
    if (HANDLE ih = GetPropW(ctl, kPropItemH)) {
        const int h = (int)(INT_PTR)ih;
        wchar_t cls[32] = {0};
        GetClassNameW(ctl, cls, 31);
        if (_wcsicmp(cls, L"COMBOBOX") == 0) {
            SendMessageW(ctl, CB_SETITEMHEIGHT, (WPARAM)-1, S(h) - S(8));
            SendMessageW(ctl, CB_SETITEMHEIGHT, 0, S(22));
        } else if (_wcsicmp(cls, L"LISTBOX") == 0) {
            SendMessageW(ctl, LB_SETITEMHEIGHT, 0, S(h));
        }
    }
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
    if (pane) applyDarkFrame(pane);
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
    InvalidateRect(ctl, nullptr, TRUE);
}

void setPrimary(HWND button, bool primary) {
    setTone(button, primary ? Tone::Primary : Tone::Normal);
}

void setTone(HWND button, Tone t) {
    SetPropW(button, L"DentalMacrosTone", (HANDLE)(INT_PTR)((int)t + 1));
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

void drawRoundRectGdi(HDC hdc, RECT r, int radius, COLORREF fill,
                      COLORREF border) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(hdc, b);
    HGDIOBJ op = SelectObject(hdc, p);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(b);
    DeleteObject(p);
}

void drawOwnerButton(DRAWITEMSTRUCT* dis) {
    const Theme& t = theme();
    bool isCheck = GetPropW(dis->hwndItem, L"DentalMacrosCheck") != nullptr;
    bool primary = GetPropW(dis->hwndItem, kPropPrimary) != nullptr;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool focused = (dis->itemState & ODS_FOCUS) != 0;

    wchar_t text[512] = {0};
    GetWindowTextW(dis->hwndItem, text, 511);

    RECT r = dis->rcItem;
    SetBkMode(dis->hDC, TRANSPARENT);
    HFONT font = (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = SelectObject(dis->hDC, font);

    if (isCheck) {
        FillRect(dis->hDC, &r, panelBrush());
        bool on = GetPropW(dis->hwndItem, L"DentalMacrosChecked") != nullptr;
        int boxSize = S(16);
        RECT box{r.left, r.top + (r.bottom - r.top - boxSize) / 2,
                 r.left + boxSize, r.top + (r.bottom - r.top + boxSize) / 2};
        drawRoundRectGdi(dis->hDC, box, S(4),
                         on ? toColorRef(t.accent) : toColorRef(t.bg),
                         toColorRef(on ? t.accent : t.border));
        if (on) {
            HPEN pen = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(255, 255, 255));
            HGDIOBJ op = SelectObject(dis->hDC, pen);
            POINT tick[3] = {
                {box.left + boxSize / 4, box.top + boxSize / 2},
                {box.left + boxSize / 2 - S(1), box.bottom - boxSize / 3},
                {box.right - boxSize / 5, box.top + boxSize / 3}};
            Polyline(dis->hDC, tick, 3);
            SelectObject(dis->hDC, op);
            DeleteObject(pen);
        }
        RECT tr{box.right + S(8), r.top, r.right, r.bottom};
        SetTextColor(dis->hDC, toColorRef(disabled ? t.textFaint : t.text));
        DrawTextW(dis->hDC, text, -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (focused) {
            RECT fr = r;
            InflateRect(&fr, -1, -1);
            SetTextColor(dis->hDC, toColorRef(t.accent));
            DrawFocusRect(dis->hDC, &fr);
        }
        SelectObject(dis->hDC, oldFont);
        return;
    }

    int toneRaw = (int)(INT_PTR)GetPropW(dis->hwndItem, L"DentalMacrosTone");
    Tone tone = toneRaw > 0 ? (Tone)(toneRaw - 1)
                            : (primary ? Tone::Primary : Tone::Normal);

    COLORREF fill, borderCol, textCol;
    if (disabled) {
        fill = toColorRef(t.panelAlt);
        borderCol = toColorRef(t.borderSoft);
        textCol = toColorRef(t.textFaint);
    } else if (tone != Tone::Normal) {
        Gdiplus::Color base = tone == Tone::Good     ? t.good
                              : tone == Tone::Danger ? t.danger
                                                     : t.accent;
        fill = toColorRef(pressed
                              ? mix(base, Gdiplus::Color(255, 0, 0, 0), 0.25f)
                              : base);
        borderCol = fill;
        textCol = RGB(255, 255, 255);
    } else {
        fill = toColorRef(pressed ? t.tile : t.panelAlt);
        borderCol = toColorRef(focused ? t.accent : t.border);
        textCol = toColorRef(t.text);
    }
    FillRect(dis->hDC, &r, panelBrush());
    drawRoundRectGdi(dis->hDC, r, S(6), fill, borderCol);
    SetTextColor(dis->hDC, textCol);
    DrawTextW(dis->hDC, text, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dis->hDC, oldFont);
}

void drawOwnerListItem(DRAWITEMSTRUCT* dis, bool isCombo) {
    const Theme& t = theme();
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
    const wchar_t* text = buf.data();

    bool selected = (dis->itemState & ODS_SELECTED) != 0;
    HBRUSH bg = CreateSolidBrush(
        toColorRef(selected ? t.accentDim : (isCombo ? t.panelAlt : t.bg)));
    FillRect(dis->hDC, &dis->rcItem, bg);
    DeleteObject(bg);

    if (selected && !isCombo) {
        RECT bar = dis->rcItem;
        bar.right = bar.left + S(3);
        HBRUSH ab = CreateSolidBrush(toColorRef(t.accent));
        FillRect(dis->hDC, &bar, ab);
        DeleteObject(ab);
    }

    RECT tr = dis->rcItem;
    tr.left += S(10);
    tr.right -= S(6);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, toColorRef(t.text));
    HFONT font = (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = SelectObject(dis->hDC, font);
    DrawTextW(dis->hDC, text, -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dis->hDC, oldFont);
}

}  // namespace

bool handleDrawItem(HWND, DRAWITEMSTRUCT* dis) {
    if (!dis) return false;
    if (dis->CtlType == ODT_BUTTON) {
        drawOwnerButton(dis);
        return true;
    }
    if (dis->CtlType == ODT_COMBOBOX) {
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
    const Theme& t = theme();
    HDC hdc = (HDC)wp;
    switch (msg) {
        case WM_CTLCOLORSTATIC: {
            bool dim = GetPropW((HWND)lp, kPropDim) != nullptr;
            SetTextColor(hdc, toColorRef(dim ? t.textDim : t.text));
            SetBkColor(hdc, toColorRef(t.panel));
            SetBkMode(hdc, OPAQUE);
            out = (LRESULT)panelBrush();
            return true;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            SetTextColor(hdc, toColorRef(t.text));
            SetBkColor(hdc, toColorRef(t.bg));
            out = (LRESULT)inputBrush();
            return true;
        }
        case WM_CTLCOLORBTN: {
            out = (LRESULT)panelBrush();
            return true;
        }
        case WM_CTLCOLORDLG: {
            out = (LRESULT)panelBrush();
            return true;
        }
    }
    return false;
}

void paintPanelBackground(HWND hwnd, HDC hdc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, panelBrush());
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

void Modal::layoutFooter(int okId, const std::string& okText, int cancelId,
                         const std::string& cancelText) {
    // Control helpers take design pixels, so convert the client size back out
    // of device pixels before positioning.
    const int bw = 104, bh = 32, pad = 16;
    int y = (int)(clientH_ / dpiScale()) - bh - pad;
    int x = (int)(clientW_ / dpiScale()) - bw - pad;
    if (!cancelText.empty()) {
        makeButton(hwnd_, okText, x, y, bw, bh, okId, true);
        makeButton(hwnd_, cancelText, x - bw - 8, y, bw, bh, cancelId, false);
    } else {
        makeButton(hwnd_, okText, x, y, bw, bh, okId, true);
    }
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
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
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
    applyDarkFrame(hwnd);

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

// -------------------------------------------------------------- messages
void showMessage(HWND parent, const std::string& title, const std::string& text,
                 bool error) {
    MessageBoxW(parent, widen(text).c_str(), widen(title).c_str(),
                MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

bool askYesNo(HWND parent, const std::string& title, const std::string& text) {
    return MessageBoxW(parent, widen(text).c_str(), widen(title).c_str(),
                       MB_YESNO | MB_ICONQUESTION) == IDYES;
}

namespace {
class TextPrompt : public Modal {
public:
    std::string prompt, hint, value;

    void build() override {
        makeLabel(hwnd_, prompt, 16, 16, 348, 20);
        edit_ = makeEdit(hwnd_, value, 16, 42, 348, 26, ID_FIRST_USER);
        if (!hint.empty()) makeLabel(hwnd_, hint, 16, 74, 348, 34, -1, true);
        layoutFooter(ID_OK, "OK", ID_CANCEL, "Cancel");
        SetFocus(edit_);
        SendMessageW(edit_, EM_SETSEL, 0, -1);
    }
    bool onOk() override {
        value = trim(getText(edit_));
        return !value.empty();
    }

private:
    HWND edit_ = nullptr;
};
}  // namespace

bool askText(HWND parent, const std::string& title, const std::string& prompt,
             std::string& value, const std::string& hint) {
    TextPrompt p;
    p.prompt = prompt;
    p.hint = hint;
    p.value = value;
    if (!p.run(parent, title, 380, hint.empty() ? 150 : 186)) return false;
    value = p.value;
    return true;
}

}  // namespace ui
