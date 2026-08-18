#include "minideck.h"

#include "gdiplus_inc.h"
#include "keys.h"
#include "theme.h"
#include "util.h"

#include <algorithm>
#include <map>
#include <windowsx.h>

using namespace Gdiplus;

namespace ui {
namespace {

constexpr wchar_t kClassName[] = L"MacroDeckMini";

// Design-pixel metrics. Deliberately tight - the whole point is a window that
// takes up less room than the icons already on the desktop.
constexpr int kPad = 5;
constexpr int kGap = 4;
constexpr int kGripH = 15;
constexpr int kFlashMs = 450;

enum {
    IDM_MINI_SMALL = 1,
    IDM_MINI_MEDIUM,
    IDM_MINI_LARGE,
    IDM_MINI_OPEN,
    IDM_MINI_HIDE,
    IDM_MINI_COLS = 20,      // + the column count
};

std::wstring W(const std::string& s) { return widen(s); }

RectF toRectF(const RECT& r) {
    return RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left),
                 (REAL)(r.bottom - r.top));
}

bool ptIn(const RECT& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

// The icons here are the same handful the main deck already loads, so this is
// a few bitmaps rather than anything worth managing carefully. Freed in
// destroy().
std::map<std::string, Image*>& iconCache() {
    static std::map<std::string, Image*> cache;
    return cache;
}

Image* iconFor(const std::string& relPath) {
    if (trim(relPath).empty()) return nullptr;
    auto& cache = iconCache();
    auto it = cache.find(relPath);
    if (it != cache.end()) return it->second;
    std::string full = joinPath(exeDir(), relPath);
    if (!fileExists(full)) full = relPath;
    Image* img = nullptr;
    if (fileExists(full)) {
        img = Image::FromFile(W(full).c_str());
        if (img && img->GetLastStatus() != Ok) {
            delete img;
            img = nullptr;
        }
    }
    cache[relPath] = img;
    return img;
}

}  // namespace

MiniDeck::~MiniDeck() { destroy(); }

// ------------------------------------------------------------------ content
void MiniDeck::setButtons(const std::string& caption,
                          std::vector<MiniButton> buttons) {
    caption_ = caption;
    buttons_ = std::move(buttons);
    if (hover_ >= (int)buttons_.size()) hover_ = -1;
    if (hwnd_) {
        relayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

MiniPlacement MiniDeck::placement() const {
    MiniPlacement p = place_;
    if (hwnd_) {
        RECT r{};
        GetWindowRect(hwnd_, &r);
        p.x = r.left;
        p.y = r.top;
        p.visible = IsWindowVisible(hwnd_) != 0;
    }
    return p;
}

void MiniDeck::setPlacement(const MiniPlacement& p) {
    place_ = p;
    place_.tile = std::max(kMiniTileSmall, std::min(kMiniTileLarge, place_.tile));
    place_.cols = std::max(kMiniColsMin, std::min(kMiniColsMax, place_.cols));
    if (hwnd_) relayout();
}

bool MiniDeck::visible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

// ------------------------------------------------------------------- window
bool MiniDeck::ensureWindow(HINSTANCE inst) {
    if (hwnd_) return true;
    inst_ = inst;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &MiniDeck::wndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }

    // No owner, on purpose. An owned window is hidden along with its owner,
    // and the main window spends most of its life hidden in the notification
    // area - which is exactly when this palette is the only way in.
    //
    // WS_EX_TOOLWINDOW keeps it out of the taskbar and Alt-Tab, where a
    // 200-pixel launcher has no business turning up.
    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName,
        L"MacroDeck buttons", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst,
        this);
    if (!hwnd_) return false;

    relayout();
    return true;
}

void MiniDeck::show(HINSTANCE inst) {
    if (!ensureWindow(inst)) return;
    relayout();
    // SW_SHOWNOACTIVATE, not SW_SHOW: appearing must not pull focus off
    // whatever the user is working in.
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    place_.visible = true;
    if (moved_) moved_();
}

void MiniDeck::hide() {
    if (!hwnd_) {
        place_.visible = false;
        return;
    }
    place_ = placement();
    place_.visible = false;
    ShowWindow(hwnd_, SW_HIDE);
    if (moved_) moved_();
}

void MiniDeck::toggle(HINSTANCE inst) {
    if (visible()) hide();
    else show(inst);
}

void MiniDeck::destroy() {
    if (hwnd_) {
        place_ = placement();
        HWND h = hwnd_;
        hwnd_ = nullptr;          // WM_DESTROY clears it too; be idempotent
        DestroyWindow(h);
    }
    for (auto& kv : iconCache()) delete kv.second;
    iconCache().clear();
}

// ------------------------------------------------------------------- layout
void MiniDeck::clampOnScreen(int& x, int& y, int w, int h) const {
    // Keep it on a monitor that actually exists. A position remembered from a
    // laptop docked to a second screen would otherwise come back off in the
    // void, with no title bar to drag it home by.
    POINT p{x + w / 2, y + h / 2};
    HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    const RECT& work = mi.rcWork;
    x = std::max<int>(work.left, std::min<int>(x, work.right - w));
    y = std::max<int>(work.top, std::min<int>(y, work.bottom - h));
}

void MiniDeck::relayout() {
    if (!hwnd_) return;

    const int tile = S(place_.tile);
    const int gap = S(kGap);
    const int pad = S(kPad);
    const int grip = S(kGripH);

    const int n = (int)buttons_.size();
    const int cols = std::max(1, std::min(place_.cols, std::max(1, n)));
    const int rows = n > 0 ? (n + cols - 1) / cols : 1;

    int contentW = cols * tile + (cols - 1) * gap;
    // An empty palette still has to be wide enough to say that it is empty.
    if (n == 0) contentW = std::max(contentW, S(190));
    const int w = contentW + pad * 2;
    const int h = grip + pad +
                  (n > 0 ? rows * tile + (rows - 1) * gap : S(22)) + pad;

    rects_.clear();
    rects_.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        const int r = i / cols, c = i % cols;
        RECT rc;
        rc.left = pad + c * (tile + gap);
        rc.top = grip + pad + r * (tile + gap);
        rc.right = rc.left + tile;
        rc.bottom = rc.top + tile;
        rects_.push_back(rc);
    }

    rcGrip_ = RECT{0, 0, w, grip};
    rcClose_ = RECT{w - grip, 0, w, grip};

    RECT cur{};
    GetWindowRect(hwnd_, &cur);
    int x = place_.x, y = place_.y;
    if (x == INT_MIN || y == INT_MIN) {
        // First time out: bottom-right of the work area, clear of the taskbar.
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        x = work.right - w - S(24);
        y = work.bottom - h - S(24);
    } else if (IsWindowVisible(hwnd_)) {
        // Already on screen: resizing must not teleport it back to the
        // remembered corner under the user.
        x = cur.left;
        y = cur.top;
    }
    clampOnScreen(x, y, w, h);
    place_.x = x;
    place_.y = y;

    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER);
}

int MiniDeck::hitTest(POINT pt) const {
    for (size_t i = 0; i < rects_.size(); ++i)
        if (ptIn(rects_[i], pt)) return (int)i;
    return -1;
}

// ------------------------------------------------------------------ painting
void MiniDeck::paintButton(void* graphics, size_t index) {
    Graphics& g = *reinterpret_cast<Graphics*>(graphics);
    const MiniButton& b = buttons_[index];
    RectF r = toRectF(rects_[index]);
    const float radius = Sf(6.0f);
    const bool hover = hover_ == (int)index;

    float fire = 0.0f;
    auto it = fired_.find(b.key);
    if (it != fired_.end()) {
        DWORD age = GetTickCount() - it->second;
        if (age < (DWORD)kFlashMs) fire = 1.0f - (float)age / (float)kFlashMs;
    }

    Color base = readableTileColor(b.color, hover ? theme().tileHover
                                                  : theme().tile);
    if (hover && trim(b.color).empty()) base = theme().tileHover;
    if (fire > 0) base = mix(base, theme().good, fire * 0.6f);
    // A key with nothing on it yet is drawn back towards the panel, so the
    // palette reads as "these do something" at a glance.
    if (!b.hasSteps) base = mix(base, theme().panel, 0.5f);

    fillRounded(g, r, radius, base);
    strokeRounded(g, r, radius,
                  hover ? theme().accent : withAlpha(theme().border, 150),
                  hover ? Sf(1.6f) : 1.0f);

    // At this size there is room for a glyph and a key name and nothing else.
    // The title goes in the grip strip on hover rather than being clipped to
    // three letters here.
    const float iconSize = r.Height * 0.42f;
    RectF iconBox(r.X + (r.Width - iconSize) / 2, r.Y + r.Height * 0.13f,
                  iconSize, iconSize);
    Image* img = iconFor(b.icon);
    if (img) {
        float iw = (float)img->GetWidth(), ih = (float)img->GetHeight();
        if (iw > 0 && ih > 0) {
            float scale = std::min(iconBox.Width / iw, iconBox.Height / ih);
            RectF dst(iconBox.X + (iconBox.Width - iw * scale) / 2,
                      iconBox.Y + (iconBox.Height - ih * scale) / 2, iw * scale,
                      ih * scale);
            g.DrawImage(img, dst);
        }
    } else if (!b.firstAction.empty()) {
        drawActionIcon(g, b.firstAction, iconBox,
                       withAlpha(Color(255, 255, 255, 255), 235));
    }

    RectF label(r.X + Sf(1.0f), r.GetBottom() - r.Height * 0.40f,
                r.Width - Sf(2.0f), r.Height * 0.34f);
    drawText(g, W(keys::displayName(b.key)), label,
             place_.tile <= kMiniTileSmall ? 8.0f : 9.0f,
             withAlpha(Color(255, 255, 255, 255), 225), Align::Center, true);
}

void MiniDeck::paint(HDC hdc) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        SolidBrush bg(theme().panel);
        g.FillRectangle(&bg, 0, 0, w, h);
        RectF frame(0.5f, 0.5f, (REAL)w - 1.0f, (REAL)h - 1.0f);
        strokeRounded(g, frame, Sf(7.0f), theme().border, 1.0f);

        // Grip strip. The whole strip drags, so there is no hunting for a
        // title bar that this window deliberately does not have.
        RectF gripR = toRectF(rcGrip_);
        SolidBrush gripBg(theme().panelAlt);
        g.FillRectangle(&gripBg, gripR);

        const float cy = gripR.Y + gripR.Height / 2;
        SolidBrush dot(theme().textFaint);
        for (int i = 0; i < 3; ++i)
            g.FillEllipse(&dot, Sf(6.0f) + i * Sf(4.0f), cy - Sf(1.0f),
                          Sf(2.0f), Sf(2.0f));

        // The hovered button's title, or the keyboard's name when nothing is
        // under the cursor. This is where a label too long for a 38-pixel
        // button gets to be read.
        std::string strip = caption_;
        if (hover_ >= 0 && hover_ < (int)buttons_.size()) {
            const MiniButton& b = buttons_[(size_t)hover_];
            strip = b.title.empty() ? keys::displayName(b.key) : b.title;
            if (!b.hasSteps) strip += " - no steps yet";
        }
        RectF capR(Sf(18.0f), gripR.Y, gripR.Width - Sf(18.0f) - Sf(kGripH),
                   gripR.Height);
        drawText(g, W(strip), capR, 9,
                 hover_ >= 0 ? theme().text : theme().textDim, Align::Left);

        RectF cl = toRectF(rcClose_);
        Pen x(hoverClose_ ? theme().danger : theme().textFaint, Sf(1.3f));
        const float m = Sf(4.5f);
        g.DrawLine(&x, cl.X + m, cl.Y + m, cl.GetRight() - m,
                   cl.GetBottom() - m);
        g.DrawLine(&x, cl.GetRight() - m, cl.Y + m, cl.X + m,
                   cl.GetBottom() - m);

        if (buttons_.empty()) {
            RectF empty(Sf(8.0f), (REAL)S(kGripH), (REAL)w - Sf(16.0f),
                        (REAL)h - (REAL)S(kGripH));
            drawText(g, L"No keys on this keyboard yet", empty, 10,
                     theme().textFaint, Align::Center);
        } else {
            for (size_t i = 0; i < buttons_.size(); ++i) paintButton(&g, i);
        }
    }
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// -------------------------------------------------------------------- events
void MiniDeck::noteFired(const std::string& key) {
    fired_[key] = GetTickCount();
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        startFlashTimer();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MiniDeck::startFlashTimer() {
    if (flashTimer_ || !hwnd_) return;
    SetTimer(hwnd_, 1, 60, nullptr);
    flashTimer_ = true;
}

void MiniDeck::contextMenu(POINT screenPt) {
    HMENU m = CreatePopupMenu();
    HMENU size = CreatePopupMenu();
    AppendMenuW(size,
                MF_STRING | (place_.tile == kMiniTileSmall ? MF_CHECKED : 0),
                IDM_MINI_SMALL, L"Small");
    AppendMenuW(size,
                MF_STRING | (place_.tile == kMiniTileMedium ? MF_CHECKED : 0),
                IDM_MINI_MEDIUM, L"Medium");
    AppendMenuW(size,
                MF_STRING | (place_.tile == kMiniTileLarge ? MF_CHECKED : 0),
                IDM_MINI_LARGE, L"Large");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)size, L"Button size");

    HMENU cols = CreatePopupMenu();
    for (int c = kMiniColsMin; c <= kMiniColsMax; ++c) {
        wchar_t buf[16];
        wsprintfW(buf, L"%d", c);
        AppendMenuW(cols, MF_STRING | (place_.cols == c ? MF_CHECKED : 0),
                    IDM_MINI_COLS + c, buf);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)cols, L"Buttons per row");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_MINI_OPEN, L"Open MacroDeck");
    AppendMenuW(m, MF_STRING, IDM_MINI_HIDE, L"Hide these buttons");

    // A menu put up by a window that never takes focus needs the foreground
    // nudge, or it will not close when the user clicks away from it.
    SetForegroundWindow(hwnd_);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  screenPt.x, screenPt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);

    if (cmd == IDM_MINI_SMALL) place_.tile = kMiniTileSmall;
    else if (cmd == IDM_MINI_MEDIUM) place_.tile = kMiniTileMedium;
    else if (cmd == IDM_MINI_LARGE) place_.tile = kMiniTileLarge;
    else if (cmd > IDM_MINI_COLS && cmd <= IDM_MINI_COLS + kMiniColsMax)
        place_.cols = cmd - IDM_MINI_COLS;
    else if (cmd == IDM_MINI_OPEN) { if (open_) open_(); return; }
    else if (cmd == IDM_MINI_HIDE) { hide(); return; }
    else return;

    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (moved_) moved_();
}

LRESULT CALLBACK MiniDeck::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MiniDeck* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<MiniDeck*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MiniDeck*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(hwnd, msg, wp, lp);
}

LRESULT MiniDeck::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   // WM_PAINT covers every pixel

    // Clicking a button must not take the foreground away from the window the
    // macro is about to type into.
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_MOUSEMOVE: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (dragging_) {
            POINT screen = p;
            ClientToScreen(hwnd, &screen);
            RECT r{};
            GetWindowRect(hwnd, &r);
            int x = screen.x - dragGrab_.x;
            int y = screen.y - dragGrab_.y;
            clampOnScreen(x, y, r.right - r.left, r.bottom - r.top);
            SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        int h = hitTest(p);
        bool hc = ptIn(rcClose_, p);
        if (h != hover_ || hc != hoverClose_) {
            hover_ = h;
            hoverClose_ = hc;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hover_ != -1 || hoverClose_) {
            hover_ = -1;
            hoverClose_ = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (ptIn(rcClose_, p)) { hide(); return 0; }
        if (ptIn(rcGrip_, p)) {
            dragging_ = true;
            dragGrab_ = p;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
            place_ = placement();
            if (moved_) moved_();
            return 0;
        }
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int i = hitTest(p);
        if (i >= 0 && i < (int)buttons_.size()) {
            const MiniButton& b = buttons_[(size_t)i];
            if (b.hasSteps && run_) {
                noteFired(b.key);
                run_(b.key);
            }
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &p);
        contextMenu(p);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            DWORD now = GetTickCount();
            bool any = false;
            for (auto it = fired_.begin(); it != fired_.end();) {
                if (now - it->second >= (DWORD)kFlashMs) it = fired_.erase(it);
                else { any = true; ++it; }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            if (!any) {
                KillTimer(hwnd, 1);
                flashTimer_ = false;
            }
            return 0;
        }
        break;

    case WM_DESTROY:
        if (flashTimer_) { KillTimer(hwnd, 1); flashTimer_ = false; }
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace ui
