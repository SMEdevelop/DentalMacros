#include "dialogs.h"

#include "device.h"
#include "discover.h"
#include "keys.h"
#include "recorder.h"
#include "theme.h"
#include "ui_common.h"
#include "util.h"
#include "vision.h"
#include "winman.h"

#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

namespace ui {
namespace {

constexpr UINT WM_KEY_ARRIVED = WM_APP + 21;

std::string keyComboFromModifiers(const std::string& mainKey) {
    std::string combo;
    auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (held(VK_CONTROL)) combo += "ctrl+";
    if (held(VK_MENU)) combo += "alt+";
    if (held(VK_SHIFT)) combo += "shift+";
    if (held(VK_LWIN) || held(VK_RWIN)) combo += "win+";
    return combo + mainKey;
}

// ------------------------------------------------------------- key capture
// The first line of `prompt` is the heading; anything after a newline is
// the sentence under it.
constexpr char kLeftClickNote[] =
    "Left click can't be used, because it's how you click in this window. "
    "Any other button works.";
constexpr char kOtherDeviceNote[] =
    "That came from a different keypad. Press a key on the one these buttons "
    "are for.";

class KeyCaptureDialog : public Modal {
public:
    std::string prompt;
    std::string restrictMatch;
    bool comboMode = false;
    bool allowPickByName = true;
    KeyCaptureResult result;

    static constexpr int kW = 520;
    static constexpr int kCW = kW - kSheetPad * 2;

    ~KeyCaptureDialog() override {
        listener_.stop();
    }

    std::string heading() const { return prompt.substr(0, prompt.find('\n')); }
    std::string detail() const {
        const size_t n = prompt.find('\n');
        return n == std::string::npos ? std::string() : prompt.substr(n + 1);
    }
    std::string waiting() const {
        return comboMode ? "Waiting for you to press the keys"
                         : "Waiting for you to press a key or button";
    }
    // Tall enough for the longest thing the banner will say.
    int bannerH() const {
        int h = bannerHeight(waiting(), kCW);
        h = std::max(h, bannerHeight(kLeftClickNote, kCW));
        h = std::max(h, bannerHeight(kOtherDeviceNote, kCW));
        return h;
    }
    int height() const {
        int h = kSheetPad + textHeight(heading(), tk::Text::Title, kCW);
        if (!detail().empty())
            h += tk::kSpace2 + textHeight(detail(), tk::Text::Body, kCW);
        h += tk::kSpace5 + bannerH();
        if (allowPickByName)
            h += tk::kSpace6 + 18 + tk::kSpace2 + tk::kTargetMin;
        return h + tk::kSpace6 + tk::kTargetBig + kSheetPad;
    }

    void build() override {
        int y = kSheetPad;
        const int hh = textHeight(heading(), tk::Text::Title, kCW);
        makeText(hwnd_, heading(), kSheetPad, y, kCW, hh, tk::Text::Title);
        y += hh;
        if (!detail().empty()) {
            y += tk::kSpace2;
            const int dh = textHeight(detail(), tk::Text::Body, kCW);
            makeText(hwnd_, detail(), kSheetPad, y, kCW, dh, tk::Text::Body, true);
            y += dh;
        }
        y += tk::kSpace5;
        banner_ = makeBanner(hwnd_, BannerTone::Info, waiting(), kSheetPad, y, kCW);
        SetWindowPos(banner_, nullptr, 0, 0, S(kCW), S(bannerH()),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        y += bannerH();

        if (allowPickByName) {
            y += tk::kSpace6;
            makeText(hwnd_, "OR PICK IT FROM A LIST", kSheetPad, y, kCW, 18,
                     tk::Text::Overline, true);
            y += 18 + tk::kSpace2;
            std::vector<std::string> names;
            names.push_back("Choose a key");
            for (const auto& n : keys::allNames()) names.push_back(n);
            const int bw = 160;
            combo_ = makeCombo(hwnd_, kSheetPad, y, kCW - bw - tk::kSpace3,
                               tk::kTargetMin, ID_FIRST_USER, names, 0);
            setTextStyle(combo_, tk::Text::Body);
            HWND use = makeButton(hwnd_, "Use this key", kSheetPad + kCW - bw, y,
                                  bw, tk::kTargetMin, ID_FIRST_USER + 1);
            setTone(use, Tone::Secondary);
            setTextStyle(use, tk::Text::Button);
        }

        // Nothing to confirm: the window closes the moment the keys are
        // pressed, which is what lets Enter, Space and Tab be the answer too.
        HWND cancel = makeButton(hwnd_, "Cancel", kSheetPad, footerTop(), kCW,
                                 tk::kTargetBig, ID_CANCEL);
        setTextStyle(cancel, tk::Text::Button);

        try {
            // Pointer input as well as keys. Without it this dialog is deaf to
            // the very buttons a mouse deck exists for: a side button would be
            // pressed, nothing would happen, and there would be no way to bind
            // it at all. A modal that is open for a few seconds is also the one
            // place the cost of hearing pointer movement does not matter -
            // movement is discarded before it becomes an event.
            //
            // Except when recording a combo. That answer goes to
            // RegisterHotKey, or into a combo this program will type, and
            // neither can be a mouse button - so hearing the mouse there could
            // only produce a shortcut that silently fails to register.
            listener_.start(
                [this](const dev::KeyEvent& ev) {
                    if (!ev.down) return;
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        pending_.push_back(ev);
                    }
                    PostMessageW(hwnd_, WM_KEY_ARRIVED, 0, 0);
                },
                !comboMode);
        } catch (const std::exception& e) {
            setBannerTone(banner_, BannerTone::Danger);
            setText(banner_,
                    std::string("Dental Macros can't hear the keyboard right "
                                "now. ") + e.what());
        }
    }

    // Enter is a key like any other here. Only in the list does it mean
    // "use this one".
    bool onOk() override {
        if (combo_ && GetFocus() == combo_) usePicked();
        return false;
    }

    void onCommand(int id, int notify) override {
        if (id == ID_FIRST_USER + 1 && notify == BN_CLICKED) usePicked();
    }

    void usePicked() {
        if (!combo_) return;
        const int sel = (int)SendMessageW(combo_, CB_GETCURSEL, 0, 0);
        if (sel <= 0) {
            SetFocus(combo_);
            return;
        }
        wchar_t buf[128] = {0};
        SendMessageW(combo_, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)buf);
        result.ok = true;
        result.keyName = narrow(buf);
        result.combo = result.keyName;
        close(true);
    }

    bool onMessage(UINT msg, WPARAM, LPARAM, LRESULT& out) override {
        if (msg != WM_KEY_ARRIVED) return false;
        std::vector<dev::KeyEvent> batch;
        {
            std::lock_guard<std::mutex> lk(mu_);
            batch.swap(pending_);
        }
        for (const auto& ev : batch) {
            std::string name = keys::vkName(ev.vk);
            if (comboMode && keys::isModifier(ev.vk)) continue;

            // Left click is how the person in front of this dialog clicks
            // Cancel, so it can never be the answer. Now that the dialog
            // hears the mouse, that has to be said here rather than left to
            // the caller - which would take the click, close, and only then
            // explain itself.
            if (!cfg::keyBindable(name)) {
                setBannerTone(banner_, BannerTone::Warning);
                setText(banner_, kLeftClickNote);
                continue;
            }

            if (!restrictMatch.empty() &&
                !cfg::specMatches(restrictMatch, ev.devicePath)) {
                setBannerTone(banner_, BannerTone::Warning);
                setText(banner_, kOtherDeviceNote);
                continue;
            }
            result.ok = true;
            result.keyName = name;
            result.combo = comboMode ? keyComboFromModifiers(name) : name;
            result.devicePath = ev.devicePath;
            close(true);
            out = 0;
            return true;
        }
        out = 0;
        return true;
    }

    void onCancel() override { listener_.stop(); }

private:
    HWND banner_ = nullptr;
    HWND combo_ = nullptr;
    dev::RawInputListener listener_;
    std::mutex mu_;
    std::vector<dev::KeyEvent> pending_;
};

}  // namespace

KeyCaptureResult captureKey(HWND parent, const std::string& title,
                            const std::string& prompt,
                            const std::string& restrictMatch, bool comboMode,
                            bool allowPickByName) {
    KeyCaptureDialog d;
    d.prompt = prompt;
    d.restrictMatch = restrictMatch;
    d.comboMode = comboMode;
    d.allowPickByName = allowPickByName;
    d.run(parent, title, KeyCaptureDialog::kW, d.height());
    return d.result;
}

// ----------------------------------------------------------- click recorder
namespace {

constexpr UINT WM_CLICK_ARRIVED = WM_APP + 22;

// ---- numbered markers on screen
//
// One small window per recorded spot, so the user can see exactly where each
// click will land. WS_EX_LAYERED | WS_EX_TRANSPARENT makes them invisible to
// the mouse and to WindowFromPoint, so a marker is never recorded as the
// target, and a spot can be right-clicked again straight through one.
constexpr wchar_t kMarkerClass[] = L"DentalMacrosClickMarker";
constexpr int kMarkerSize = 52;   // design px; the click point is the centre

class MarkerSet {
public:
    ~MarkerSet() { clear(); }

    // Rebuilt from scratch every time: a few dozen tiny windows at most, and
    // removing a click from the middle renumbers every one after it.
    void sync(const std::vector<rec::Click>& clicks, int highlight) {
        clear();
        for (size_t i = 0; i < clicks.size(); ++i)
            add(clicks[i].x, clicks[i].y, (int)i + 1, (int)i == highlight);
    }

    void clear() {
        for (HWND h : wins_) DestroyWindow(h);
        wins_.clear();
    }

private:
    void add(int x, int y, int number, bool highlight) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kMarkerClass;
            RegisterClassExW(&wc);
            registered = true;
        }

        const int size = S(kMarkerSize);
        const POINT topLeft{x - size / 2, y - size / 2};
        HWND h = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kMarkerClass, L"", WS_POPUP, topLeft.x, topLeft.y, size, size,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!h) return;

        // Drawn with GDI+ straight into a premultiplied 32-bit DIB, which is
        // what UpdateLayeredWindow wants for per-pixel alpha.
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits,
                                       nullptr, 0);
        if (dib && bits) {
            HGDIOBJ old = SelectObject(mem, dib);
            {
                Gdiplus::Bitmap canvas(size, size, size * 4,
                                       PixelFormat32bppPARGB, (BYTE*)bits);
                Gdiplus::Graphics g(&canvas);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
                g.Clear(Gdiplus::Color(0, 0, 0, 0));

                // Denim, or honey-brown for the one about to be redone. Each
                // carries a cream edge so it shows on a dark screen and a
                // white form alike.
                const Gdiplus::Color tone =
                    highlight ? tok().warningInk : tok().primary;
                const Gdiplus::Color edgeC = tok().surfaceRaised;
                // A ring exactly on the spot.
                const float c = size / 2.0f;
                const float r = Sf(7.0f);
                Gdiplus::Pen outline(edgeC, Sf(5.0f));
                Gdiplus::Pen ring(tone, Sf(2.5f));
                g.DrawEllipse(&outline, c - r, c - r, 2 * r, 2 * r);
                g.DrawEllipse(&ring, c - r, c - r, 2 * r, 2 * r);

                // The number in a badge up and to the right, clear of the
                // spot so it never hides what is being clicked.
                const float br = Sf(12.0f);
                Gdiplus::RectF badge((Gdiplus::REAL)size - 2 * br - Sf(2.0f),
                                     Sf(2.0f), 2 * br, 2 * br);
                Gdiplus::SolidBrush fill(tone);
                Gdiplus::Pen edge(edgeC, Sf(2.0f));
                g.FillEllipse(&fill, badge);
                g.DrawEllipse(&edge, badge);
                drawText(g, std::to_wstring(number), badge,
                         number > 9 ? 11.0f : 13.0f, tok().white, Align::Center,
                         true);
            }
            POINT src{0, 0};
            POINT dst = topLeft;
            SIZE sz{size, size};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(h, screen, &dst, &sz, mem, &src, 0, &blend,
                                ULW_ALPHA);
            SelectObject(mem, old);
        }
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);

        ShowWindow(h, SW_SHOWNOACTIVATE);
        wins_.push_back(h);
    }

    std::vector<HWND> wins_;
};

// ---- the recorder window

// Only one recorder is ever open (it's modal), so a single static slot is
// enough to route the low-level hook callback - hook procs are free functions
// and carry no instance pointer of their own.
class ClickRecorderPanel;
ClickRecorderPanel* g_activeRecorder = nullptr;

// A right click is the recording gesture, so both halves of it are eaten: the
// down so nothing reacts to it, and the up because Windows raises
// WM_CONTEXTMENU from the button-up - let that through and a context menu
// opens over the very thing being recorded. The position is taken on the up.
//
// Only ever touched from the hook, which runs on the thread that installed
// it: the same thread the recorder lives on.
bool g_swallowRightUp = false;

struct RawClick {
    POINT pt{0, 0};
    HWND wnd = nullptr;
    DWORD time = 0;
};

// What a recorded click landed in. `own` is set for Dental Macros's own
// windows, which are never a target.
rec::Target describeTarget(HWND hit, bool& own) {
    rec::Target t;
    own = false;
    // The hook caught the child control under the pointer; the useful thing
    // is the top-level window that owns it.
    HWND top = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
    if (!top) return t;
    DWORD pid = 0;
    GetWindowThreadProcessId(top, &pid);
    if (pid == GetCurrentProcessId()) {
        own = true;
        return t;
    }
    win::WindowInfo info;
    if (!win::describeWindow(top, info) || info.cls.empty()) return t;
    // The desktop and the taskbar are not programs to lock in place: moving
    // them is meaningless, so a click there is just a spot on the screen.
    if (info.cls == "Progman" || info.cls == "WorkerW" ||
        info.cls == "Shell_TrayWnd" || info.cls == "Shell_SecondaryTrayWnd")
        return t;
    t.has = true;
    t.cls = info.cls;
    t.exe = info.exe;
    t.x = info.x;
    t.y = info.y;
    t.w = info.w;
    t.h = info.h;
    t.maximized = info.maximized;
    return t;
}

constexpr int kRecW = 480;
constexpr int kRecH = 664;
constexpr int kRecPad = tk::kSpace5;   // 24: a small panel beside the work
enum {
    kRecList = ID_FIRST_USER,
    kRecUndo,
    kRecRemove,
    kRecRedo,
    kRecFast,
    kRecNormal,
    kRecSlow,
};

class ClickRecorderPanel : public Modal {
public:
    std::vector<rec::Click> clicks;
    rec::Speed speed = rec::Speed::Normal;

    ~ClickRecorderPanel() override { finish(); }

    void build() override {
        const int x = kRecPad, w = kRecW - kRecPad * 2;
        makeText(hwnd_, "Recording clicks", x, 20, w, 34, tk::Text::Title);
        makeText(hwnd_,
                 "Left-click as usual to get to the right screen. Then "
                 "right-click each spot you want this button to click. "
                 "Right-click twice quickly for a double-click.",
                 x, 62, w, 104, tk::Text::Body);
        list_ = makeList(hwnd_, x, 176, w, 160, kRecList);
        setListRowHeight(list_, tk::kTargetMin);
        setTextStyle(list_, tk::Text::Body);
        status_ = makeText(hwnd_, "", x, 346, w, 62, tk::Text::Label, true);

        // Undo, take one out, redo one: three choices of the same weight,
        // each as wide as its words need.
        const int uw = 124, rw = 112;
        btnUndo_ = makeButton(hwnd_, "Undo last", x, 416, uw, tk::kTargetMin,
                              kRecUndo);
        btnRemove_ = makeButton(hwnd_, "Remove", x + uw + tk::kSpace2, 416, rw,
                                tk::kTargetMin, kRecRemove);
        setTone(btnRemove_, Tone::Danger);
        const int redoX = x + uw + rw + tk::kSpace2 * 2;
        btnRedo_ = makeButton(hwnd_, "Redo this spot", redoX, 416, x + w - redoX,
                              tk::kTargetMin, kRecRedo);
        for (HWND h : {btnUndo_, btnRemove_, btnRedo_})
            setTextStyle(h, tk::Text::Button);

        makeText(hwnd_, "PAUSE BETWEEN CLICKS", x, 482, w, 20, tk::Text::Overline,
                 true);
        const int sw = w / 3;
        btnFast_ = makeButton(hwnd_, "Fast", x, 506, sw, tk::kTargetMin, kRecFast);
        makePill(btnFast_, PillPart::First);
        btnNormal_ = makeButton(hwnd_, "Normal", x + sw, 506, sw, tk::kTargetMin,
                                kRecNormal);
        makePill(btnNormal_, PillPart::Middle);
        btnSlow_ = makeButton(hwnd_, "Slow", x + sw * 2, 506, w - sw * 2,
                              tk::kTargetMin, kRecSlow);
        makePill(btnSlow_, PillPart::Last);

        const int by = (int)(clientH_ / dpiScale()) - kRecPad - tk::kTargetBig;
        const int cw = 140;
        HWND cancel = makeButton(hwnd_, "Cancel", x, by, cw, tk::kTargetBig,
                                 ID_CANCEL);
        setTextStyle(cancel, tk::Text::Button);
        btnDone_ = makeButton(hwnd_, "Save clicks", x + cw + tk::kSpace3, by,
                              w - cw - tk::kSpace3, tk::kTargetBig, ID_OK, true);
        setTextStyle(btnDone_, tk::Text::ButtonBig);

        placeInCorner();

        g_activeRecorder = this;
        g_swallowRightUp = false;
        hook_ = SetWindowsHookExW(WH_MOUSE_LL, &ClickRecorderPanel::mouseProc,
                                  GetModuleHandleW(nullptr), 0);
        if (!hook_) {
            g_activeRecorder = nullptr;
            setStatus("Dental Macros couldn't watch the mouse. Press Cancel, "
                      "then try recording again.");
        } else {
            setStatus(waitingText());
        }
        refresh();
    }

    void onCommand(int id, int notify) override {
        if (id == kRecList) {
            if (notify == LBN_SELCHANGE) updateButtons();
            return;
        }
        if (notify != BN_CLICKED) return;
        switch (id) {
            case kRecFast: speed = rec::Speed::Fast; break;
            case kRecNormal: speed = rec::Speed::Normal; break;
            case kRecSlow: speed = rec::Speed::Slow; break;
            case kRecUndo:
                if (clicks.empty()) return;
                clicks.pop_back();
                afterEdit();
                setStatus(clicks.empty()
                              ? waitingText()
                              : "Removed the last click. " + waitingText());
                break;
            case kRecRemove: {
                const int sel = selection();
                if (sel < 0) return;
                clicks.erase(clicks.begin() + sel);
                afterEdit();
                setStatus("Removed click " + std::to_string(sel + 1) + ". " +
                          waitingText());
                break;
            }
            case kRecRedo: {
                const int sel = selection();
                if (sel < 0) return;
                if (replace_ == sel) {
                    replace_ = -1;   // pressed again: never mind
                    setStatus(waitingText());
                } else {
                    replace_ = sel;
                    setStatus("Right-click the new spot for click " +
                              std::to_string(sel + 1) + ".");
                }
                haveLast_ = false;
                break;
            }
            default: return;
        }
        refresh();
    }

    // Done.
    bool onOk() override {
        finish();
        return true;
    }

    bool canCancel() override {
        if (clicks.empty()) return true;
        // Clicks arriving while the question is up would land in a recording
        // that may be about to be thrown away, so stop listening meanwhile.
        g_activeRecorder = nullptr;
        const bool yes = confirm(
            hwnd_,
            "Throw away " + std::to_string(clicks.size()) +
                (clicks.size() == 1 ? " recorded click?" : " recorded clicks?"),
            "", "Throw away", "Keep recording", true);
        if (!yes && hook_) g_activeRecorder = this;
        return yes;
    }

    void onCancel() override {
        finish();
        clicks.clear();
    }

    bool onMessage(UINT msg, WPARAM, LPARAM, LRESULT& out) override {
        if (msg != WM_CLICK_ARRIVED) return false;
        std::vector<RawClick> batch;
        batch.swap(pending_);
        for (const RawClick& rc : batch) onClick(rc);
        out = 0;
        return true;
    }

private:
    static LRESULT CALLBACK mouseProc(int code, WPARAM wp, LPARAM lp) {
        if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);

        // Left click is deliberately untouched throughout recording, so the
        // target app can be driven normally. Only the right click is ours.
        if (wp == WM_RBUTTONDOWN && g_activeRecorder) {
            g_swallowRightUp = true;
            return 1;
        }
        if (wp == WM_RBUTTONUP && g_swallowRightUp) {
            g_swallowRightUp = false;
            if (ClickRecorderPanel* self = g_activeRecorder) {
                const auto* m = (const MSLLHOOKSTRUCT*)lp;
                // Resolve the window here, at the instant of the click. By
                // the time the posted message is handled a menu may have
                // closed or the z-order changed, and WindowFromPoint would
                // answer about a different window.
                RawClick rc;
                rc.pt = m->pt;
                rc.wnd = WindowFromPoint(m->pt);
                rc.time = m->time;
                self->pending_.push_back(rc);
                PostMessageW(self->hwnd_, WM_CLICK_ARRIVED, 0, 0);
            }
            // Eaten even when recording has stopped in between, so it cannot
            // leave a context menu behind.
            return 1;
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    void onClick(const RawClick& raw) {
        bool own = false;
        rec::Target target = describeTarget(raw.wnd, own);
        if (own) {
            setStatus("That's Dental Macros. Right-click inside the program "
                      "you want clicked.");
            return;
        }

        rec::Click c;
        c.x = raw.pt.x;
        c.y = raw.pt.y;
        c.window = target;

        std::string said;
        if (replace_ >= 0 && replace_ < (int)clicks.size()) {
            const int i = replace_;
            clicks[(size_t)i] = c;
            replace_ = -1;
            haveLast_ = false;
            said = "Click " + std::to_string(i + 1) + " redone.";
            said += windowNote(i);
        } else if (haveLast_ && !clicks.empty() && !clicks.back().isDouble &&
                   rec::makesDouble(lastPt_.x, lastPt_.y, lastTime_, raw.pt.x,
                                    raw.pt.y, raw.time, GetDoubleClickTime(),
                                    GetSystemMetrics(SM_CXDOUBLECLK),
                                    GetSystemMetrics(SM_CYDOUBLECLK))) {
            clicks.back().isDouble = true;
            haveLast_ = false;   // a third quick click is a new click
            said = "Click " + std::to_string(clicks.size()) +
                   " is now a double-click.";
        } else {
            clicks.push_back(c);
            haveLast_ = true;
            lastPt_ = raw.pt;
            lastTime_ = raw.time;
            said = "Click " + std::to_string(clicks.size()) + " recorded.";
            said += windowNote((int)clicks.size() - 1);
        }
        // With no note to read, say what to do next.
        if (said.find('\n') == std::string::npos)
            said += " Right-click the next spot, or press Save clicks.";
        setStatus(said);
        refresh();
    }

    // A word about click `i` when it is not where the others are.
    std::string windowNote(int i) const {
        const rec::Click& c = clicks[(size_t)i];
        if (!c.window.has)
            return "\nIt isn't inside a program, so it will go to that spot "
                   "on the screen wherever things are.";
        const rec::Click& first = clicks.front();
        if (i > 0 && first.window.has && !c.window.sameWindowAs(first.window))
            return "\nIt's in " + c.window.displayName() + ", not " +
                   first.window.displayName() + ". That's fine if you meant it.";
        return "";
    }

    std::string waitingText() const {
        if (clicks.empty()) return "Waiting for your first right-click...";
        return "Waiting for click " + std::to_string(clicks.size() + 1) +
               "...";
    }

    void afterEdit() {
        replace_ = -1;
        haveLast_ = false;
    }

    void refresh() {
        const int sel = selection();
        SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < clicks.size(); ++i) {
            const std::string row =
                std::string((int)i == replace_ ? "\xE2\x86\x92 " : "    ") +
                std::to_string(i + 1) + ".  " + rec::describe(clicks[i]);
            SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)widen(row).c_str());
        }
        if (replace_ >= 0)
            SendMessageW(list_, LB_SETCURSEL, (WPARAM)replace_, 0);
        else if (sel >= 0 && sel < (int)clicks.size())
            SendMessageW(list_, LB_SETCURSEL, (WPARAM)sel, 0);
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list_, nullptr, TRUE);

        markers_.sync(clicks, replace_);
        updateButtons();
    }

    void updateButtons() {
        const bool any = !clicks.empty();
        const bool sel = selection() >= 0;
        EnableWindow(btnUndo_, any);
        EnableWindow(btnRemove_, sel);
        EnableWindow(btnRedo_, sel);
        setText(btnRedo_, replace_ >= 0 ? "Stop redoing" : "Redo this spot");
        if (btnDone_) {
            EnableWindow(btnDone_, any);
            // Greyed, it says what to do instead.
            setText(btnDone_, any ? "Save clicks" : "Right-click a spot first");
        }
        setPillActive(btnFast_, speed == rec::Speed::Fast);
        setPillActive(btnNormal_, speed == rec::Speed::Normal);
        setPillActive(btnSlow_, speed == rec::Speed::Slow);
    }

    int selection() const {
        const int sel = (int)SendMessageW(list_, LB_GETCURSEL, 0, 0);
        return sel >= 0 && sel < (int)clicks.size() ? sel : -1;
    }

    void setStatus(const std::string& text) { setText(status_, text); }

    // Top-right of the screen the pointer is on: out of the way of what is
    // being clicked, which is usually in the middle.
    void placeInCorner() {
        POINT cursor{0, 0};
        GetCursorPos(&cursor);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST),
                             &mi))
            return;
        RECT wr;
        GetWindowRect(hwnd_, &wr);
        const int w = wr.right - wr.left;
        SetWindowPos(hwnd_, HWND_TOPMOST, mi.rcWork.right - w - S(16),
                     mi.rcWork.top + S(16), 0, 0, SWP_NOSIZE);
    }

    void finish() {
        uninstallHook();
        markers_.clear();
    }

    void uninstallHook() {
        if (hook_) {
            UnhookWindowsHookEx(hook_);
            hook_ = nullptr;
        }
        if (g_activeRecorder == this) g_activeRecorder = nullptr;
        // Never leave this armed: with the hook gone it would do nothing, and
        // if another recording started it would eat that one's first release.
        g_swallowRightUp = false;
    }

    HWND list_ = nullptr, status_ = nullptr;
    HWND btnFast_ = nullptr, btnNormal_ = nullptr, btnSlow_ = nullptr;
    HWND btnUndo_ = nullptr, btnRemove_ = nullptr, btnRedo_ = nullptr;
    HWND btnDone_ = nullptr;
    HHOOK hook_ = nullptr;
    std::vector<RawClick> pending_;
    MarkerSet markers_;

    int replace_ = -1;        // the row the next right-click replaces
    bool haveLast_ = false;   // the last right-click added a click...
    POINT lastPt_{0, 0};      // ...here...
    DWORD lastTime_ = 0;      // ...at this time, for double-click detection
};

}  // namespace

bool recordClicksPanel(HWND owner, std::vector<rec::Click>& clicks,
                       rec::Speed& speed) {
    ClickRecorderPanel panel;
    panel.speed = speed;
    if (!panel.run(owner, "Recording clicks", kRecW, kRecH)) return false;
    if (panel.clicks.empty()) return false;
    clicks = panel.clicks;
    speed = panel.speed;
    return true;
}

// ----------------------------------------------------------- device picker
namespace {

class AddKeyboardDialog : public Modal {
public:
    cfg::Config* config = nullptr;
    std::string newAlias;
    // Which kind of device is being added. The dialog is otherwise identical
    // for both: a mouse button is a key as far as everything here is
    // concerned, so only the wording and the list it scans change.
    bool wantMouse = false;

    static constexpr int kW = 560;
    static constexpr int kCW = kW - kSheetPad * 2;
    static constexpr int kTileH = 80;
    static constexpr int kTileGap = tk::kSpace3;
    static constexpr int kPaneH = kTileH * 3 + kTileGap * 2;
    enum {
        kLookAgain = ID_FIRST_USER + 1,
        kPane = ID_FIRST_USER + 2,
        kTileFirst = ID_FIRST_USER + 10,
    };

    const char* noun() const { return wantMouse ? "mouse" : "keypad"; }
    std::string heading() const {
        return wantMouse ? "Which mouse should run shortcuts?"
                         : "Which keypad should run shortcuts?";
    }
    std::string detail() const {
        return wantMouse ? "Press any button on it except the left one, and "
                           "it's picked for you. Or pick it below."
                         : "Press any key on it, and it's picked for you. Or "
                           "pick it below.";
    }
    std::string warning() const {
        return wantMouse ? "Its buttons stop working normally while Dental "
                           "Macros is listening. Pick a spare mouse, not the "
                           "one you point with."
                         : "Its keys stop typing while Dental Macros is "
                           "listening. Pick a spare keypad, not the keyboard "
                           "you type on.";
    }

    int height() const {
        return kSheetPad + textHeight(heading(), tk::Text::Title, kCW) +
               tk::kSpace2 + textHeight(detail(), tk::Text::Body, kCW) +
               tk::kSpace5 + bannerHeight(warning(), kCW) + tk::kSpace5 +
               kPaneH + tk::kSpace3 + tk::kTargetMin + tk::kSpace6 +
               tk::kTargetBig + kSheetPad;
    }

    ~AddKeyboardDialog() override { listener_.stop(); }

    void build() override {
        int y = kSheetPad;
        int h = textHeight(heading(), tk::Text::Title, kCW);
        makeText(hwnd_, heading(), kSheetPad, y, kCW, h, tk::Text::Title);
        y += h + tk::kSpace2;
        h = textHeight(detail(), tk::Text::Body, kCW);
        makeText(hwnd_, detail(), kSheetPad, y, kCW, h, tk::Text::Body, true);
        y += h + tk::kSpace5;
        makeBanner(hwnd_, BannerTone::Warning, warning(), kSheetPad, y, kCW);
        y += bannerHeight(warning(), kCW) + tk::kSpace5;
        pane_ = makeScrollPane(hwnd_, kSheetPad, y, kCW, kPaneH, kPane);
        y += kPaneH + tk::kSpace3;
        const int bw = 150;
        status_ = makeText(hwnd_, "", kSheetPad, y + 2, kCW - bw - tk::kSpace3,
                           44, tk::Text::Compact, true);
        HWND again = makeButton(hwnd_, "Look again", kSheetPad + kCW - bw, y, bw,
                                tk::kTargetMin, kLookAgain);
        setTextStyle(again, tk::Text::Button);
        ok_ = layoutFooter(ID_OK, "", ID_CANCEL, "Cancel");
        rescan();
        listen();
    }

    void onCommand(int id, int notify) override {
        if (notify != BN_CLICKED) return;
        if (id == kLookAgain) {
            rescan();
            return;
        }
        const int i = id - kTileFirst;
        if (i >= 0 && i < (int)tiles_.size()) select(i, (int)groups_[(size_t)i]);
    }

    bool onOk() override {
        // The button says what to do while nothing is picked.
        if (pick_ < 0 || pick_ >= (int)devices_.size()) return false;
        // It is named next; keys pressed meanwhile are not a new choice.
        listener_.stop();
        if (add(devices_[(size_t)pick_])) return true;
        listen();
        return false;
    }

    void onCancel() override { listener_.stop(); }

    bool onMessage(UINT msg, WPARAM, LPARAM, LRESULT& out) override {
        if (msg != WM_KEY_ARRIVED) return false;
        std::vector<dev::KeyEvent> batch;
        {
            std::lock_guard<std::mutex> lk(mu_);
            batch.swap(pending_);
        }
        for (const auto& ev : batch) {
            // While adding a mouse, a stray keystroke must not pick an entry
            // out of a list of mice - and vice versa.
            if (dev::isMouseVk(ev.vk) != wantMouse) continue;
            const std::string name = keys::vkName(ev.vk);
            // Left click picks things in this window; Enter, Tab, Space and
            // Esc work it. Neither says which device is wanted.
            if (wantMouse ? !cfg::keyBindable(name) : drivesThisWindow(ev.vk))
                continue;
            // The input the key came in on - or, failing that, another input
            // of the same keypad. A Bluetooth keypad's media keys arrive on an
            // input of their own that is not in this list, and on some keypads
            // that is every button there is.
            size_t hit = devices_.size();
            for (size_t i = 0; i < devices_.size(); ++i)
                if (devices_[i].path == ev.devicePath) { hit = i; break; }
            if (hit == devices_.size())
                for (size_t i = 0; i < devices_.size(); ++i)
                    if (dev::sameDevice(devices_[i].path, ev.devicePath)) {
                        hit = i;
                        break;
                    }
            if (hit < devices_.size()) {
                // The tile for that keypad; the input it came in on is the
                // one to match.
                for (size_t t = 0; t < groups_.size(); ++t)
                    if (dev::sameDevice(devices_[groups_[t]].path,
                                        devices_[hit].path)) {
                        select((int)t, (int)hit);
                        scrollPaneToShow(pane_, tiles_[t]);
                        break;
                    }
                setText(status_, "You pressed " + keys::displayName(name) +
                                     " on this one.");
            }
        }
        out = 0;
        return true;
    }

private:
    static bool drivesThisWindow(int vk) {
        switch (vk) {
            case VK_RETURN: case VK_TAB: case VK_ESCAPE: case VK_SPACE:
            case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
            case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
            case VK_MENU: case VK_LMENU: case VK_RMENU:
            case VK_LWIN: case VK_RWIN:
                return true;
        }
        return false;
    }

    std::string titleOf(const dev::InputDevice& d) const {
        if (!d.name.empty()) return d.name;
        return wantMouse ? "Mouse with no name" : "Keyboard with no name";
    }

    std::string subOf(const dev::InputDevice& d) const {
        // Any input of it counts: a deck may match its media keys.
        for (const auto& kb : config->keyboards) {
            if (kb.isMouse != wantMouse) continue;
            for (const auto& in : devices_)
                if (dev::sameDevice(in.path, d.path) &&
                    cfg::specMatches(kb.match, in.path))
                    return "Already set up as \"" + kb.alias + "\"";
        }
        return d.btAddress.empty() ? "Plugged in, or on a wireless receiver"
                                   : "Bluetooth";
    }

    void rescan() {
        for (HWND t : tiles_) DestroyWindow(t);
        tiles_.clear();
        devices_ = wantMouse ? dev::listMice() : dev::listKeyboards();
        // One tile per thing on the desk. A keypad often shows up as two or
        // three inputs (its keys, its media keys...), and three tiles with
        // the same name only make people guess.
        groups_.clear();
        for (size_t i = 0; i < devices_.size(); ++i) {
            bool seen = false;
            for (size_t g : groups_)
                if (dev::sameDevice(devices_[g].path, devices_[i].path)) {
                    seen = true;
                    break;
                }
            if (!seen) groups_.push_back(i);
        }
        // Room for the scroll bar once there are more than fit.
        const int w = kCW - (groups_.size() > 3 ? 20 : 0);
        // Two keypads of the same make have the same name; a number tells
        // them apart, and a key press says which is which.
        std::vector<std::string> titles;
        for (size_t g : groups_) titles.push_back(titleOf(devices_[g]));
        for (size_t t = 0; t < titles.size(); ++t) {
            int same = 0, nth = 0;
            for (size_t u = 0; u < titles.size(); ++u)
                if (titleOf(devices_[groups_[u]]) == titleOf(devices_[groups_[t]])) {
                    ++same;
                    if (u <= t) ++nth;
                }
            if (same > 1) titles[t] += " " + std::to_string(nth);
        }
        for (size_t t = 0; t < groups_.size(); ++t) {
            const dev::InputDevice& d = devices_[groups_[t]];
            HWND tile = makeChoice(pane_, titles[t], subOf(d), 0,
                                   (int)t * (kTileH + kTileGap), w, kTileH,
                                   kTileFirst + (int)t);
            setChoiceTitleStyle(tile, tk::Text::Button);
            tiles_.push_back(tile);
        }
        setScrollPaneContent(pane_, groups_.empty()
                                        ? 0
                                        : (int)groups_.size() *
                                                  (kTileH + kTileGap) -
                                              kTileGap);
        sel_ = pick_ = -1;
        updateOk();
        const size_t n = groups_.size();
        if (n == 0)
            setText(status_, wantMouse ? "No mice found. Check it's switched "
                                         "on, then look again."
                                       : "No keypads found. Check it's "
                                         "switched on, then look again.");
        else
            setText(status_,
                    "Found " + std::to_string(n) +
                        (wantMouse ? (n == 1 ? " mouse." : " mice and trackpads.")
                                   : (n == 1 ? " keyboard."
                                             : " keyboards and keypads.")));
    }

    // Tile `i`, and `device`, the input of it to match.
    void select(int i, int device) {
        sel_ = i;
        pick_ = device;
        for (size_t j = 0; j < tiles_.size(); ++j)
            setChoiceSelected(tiles_[j], (int)j == i);
        updateOk();
    }

    // The main button says what it does - or, while nothing is picked, what
    // to do first.
    void updateOk() {
        if (!ok_) return;
        EnableWindow(ok_, sel_ >= 0);
        setText(ok_, sel_ >= 0 ? (wantMouse ? "Add mouse" : "Add keypad")
                               : (wantMouse ? "Pick a mouse first"
                                            : "Pick a keypad first"));
    }

    void listen() {
        try {
            listener_.start(
                [this](const dev::KeyEvent& ev) {
                    if (!ev.down) return;
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        pending_.push_back(ev);
                    }
                    PostMessageW(hwnd_, WM_KEY_ARRIVED, 0, 0);
                },
                wantMouse);
        } catch (const std::exception& e) {
            setText(status_, std::string("Dental Macros can't hear key presses "
                                         "right now, so pick it from the "
                                         "list. ") +
                                 e.what());
        }
    }

    bool add(const dev::InputDevice& d) {
        for (const auto& kb : config->keyboards) {
            // A keypad's dial gets added as a mouse after its keys were added
            // as a keyboard, and both decks match the same keypad. Only the
            // same kind of deck twice is a duplicate.
            if (kb.isMouse != wantMouse) continue;
            if (cfg::specMatches(kb.match, d.path)) {
                if (!confirm(hwnd_,
                             std::string("This ") + noun() +
                                 " is already set up as \"" + kb.alias +
                                 "\". Add it again?",
                             "Both would run on the same keys.", "Add it again",
                             "Cancel"))
                    return false;
                break;
            }
        }

        bool stable = true;
        std::string match = cfg::chooseMatch(d, devices_, &stable);

        std::string suggested = config->uniqueAlias(
            !d.name.empty() ? d.name
                            : (wantMouse ? "Mouse" : "Keypad"));
        if (!askText(hwnd_, wantMouse ? "Name this mouse" : "Name this keypad",
                     wantMouse ? "What should this mouse be called?"
                               : "What should this keypad be called?",
                     suggested, "", wantMouse ? "Add mouse" : "Add keypad"))
            return false;

        cfg::Keyboard kb;
        kb.alias = config->uniqueAlias(suggested);
        kb.match = match;
        kb.isMouse = wantMouse;
        kb.block = config->settings.blockByDefault;
        // Never "take everything" by default on a pointer. A mouse with all
        // its buttons swallowed the moment it is added is a nasty surprise,
        // and unlike a keyboard there is no spare one to undo it with.
        kb.blockAllKeys =
            wantMouse ? false : config->settings.blockAllKeysByDefault;
        // A mouse has a known, fixed set of buttons, so it can start on a
        // deck that already names them rather than an empty grid.
        kb.deck = wantMouse ? "mouse" : "auto";
        config->keyboards.push_back(kb);
        config->dirty = true;
        newAlias = kb.alias;

        if (!stable)
            notice(hwnd_, "Keep it in the same USB port",
                   std::string("You have two ") +
                       (wantMouse ? "mice" : "keypads") +
                       " that look the same, so Dental Macros tells them apart "
                       "by the USB port. If you move this one to another "
                       "port, add it again.");
        return true;
    }

    HWND pane_ = nullptr, status_ = nullptr, ok_ = nullptr;
    std::vector<HWND> tiles_;
    std::vector<size_t> groups_;   // per tile, its first input in devices_
    int sel_ = -1;                 // the tile picked
    int pick_ = -1;                // the input of it, in devices_
    std::vector<dev::InputDevice> devices_;
    dev::RawInputListener listener_;
    std::mutex mu_;
    std::vector<dev::KeyEvent> pending_;
};

}  // namespace

bool addKeyboardDialog(HWND parent, cfg::Config& config,
                       std::string& newAlias, bool mouse) {
    AddKeyboardDialog d;
    d.config = &config;
    d.wantMouse = mouse;
    if (!d.run(parent, mouse ? "Add a mouse" : "Add a keypad",
               AddKeyboardDialog::kW, d.height()))
        return false;
    newAlias = d.newAlias;
    return !newAlias.empty();
}

// ------------------------------------------------------------- step editor
namespace {

struct FieldCtl {
    cfg::FieldDef def;
    Field field;            // label, box and hint, for anything typed
    HWND label = nullptr;   // the label above it
    HWND input = nullptr;   // the box, dropdown or checkbox
    HWND extra = nullptr;   // Choose a file / Press keys / Choose
    HWND extra2 = nullptr;  // Take a picture of the screen, on a picture field
    HWND hint = nullptr;
};

// Design-pixel geometry. The heading and the action picker sit above the
// scrolling field area and the buttons below it, so a step with a lot of
// parameters scrolls rather than pushing the buttons off the screen.
constexpr int kStepDlgW = 560;
constexpr int kStepPad = tk::kSpace6;   // 32, as every sheet
constexpr int kStepCW = kStepDlgW - kStepPad * 2;
// Below the fields: a gap, the 64px buttons and the bottom padding.
constexpr int kStepFooterH = tk::kSpace5 + tk::kTargetBig + kStepPad;
constexpr int kStepHelperW = 160;

class StepDialog : public Modal {
public:
    cfg::Step step;
    cfg::Settings settings;

    void build() override {
        std::vector<std::string> items;
        for (const auto& cat : cfg::actionCategories())
            for (const auto& a : cfg::actionDefs())
                if (cat == a.category) {
                    order_.push_back(a.id);
                    items.push_back(std::string(a.category) + ": " + a.label);
                }
        int sel = 0;
        for (size_t i = 0; i < order_.size(); ++i)
            if (order_[i] == step.action) sel = (int)i;
        if (step.action.empty()) step.action = order_.empty() ? "" : order_[0];

        int y = kStepPad;
        heading_ = makeText(hwnd_, "", kStepPad, y, kStepCW, 34, tk::Text::Title);
        y += 34 + tk::kSpace4;
        makeText(hwnd_, "What should this step do?", kStepPad, y, kStepCW, 20,
                 tk::Text::Label);
        y += 20 + 6;
        combo_ = makeCombo(hwnd_, kStepPad, y, kStepCW, tk::kTargetMin,
                           ID_FIRST_USER, items, sel);
        setTextStyle(combo_, tk::Text::Body);
        y += tk::kTargetMin + tk::kSpace2;
        blurbTop_ = y;
        blurb_ = makeText(hwnd_, "", kStepPad, y, kStepCW, 26, tk::Text::Body,
                          true);

        pane_ = makeScrollPane(hwnd_, 0, y + 26 + tk::kSpace4, kStepDlgW, 200,
                               ID_FIRST_USER + 2);
        rebuildFields();
    }

    void onCommand(int id, int notify) override {
        if (id == ID_FIRST_USER && notify == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(combo_, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)order_.size()) {
                collect(false);
                step.action = order_[(size_t)sel];
                rebuildFields();
            }
            return;
        }
        // per-field helper buttons live at ID_FIRST_USER + 100 + index*2,
        // and a field's second helper (Snip) at the odd id after it
        if (notify == BN_CLICKED && id >= ID_FIRST_USER + 100 &&
            id < ID_FIRST_USER + 500) {
            const int offset = id - (ID_FIRST_USER + 100);
            const int index = offset / 2;
            if (index >= 0 && index < (int)fields_.size()) {
                if (offset % 2) snipInto(fields_[(size_t)index]);
                else helperClicked(fields_[(size_t)index]);
            }
        }
    }

    bool onOk() override { return collect(true); }

private:
    // Snip a picture straight into this field. The main window is hidden as
    // well as this dialog, so that neither ends up in the picture.
    void snipInto(FieldCtl& f) {
        const bool parentShown = parent_ && IsWindowVisible(parent_);
        if (parentShown) ShowWindow(parent_, SW_HIDE);
        std::string rel;
        const bool got = snipTemplate(hwnd_, rel);
        if (parentShown) ShowWindow(parent_, SW_SHOWNA);
        SetForegroundWindow(hwnd_);
        if (got) setText(f.input, rel);
    }

    void helperClicked(FieldCtl& f) {
        switch (f.def.kind) {
            case cfg::FieldKind::File: {
                std::string rel = getText(f.input);
                if (chooseImageFile(hwnd_, rel)) setText(f.input, rel);
                break;
            }
            case cfg::FieldKind::KeyCombo: {
                KeyCaptureResult r = captureKey(
                    hwnd_, "Press a shortcut",
                    "Press the shortcut\nHold Ctrl, Alt, Shift or Win, then "
                    "press the last key.",
                    "", true, false);
                if (r.ok) setText(f.input, r.combo);
                break;
            }
            case cfg::FieldKind::Layout: {
                HMENU menu = CreatePopupMenu();
                auto names = win::listLayouts(settings.layoutsDir);
                if (names.empty())
                    AppendMenuW(menu, MF_STRING | MF_GRAYED, 1,
                                L"No layouts saved yet");
                else
                    for (size_t i = 0; i < names.size(); ++i)
                        AppendMenuW(menu, MF_STRING, i + 100,
                                    widen(names[i]).c_str());
                RECT r;
                GetWindowRect(f.extra, &r);
                int cmd = (int)TrackPopupMenu(
                    menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left,
                    r.bottom, 0, hwnd_, nullptr);
                DestroyMenu(menu);
                if (cmd >= 100 && cmd - 100 < (int)names.size())
                    setText(f.input, names[(size_t)(cmd - 100)]);
                break;
            }
            default:
                break;
        }
    }

    // Read the current controls back into step.params.
    // When `validate` is set, required fields must be filled in.
    bool collect(bool validate) {
        js::Value params = js::Value::object();
        if (validate)
            for (auto& f : fields_)
                if (f.field.edit) setFieldError(f.field, "");
        for (auto& f : fields_) {
            if (!f.input) continue;
            const cfg::FieldDef& d = f.def;
            if (d.kind == cfg::FieldKind::Bool) {
                bool on = isChecked(f.input);
                bool def = lower(trim(d.def)) == "true";
                if (on != def) params[d.key] = on;
                continue;
            }
            std::string text;
            if (d.kind == cfg::FieldKind::Choice) {
                int sel = (int)SendMessageW(f.input, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    wchar_t buf[128] = {0};
                    SendMessageW(f.input, CB_GETLBTEXT, (WPARAM)sel,
                                 (LPARAM)buf);
                    text = narrow(buf);
                }
            } else {
                text = getText(f.input);
                if (d.kind != cfg::FieldKind::Multiline) text = trim(text);
            }

            if (text.empty()) {
                if (validate && d.required) {
                    complain(f, "Fill this in to save the step");
                    return false;
                }
                continue;
            }

            if (d.kind == cfg::FieldKind::Int || d.kind == cfg::FieldKind::Float) {
                char* end = nullptr;
                double v = strtod(text.c_str(), &end);
                if (!end || *end != '\0') {
                    if (validate) {
                        complain(f, "Type a number here, like 2 or 0.5");
                        return false;
                    }
                    continue;
                }
                params[d.key] = v;
                continue;
            }
            if (text == trim(d.def) && !d.required) continue;
            params[d.key] = text;
        }
        // Not part of any action's schema, so it has to be carried across
        // explicitly - the loop above only ever writes declared fields, and
        // anything not written here is dropped on save.
        if (continueChk_ && isChecked(continueChk_))
            params[cfg::kContinueOnError] = true;
        step.params = std::move(params);
        return true;
    }

    // What is wrong goes under the box itself, which is scrolled into view.
    void complain(FieldCtl& f, const std::string& what) {
        if (f.field.edit) setFieldError(f.field, what);
        scrollPaneToShow(pane_, f.label ? f.label : f.input);
        SetFocus(f.input);
    }

    void rebuildFields() {
        for (auto& f : fields_)
            for (HWND h : {f.label, f.input, f.extra, f.extra2, f.hint})
                if (h) DestroyWindow(h);
        fields_.clear();
        if (continueChk_) { DestroyWindow(continueChk_); continueChk_ = nullptr; }

        const cfg::ActionDef* def = cfg::findAction(step.action);
        if (!def) return;
        setText(heading_, def->label);
        // The sentence about the action can run to a few lines; the fields
        // start under it.
        const int blurbH = textHeight(def->blurb, tk::Text::Body, kStepCW);
        SetWindowPos(blurb_, nullptr, S(kStepPad), S(blurbTop_), S(kStepCW),
                     S(blurbH), SWP_NOZORDER | SWP_NOACTIVATE);
        setText(blurb_, def->blurb);
        headerH_ = blurbTop_ + blurbH + tk::kSpace4;

        const int x = kStepPad;
        int y = tk::kSpace2;
        int index = 0;
        for (const cfg::FieldDef& fd : def->fields) {
            FieldCtl f;
            f.def = fd;
            const int helperId = ID_FIRST_USER + 100 + index * 2;
            const int inputId = ID_FIRST_USER + 500 + index;
            const std::string hint = fd.hint ? fd.hint : "";

            std::string current;
            const js::Value* v = step.params.find(fd.key);
            if (v) {
                if (v->isStr()) current = v->s;
                else if (v->isNum()) {
                    if (v->n == std::floor(v->n) && std::fabs(v->n) < 1e15)
                        current = std::to_string((long long)v->n);
                    else {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "%g", v->n);
                        current = buf;
                    }
                } else if (v->isBool()) {
                    current = v->b ? "true" : "false";
                }
            } else {
                current = fd.def;
            }

            // A hint under a checkbox or a dropdown, which are not TextFields.
            auto hintUnder = [&](int at) {
                if (hint.empty()) return at;
                at += 6;
                const int h = textHeight(hint, tk::Text::Compact, kStepCW);
                f.hint = makeText(pane_, hint, x, at, kStepCW, h,
                                  tk::Text::Compact, true);
                return at + h;
            };
            // A TextField, keeping its parts where rebuildFields can find them.
            auto field = [&](int w, int boxH, bool multi) {
                f.field = makeField(pane_, fd.label, current, hint, x, y, w,
                                    inputId, boxH, multi);
                f.label = f.field.label;
                f.input = f.field.edit;
                f.hint = f.field.hint;
                return y + fieldHeight(hint, w, boxH);
            };
            auto helper = [&](const char* text, int id, int bx, int by, int bw) {
                HWND b = makeButton(pane_, text, bx, by, bw, tk::kTargetMin, id);
                setTone(b, Tone::Secondary);
                setTextStyle(b, tk::Text::Button);
                return b;
            };

            switch (fd.kind) {
                case cfg::FieldKind::Bool: {
                    bool on = v ? (v->isBool() ? v->b : lower(current) == "true")
                                : lower(trim(fd.def)) == "true";
                    f.input = makeCheck(pane_, fd.label, x, y, kStepCW,
                                        tk::kTargetMin, inputId, on);
                    setTextStyle(f.input, tk::Text::Body);
                    y = hintUnder(y + tk::kTargetMin);
                    break;
                }
                case cfg::FieldKind::Choice: {
                    std::vector<std::string> choices = split(fd.choices, ',');
                    int sel = 0;
                    for (size_t i = 0; i < choices.size(); ++i)
                        if (choices[i] == current) sel = (int)i;
                    f.label = makeText(pane_, fd.label, x, y, kStepCW, 20,
                                       tk::Text::Label);
                    f.input = makeCombo(pane_, x, y + 26, 280, tk::kTargetMin,
                                        inputId, choices, sel);
                    setTextStyle(f.input, tk::Text::Body);
                    y = hintUnder(y + 26 + tk::kTargetMin);
                    break;
                }
                case cfg::FieldKind::Multiline:
                    y = field(kStepCW, 120, true);
                    break;
                case cfg::FieldKind::File:
                    // A picture to find on screen: taking one is how most
                    // people will make it, so that comes first.
                    if (std::string(fd.key) == "image") {
                        y = field(kStepCW, tk::kTargetMin, false);
                        y += tk::kSpace3;
                        const int pw = 300;
                        f.extra2 = helper("Take a picture of the screen",
                                          helperId + 1, x, y, pw);
                        f.extra = helper("Choose a file", helperId,
                                         x + pw + tk::kSpace3, y,
                                         kStepCW - pw - tk::kSpace3);
                        y += tk::kTargetMin;
                        break;
                    }
                    [[fallthrough]];
                case cfg::FieldKind::KeyCombo:
                case cfg::FieldKind::Layout: {
                    const int boxW = kStepCW - kStepHelperW - tk::kSpace3;
                    const int top = y;
                    y = field(boxW, tk::kTargetMin, false);
                    const char* btn = fd.kind == cfg::FieldKind::File ? "Choose a file"
                                      : fd.kind == cfg::FieldKind::KeyCombo
                                          ? "Press keys"
                                          : "Choose";
                    f.extra = helper(btn, helperId, x + boxW + tk::kSpace3,
                                     top + 26, kStepHelperW);
                    break;
                }
                default:
                    y = field(kStepCW, tk::kTargetMin, false);
                    break;
            }

            y += tk::kSpace5;
            fields_.push_back(f);
            ++index;
        }

        // Applies to every action, so it lives outside the schema.
        continueChk_ = makeCheck(pane_, "Keep going if this step fails", x, y,
                                 kStepCW, tk::kTargetMin, ID_FIRST_USER + 3,
                                 step.continueOnError());
        setTextStyle(continueChk_, tk::Text::Body);
        y += tk::kTargetMin + tk::kSpace4;

        layoutForContent(y);
    }

    // Size the window to the fields, but never past the edge of the screen -
    // beyond that the field area scrolls instead.
    void layoutForContent(int contentH) {
        const int roomForClient = availableClientHeight(parent_);
        const int wantClient =
            std::min(headerH_ + contentH + kStepFooterH, roomForClient);
        const int paneH = std::max(80, wantClient - headerH_ - kStepFooterH);

        RECT rc{0, 0, S(kStepDlgW), S(wantClient)};
        DWORD style = (DWORD)GetWindowLongPtrW(hwnd_, GWL_STYLE);
        AdjustWindowRectEx(&rc, style, FALSE, 0);
        // Taller or shorter, it stays centred where it was and on screen.
        RECT wr;
        GetWindowRect(hwnd_, &wr);
        const int fullH = rc.bottom - rc.top;
        const RECT work = workAreaNear(parent_ ? parent_ : hwnd_);
        int top = (wr.top + wr.bottom) / 2 - fullH / 2;
        top = std::max<int>(work.top, std::min<int>(top, work.bottom - fullH));
        SetWindowPos(hwnd_, nullptr, wr.left, top, rc.right - rc.left, fullH,
                     SWP_NOZORDER);
        RECT cr;
        GetClientRect(hwnd_, &cr);
        clientW_ = cr.right;
        clientH_ = cr.bottom;

        MoveWindow(pane_, 0, S(headerH_), S(kStepDlgW), S(paneH), TRUE);
        setScrollPaneContent(pane_, contentH);

        for (int id : {(int)ID_OK, (int)ID_CANCEL})
            if (HWND h = GetDlgItem(hwnd_, id)) DestroyWindow(h);
        layoutFooter(ID_OK, "Save step", ID_CANCEL, "Cancel");
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    HWND heading_ = nullptr, combo_ = nullptr, blurb_ = nullptr;
    HWND pane_ = nullptr, continueChk_ = nullptr;
    int blurbTop_ = 0, headerH_ = 0;
    std::vector<std::string> order_;
    std::vector<FieldCtl> fields_;
};

}  // namespace

bool editStepDialog(HWND parent, cfg::Step& step,
                    const cfg::Settings& settings) {
    StepDialog d;
    d.step = step;
    d.settings = settings;
    // The height is provisional; build() resizes to fit the chosen action.
    if (!d.run(parent, "Edit step", kStepDlgW, 520)) return false;
    step = d.step;
    return true;
}

// ------------------------------------------------------------------- snip
namespace {

class SnipOverlay {
public:
    bool run(HWND parent, std::string& outRelPath) {
        std::string err;
        if (!vis::captureScreen(cap_, &err)) {
            showMessage(parent, "Couldn't take the picture", err, true);
            return false;
        }

        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &SnipOverlay::proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"DentalMacrosSnip";
            wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
            RegisterClassExW(&wc);
            registered = true;
        }

        hwnd_ = CreateWindowExW(WS_EX_TOPMOST, L"DentalMacrosSnip", L"Snip",
                                WS_POPUP, cap_.originX, cap_.originY,
                                cap_.width, cap_.height, parent, nullptr,
                                GetModuleHandleW(nullptr), this);
        if (!hwnd_) return false;

        makeBitmap();
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        SetCapture(hwnd_);

        MSG msg;
        while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ReleaseCapture();
        if (bmp_) DeleteObject(bmp_);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;

        if (!accepted_) return false;

        RECT crop = selection();
        if (crop.right - crop.left < 4 || crop.bottom - crop.top < 4) {
            notice(parent, "Drag a bigger box",
                   "Drag a box around the button or icon you want it to "
                   "find.");
            return false;
        }

        std::string name = "template";
        if (!askText(parent, "Save picture",
                     "What should this picture be called?", name,
                     "It goes in the images folder", "Save picture"))
            return false;
        std::string rel = "images\\" + name + ".png";
        std::string saveErr;
        if (!vis::savePng(rel, cap_, crop, &saveErr)) {
            showMessage(parent, "Couldn't save the picture", saveErr, true);
            return false;
        }
        outRelPath = rel;
        return true;
    }

private:
    RECT selection() const {
        RECT r;
        r.left = std::min(startX_, curX_);
        r.top = std::min(startY_, curY_);
        r.right = std::max(startX_, curX_);
        r.bottom = std::max(startY_, curY_);
        return r;
    }

    void makeBitmap() {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cap_.width;
        bi.bmiHeader.biHeight = -cap_.height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC dc = GetDC(nullptr);
        bmp_ = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp_ && bits)
            memcpy(bits, cap_.bgra.data(),
                   (size_t)cap_.width * cap_.height * 4);
        ReleaseDC(nullptr, dc);
    }

    void paint(HDC hdc) {
        const int w = cap_.width, h = cap_.height;
        // The whole frame is put together off screen, so dragging the box
        // doesn't flicker.
        HDC buf = CreateCompatibleDC(hdc);
        HBITMAP bufBmp = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBuf = SelectObject(buf, bufBmp);

        HDC shot = CreateCompatibleDC(hdc);
        HGDIOBJ oldShot = SelectObject(shot, bmp_);
        BitBlt(buf, 0, 0, w, h, shot, 0, 0, SRCCOPY);

        // The screen dims under the design system's overlay (warm brown at
        // 40%), then the selection is put back at full brightness so the
        // user sees exactly what will be saved.
        const tk::Rgb ov = tk::colors().overlay;
        HDC dim = CreateCompatibleDC(hdc);
        HBITMAP dimBmp = CreateCompatibleBitmap(hdc, 1, 1);
        HGDIOBJ oldDim = SelectObject(dim, dimBmp);
        SetPixel(dim, 0, 0, RGB(ov.r, ov.g, ov.b));
        BLENDFUNCTION bf{AC_SRC_OVER, 0, ov.a, 0};
        AlphaBlend(buf, 0, 0, w, h, dim, 0, 0, 1, 1, bf);
        SelectObject(dim, oldDim);
        DeleteObject(dimBmp);
        DeleteDC(dim);

        const RECT sel = selection();
        const bool showSel = dragging_ || accepted_;
        if (showSel)
            BitBlt(buf, sel.left, sel.top, sel.right - sel.left,
                   sel.bottom - sel.top, shot, sel.left, sel.top, SRCCOPY);
        SelectObject(shot, oldShot);
        DeleteDC(shot);

        {
            Gdiplus::Graphics g(buf);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
            const Tok& k = tok();
            const float th = Sf(48);
            if (showSel) {
                // A 2px primary edge just outside the box, so it never
                // covers what is being saved.
                const float edge = Sf(2);
                Gdiplus::Pen pen(k.primary, edge);
                g.DrawRectangle(&pen, (float)sel.left - edge / 2,
                                (float)sel.top - edge / 2,
                                (float)(sel.right - sel.left) + edge,
                                (float)(sel.bottom - sel.top) + edge);

                // Its size, in a Toast under the box (or above it, at the
                // bottom of the screen).
                const std::wstring size =
                    std::to_wstring(sel.right - sel.left) + L" \u00d7 " +
                    std::to_wstring(sel.bottom - sel.top);
                const float tw = toastWidth(g, size);
                float tx = std::max(0.0f, std::min((float)sel.left, w - tw));
                float ty = (float)sel.bottom + Sf(12);
                if (ty + th > h) ty = (float)sel.top - th - Sf(12);
                if (ty < 0) ty = Sf(12);
                paintToast(g, Gdiplus::RectF(tx, ty, tw, th), size, false);
            }
            // What to do, in a Toast at the top.
            const std::wstring help =
                L"Drag a box around the button or icon. Press Esc to cancel.";
            const float tw = toastWidth(g, help);
            paintToast(g, Gdiplus::RectF((w - tw) / 2, Sf(24), tw, th), help,
                       false);
        }

        BitBlt(hdc, 0, 0, w, h, buf, 0, 0, SRCCOPY);
        SelectObject(buf, oldBuf);
        DeleteObject(bufBmp);
        DeleteDC(buf);
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        SnipOverlay* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = (CREATESTRUCTW*)lp;
            self = (SnipOverlay*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        } else {
            self = (SnipOverlay*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

        switch (msg) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                self->paint(hdc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_LBUTTONDOWN:
                self->dragging_ = true;
                self->startX_ = self->curX_ = GET_X_LPARAM_(lp);
                self->startY_ = self->curY_ = GET_Y_LPARAM_(lp);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_MOUSEMOVE:
                if (self->dragging_) {
                    self->curX_ = GET_X_LPARAM_(lp);
                    self->curY_ = GET_Y_LPARAM_(lp);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_LBUTTONUP:
                if (self->dragging_) {
                    self->dragging_ = false;
                    self->curX_ = GET_X_LPARAM_(lp);
                    self->curY_ = GET_Y_LPARAM_(lp);
                    self->accepted_ = true;
                    self->done_ = true;
                }
                return 0;
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) {
                    self->accepted_ = false;
                    self->done_ = true;
                }
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static int GET_X_LPARAM_(LPARAM lp) { return (int)(short)LOWORD(lp); }
    static int GET_Y_LPARAM_(LPARAM lp) { return (int)(short)HIWORD(lp); }

    HWND hwnd_ = nullptr;
    HBITMAP bmp_ = nullptr;
    vis::Capture cap_;
    bool dragging_ = false, accepted_ = false, done_ = false;
    int startX_ = 0, startY_ = 0, curX_ = 0, curY_ = 0;
};

}  // namespace

bool snipTemplate(HWND parent, std::string& outRelPath) {
    // Hide the app so it isn't part of the screenshot.
    ShowWindow(parent, SW_HIDE);
    Sleep(180);
    SnipOverlay overlay;
    bool ok = overlay.run(parent, outRelPath);
    ShowWindow(parent, SW_SHOW);
    SetForegroundWindow(parent);
    return ok;
}

// --------------------------------------------------------------- settings
namespace {

constexpr int kSettingsW = 600;
constexpr int kSetPad = tk::kSpace6;   // 32
constexpr int kSetCW = kSettingsW - kSetPad * 2;
// Under the scrolling part: a gap, the 64px buttons and the padding.
constexpr int kSettingsFooterH = tk::kSpace5 + tk::kTargetBig + kSetPad;

enum {
    kSetStartEngine = ID_FIRST_USER,
    kSetStartMin,
    kSetToTray,
    kSetRunAtLogin,
    kSetHotkey,
    kSetHotkeyRecord,
    kSetConfidence,
    kSetSaveCopy,
    kSetLoadCopy,
    kSetShowFile,
    kSetReload,
    kSetImport,
    kSetRunSetup,
    kSetPane,
    kSetBlockFirst,   // three choice tiles
};

// What a newly added keypad does with its keys. One choice rather than two
// checkboxes, because the second only ever meant something with the first
// ticked.
enum BlockingChoice { kBlockAll = 0, kBlockMacroKeys = 1, kBlockNone = 2 };

class SettingsDialog : public Modal {
public:
    cfg::Settings settings;
    SettingsAction action = SettingsAction::None;

    void build() override {
        pane_ = makeScrollPane(hwnd_, 0, 0, kSettingsW,
                               designH() - kSettingsFooterH, kSetPane);
        const int x = kSetPad;
        int y = kSetPad;
        makeText(pane_, "Settings", x, y, kSetCW, 34, tk::Text::Title);
        y += 34 + tk::kSpace5;

        section("STARTUP", y);
        runAtLogin_ = check("Start Dental Macros when I sign in to Windows",
                            kSetRunAtLogin, runAtLoginEnabled(), y);
        startEngine_ = check("Start listening as soon as it opens",
                             kSetStartEngine, settings.startEngineOnLaunch, y);
        toTray_ = check("Keep it running by the clock when I close the window",
                        kSetToTray, settings.minimizeToTray, y);
        startMin_ = check("Start hidden, with just the icon by the clock",
                          kSetStartMin, settings.startMinimized, y);
        y += tk::kSpace5;

        section("KEYPADS", y);
        {
            // The box leaves room for its button; the hint runs full width.
            const int boxW = kSetCW - 160 - tk::kSpace3;
            const std::string hint =
                "Press it anywhere to stop or start listening, so a keypad "
                "that has stopped typing can always be given back. Leave it "
                "empty for none.";
            hotkey_ = makeField(pane_, "Off switch", settings.toggleHotkey, hint,
                                x, y, boxW, kSetHotkey);
            HWND rec = makeButton(pane_, "Press keys", x + boxW + tk::kSpace3,
                                  y + 26, 160, tk::kTargetMin, kSetHotkeyRecord);
            setTone(rec, Tone::Secondary);
            setTextStyle(rec, tk::Text::Button);
            const int hh = textHeight(hint, tk::Text::Compact, kSetCW);
            SetWindowPos(hotkey_.hint, nullptr, 0, 0, S(kSetCW), S(hh),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            y += 26 + tk::kTargetMin + 6 + hh + tk::kSpace5;
        }

        makeText(pane_, "When a new keypad is added", x, y, kSetCW, 20,
                 tk::Text::Label);
        y += 20 + tk::kSpace2;
        static const char* kTitles[] = {"Take every key from the keypad",
                                        "Take only keys that have a button",
                                        "Let every key type normally"};
        static const char* kSubs[] = {
            "Best for a spare keypad: its keys only run buttons",
            "For F13 to F24, or extra keys on a keyboard you also type on",
            "Buttons still run, and the keys type as well"};
        for (int i = 0; i < 3; ++i) {
            HWND t = makeChoice(pane_, kTitles[i], kSubs[i], x, y, kSetCW, 76,
                                kSetBlockFirst + i);
            setChoiceTitleStyle(t, tk::Text::Button);
            blocking_[i] = t;
            y += 76 + tk::kSpace2;
        }
        selectBlocking(!settings.blockByDefault ? kBlockNone
                       : settings.blockAllKeysByDefault ? kBlockAll
                                                        : kBlockMacroKeys);
        y = textUnder("To change just one keypad, right-click it in the list on "
                      "the left.",
                      y) +
            tk::kSpace5;

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", settings.matchConfidence);
        const std::string confHint =
            "Between 0.5 and 1.0. Lower forgives more but may match the wrong "
            "thing. 0.85 suits most buttons.";
        confidence_ = makeField(pane_, "How closely a picture on screen must match",
                                buf, confHint, x, y, kSetCW, kSetConfidence);
        SetWindowPos(confidence_.edit, nullptr, 0, 0, S(140), S(tk::kTargetMin),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        y += fieldHeight(confHint, kSetCW) + tk::kSpace6;

        section("BACKUP AND RESTORE", y);
        y = textUnder("Every change saves as you make it, and a copy of each "
                      "day's settings is kept too (the last 10).",
                      y, tk::Text::Body) +
            tk::kSpace4;
        const int bw = (kSetCW - tk::kSpace3) / 2;
        struct B { const char* text; int id; Tone tone; };
        const B buttons[] = {
            {"Save a copy", kSetSaveCopy, Tone::Secondary},
            {"Load a copy", kSetLoadCopy, Tone::Secondary},
            {"Show my settings file", kSetShowFile, Tone::Secondary},
            {"Reload from file", kSetReload, Tone::Secondary},
            {"Import from macrokb", kSetImport, Tone::Secondary},
            {"Run setup again", kSetRunSetup, Tone::Quiet},
        };
        for (int i = 0; i < 6; ++i) {
            HWND b = makeButton(pane_, buttons[i].text,
                                x + (i % 2) * (bw + tk::kSpace3),
                                y + (i / 2) * (tk::kTargetMin + tk::kSpace3), bw,
                                tk::kTargetMin, buttons[i].id);
            setTone(b, buttons[i].tone);
            setTextStyle(b, tk::Text::Button);
        }
        y += 3 * tk::kTargetMin + 2 * tk::kSpace3 + tk::kSpace6;

        section("ABOUT", y);
        {
            // The settings file's path is its own line, shortened in the
            // middle when it is too long to fit.
            const std::string about = aboutText();
            const size_t cut = about.rfind('\n');
            y = textUnder(about.substr(0, cut), y, tk::Text::Body);
            if (cut != std::string::npos) {
                HWND path = makeText(pane_, about.substr(cut + 1), kSetPad, y,
                                     kSetCW, 26, tk::Text::Body, true);
                SetWindowLongPtrW(path, GWL_STYLE,
                                  GetWindowLongPtrW(path, GWL_STYLE) |
                                      SS_PATHELLIPSIS);
                y += 26;
            }
            y += kSetPad;
        }

        setScrollPaneContent(pane_, y);
        layoutFooter(ID_OK, "Save changes", ID_CANCEL, "Cancel");
    }

    void onCommand(int id, int notify) override {
        if (notify != BN_CLICKED) return;
        if (id >= kSetBlockFirst && id < kSetBlockFirst + 3) {
            selectBlocking(id - kSetBlockFirst);
            return;
        }
        switch (id) {
            case kSetHotkeyRecord: {
                KeyCaptureResult r = captureKey(
                    hwnd_, "Set the off switch",
                    "Press the off switch you want\nHold Ctrl, Alt, Shift or "
                    "Win, then press the last key.",
                    "", true, false);
                if (r.ok) {
                    setText(hotkey_.edit, r.combo);
                    setFieldError(hotkey_, "");
                }
                break;
            }
            case kSetShowFile: showSettingsFile(); break;
            case kSetSaveCopy: finishWith(SettingsAction::SaveCopy); break;
            case kSetLoadCopy: finishWith(SettingsAction::LoadCopy); break;
            case kSetReload: finishWith(SettingsAction::Reload); break;
            case kSetImport: finishWith(SettingsAction::ImportMacroKb); break;
            case kSetRunSetup: finishWith(SettingsAction::RunSetup); break;
        }
    }

    bool onOk() override {
        setFieldError(hotkey_, "");
        setFieldError(confidence_, "");
        settings.startEngineOnLaunch = isChecked(startEngine_);
        settings.startMinimized = isChecked(startMin_);
        settings.minimizeToTray = isChecked(toTray_);

        settings.blockByDefault = blockChoice_ != kBlockNone;
        // "Type normally" leaves the second setting as it was: it only
        // matters once blocking is back on, and then the old answer is still
        // right.
        if (blockChoice_ == kBlockAll) settings.blockAllKeysByDefault = true;
        if (blockChoice_ == kBlockMacroKeys) settings.blockAllKeysByDefault = false;

        const std::string combo = trim(getText(hotkey_.edit));
        if (!combo.empty()) {
            unsigned mods = 0;
            int vk = 0;
            std::string err;
            if (!keys::parseHotkey(combo, mods, vk, &err)) {
                // Kept to the two lines the hint had.
                complain(hotkey_, "That won't work as a shortcut: " + err + ".");
                return false;
            }
        }
        settings.toggleHotkey = combo;

        double c = atof(getText(confidence_.edit).c_str());
        if (c < cfg::kMinConfidence || c > cfg::kMaxConfidence) {
            complain(confidence_, "Type a number between 0.5 and 1.0, like 0.85");
            return false;
        }
        settings.matchConfidence = c;

        // Not stored in macros.json - the registry is the single source of
        // truth, so a copy of the app that was moved or deleted can't leave a
        // stale "yes" behind in a settings file.
        const bool wantLogin = isChecked(runAtLogin_);
        if (wantLogin != runAtLoginEnabled()) {
            std::string err;
            if (!setRunAtLogin(wantLogin, &err))
                notice(hwnd_, "Couldn't change startup",
                       "Windows didn't let Dental Macros change whether it "
                       "starts when you sign in. Everything else was "
                       "saved.\n\nDetails: " + err,
                       NoticeKind::Problem);
        }
        return true;
    }

private:
    void section(const char* text, int& y) {
        makeText(pane_, text, kSetPad, y, kSetCW, 18, tk::Text::Overline, true);
        y += 18 + tk::kSpace2;
    }

    HWND check(const char* text, int id, bool on, int& y) {
        HWND c = makeCheck(pane_, text, kSetPad, y, kSetCW, tk::kTargetMin, id, on);
        setTextStyle(c, tk::Text::Body);
        y += tk::kTargetMin + tk::kSpace1;
        return c;
    }

    // Muted text the full width; returns where it ends.
    int textUnder(const std::string& text, int y,
                  tk::Text style = tk::Text::Compact) {
        const int h = textHeight(text, style, kSetCW);
        makeText(pane_, text, kSetPad, y, kSetCW, h, style, true);
        return y + h;
    }

    void selectBlocking(int i) {
        blockChoice_ = i;
        for (int j = 0; j < 3; ++j) setChoiceSelected(blocking_[j], j == i);
    }

    void complain(Field& f, const std::string& what) {
        setFieldError(f, what);
        scrollPaneToShow(pane_, f.label);
        SetFocus(f.edit);
    }

    // The buttons that replace or reload everything are carried out by the
    // main window. What is on screen here is saved first, so nothing typed
    // into this window is lost on the way.
    void finishWith(SettingsAction a) {
        if (!onOk()) return;
        action = a;
        close(true);
    }

    // Explorer, with macros.json already selected.
    void showSettingsFile() {
        const std::string file = dataPath("macros.json");
        if (fileExists(file)) {
            const std::wstring args = L"/select,\"" + widen(file) + L"\"";
            ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr,
                          SW_SHOWNORMAL);
        } else {
            ShellExecuteW(hwnd_, L"open", widen(dataDir()).c_str(), nullptr,
                          nullptr, SW_SHOWNORMAL);
        }
    }

    HWND pane_ = nullptr;
    HWND startEngine_ = nullptr, startMin_ = nullptr, toTray_ = nullptr,
         runAtLogin_ = nullptr;
    HWND blocking_[3] = {};
    int blockChoice_ = kBlockAll;
    Field hotkey_, confidence_;
};

}  // namespace

std::string aboutText() {
    return "Dental Macros - buttons that type and click for you.\n\n"
           "Because Windows reports which keyboard each key press came from, "
           "a spare keypad can be given over entirely to your buttons while "
           "you keep typing normally on your main keyboard.\n\n"
           "Everything runs on this computer. Nothing is sent anywhere.\n\n" +
           std::string(dataDirIsFallback()
                           ? "Your settings are kept in your Windows profile, "
                             "because Dental Macros' own folder can't be "
                             "changed:\n"
                           : "Your settings are kept next to the program:\n") +
           dataPath("macros.json");
}

bool settingsDialog(HWND parent, cfg::Settings& settings,
                    SettingsAction* action) {
    SettingsDialog d;
    d.settings = settings;
    if (action) *action = SettingsAction::None;
    // Tall enough for most of it; the rest scrolls.
    const int h = std::min(760, availableClientHeight(parent));
    if (!d.run(parent, "Settings", kSettingsW, h))
        return false;
    settings = d.settings;
    if (action) *action = d.action;
    return true;
}

// ------------------------------------------------------ simple-mode prompts
namespace {

class LongTextPrompt : public Modal {
public:
    std::string prompt, hint, value, okText;
    static constexpr int kW = 560;
    static constexpr int kCW = kW - kSheetPad * 2;
    static constexpr int kBoxH = 160;

    int height() const {
        return kSheetPad + textHeight(prompt, tk::Text::Title, kCW) +
               tk::kSpace5 + fieldHeight(hint, kCW, kBoxH) + tk::kSpace6 +
               tk::kTargetBig + kSheetPad;
    }

    void build() override {
        int y = kSheetPad;
        const int hh = textHeight(prompt, tk::Text::Title, kCW);
        makeText(hwnd_, prompt, kSheetPad, y, kCW, hh, tk::Text::Title);
        y += hh + tk::kSpace5;
        field_ = makeField(hwnd_, "Words to type", value, hint, kSheetPad, y,
                           kCW, ID_FIRST_USER, kBoxH, true);
        layoutFooter(ID_OK, okText, ID_CANCEL, "Cancel");
        SetFocus(field_.edit);
        SendMessageW(field_.edit, EM_SETSEL, 0, -1);
    }

    bool onOk() override {
        // A multi-line edit gives "\r\n"; the step stores plain "\n".
        value = replaceAll(getText(field_.edit), "\r\n", "\n");
        if (trim(value).empty()) {
            setFieldError(field_, "Type the words this button should type for "
                                  "you");
            SetFocus(field_.edit);
            return false;
        }
        return true;
    }

private:
    Field field_;
};

constexpr int kWaitChoiceFirst = ID_FIRST_USER;
const double kWaitChoices[] = {0.5, 1.0, 2.0, 5.0};
const char* kWaitLabels[] = {"Half a second", "1 second", "2 seconds",
                             "5 seconds"};
const char* kWaitSubs[] = {"A quick pause", "Most screens", "A slow screen",
                           "A program opening"};

class WaitPrompt : public Modal {
public:
    double seconds = 1.0;
    static constexpr int kW = 520;
    static constexpr int kCW = kW - kSheetPad * 2;
    static constexpr int kTileH = 88;
    static constexpr char kDetail[] =
        "Give a program time to open a screen or a menu before the next step.";

    int height() const {
        return kSheetPad + 34 + tk::kSpace2 +
               textHeight(kDetail, tk::Text::Body, kCW) + tk::kSpace5 +
               kTileH * 2 + tk::kSpace3 + tk::kSpace6 + tk::kTargetBig +
               kSheetPad;
    }

    void build() override {
        int y = kSheetPad;
        makeText(hwnd_, "How long should it wait?", kSheetPad, y, kCW, 34,
                 tk::Text::Title);
        y += 34 + tk::kSpace2;
        const int dh = textHeight(kDetail, tk::Text::Body, kCW);
        makeText(hwnd_, kDetail, kSheetPad, y, kCW, dh, tk::Text::Body, true);
        y += dh + tk::kSpace5;
        const int tw = (kCW - tk::kSpace3) / 2;
        int chosen = 1;
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(kWaitChoices[i] - seconds) < 1e-9) chosen = i;
            tiles_[i] = makeChoice(hwnd_, kWaitLabels[i], kWaitSubs[i],
                                   kSheetPad + (i % 2) * (tw + tk::kSpace3),
                                   y + (i / 2) * (kTileH + tk::kSpace3), tw,
                                   kTileH, kWaitChoiceFirst + i);
            setChoiceTitleStyle(tiles_[i], tk::Text::Button);
        }
        layoutFooter(ID_OK, "Add wait", ID_CANCEL, "Cancel");
        select(chosen);
        SetFocus(tiles_[chosen]);
    }

    void onCommand(int id, int notify) override {
        if (notify != BN_CLICKED) return;
        const int i = id - kWaitChoiceFirst;
        if (i >= 0 && i < 4) select(i);
    }

private:
    void select(int i) {
        seconds = kWaitChoices[i];
        for (int j = 0; j < 4; ++j) setChoiceSelected(tiles_[j], j == i);
    }

    HWND tiles_[4] = {};
};

// One of several, as a column of choice tiles that scrolls.
class ListSheet : public Modal {
public:
    std::string heading, detail, okText;
    std::vector<ListChoice> items;
    int selected = -1;

    static constexpr int kW = 560;
    static constexpr int kCW = kW - kSheetPad * 2;
    static constexpr int kTileH = 76;
    static constexpr int kGap = tk::kSpace2;
    static constexpr int kShown = 5;   // tiles in view before it scrolls

    int paneH() const {
        const int n = std::max(1, std::min((int)items.size(), kShown));
        return n * (kTileH + kGap) - kGap;
    }
    int height() const {
        int h = kSheetPad + textHeight(heading, tk::Text::Title, kCW);
        if (!detail.empty())
            h += tk::kSpace2 + textHeight(detail, tk::Text::Body, kCW);
        return h + tk::kSpace5 + paneH() + tk::kSpace6 + tk::kTargetBig +
               kSheetPad;
    }

    void build() override {
        int y = kSheetPad;
        int h = textHeight(heading, tk::Text::Title, kCW);
        makeText(hwnd_, heading, kSheetPad, y, kCW, h, tk::Text::Title);
        y += h;
        if (!detail.empty()) {
            y += tk::kSpace2;
            h = textHeight(detail, tk::Text::Body, kCW);
            makeText(hwnd_, detail, kSheetPad, y, kCW, h, tk::Text::Body, true);
            y += h;
        }
        y += tk::kSpace5;
        HWND pane = makeScrollPane(hwnd_, kSheetPad, y, kCW, paneH(),
                                   ID_FIRST_USER + 1);
        const int w = kCW - ((int)items.size() > kShown ? 20 : 0);
        for (size_t i = 0; i < items.size(); ++i) {
            HWND t = makeChoice(pane, items[i].title, items[i].sub, 0,
                                (int)i * (kTileH + kGap), w, kTileH,
                                ID_FIRST_USER + 10 + (int)i);
            setChoiceTitleStyle(t, tk::Text::Button);
            tiles_.push_back(t);
        }
        setScrollPaneContent(pane, (int)items.size() * (kTileH + kGap) - kGap);
        ok_ = layoutFooter(ID_OK, okText, ID_CANCEL, "Cancel");
        select(selected);
        if (selected >= 0 && selected < (int)tiles_.size()) {
            scrollPaneToShow(pane, tiles_[(size_t)selected]);
            SetFocus(tiles_[(size_t)selected]);
        }
    }

    void onCommand(int id, int notify) override {
        if (notify != BN_CLICKED) return;
        const int i = id - (ID_FIRST_USER + 10);
        if (i >= 0 && i < (int)tiles_.size()) select(i);
    }

    bool onOk() override { return selected >= 0; }

private:
    void select(int i) {
        selected = (i >= 0 && i < (int)tiles_.size()) ? i : -1;
        for (size_t j = 0; j < tiles_.size(); ++j)
            setChoiceSelected(tiles_[j], (int)j == selected);
        EnableWindow(ok_, selected >= 0);
        setText(ok_, selected >= 0 ? okText : "Pick one first");
    }

    std::vector<HWND> tiles_;
    HWND ok_ = nullptr;
};

}  // namespace

bool askLongText(HWND parent, const std::string& title,
                 const std::string& prompt, std::string& value,
                 const std::string& hint, const std::string& okText) {
    LongTextPrompt d;
    d.prompt = prompt;
    d.hint = hint;
    d.value = value;
    d.okText = okText.empty() ? "Add text" : okText;
    if (!d.run(parent, title, LongTextPrompt::kW, d.height())) return false;
    value = d.value;
    return true;
}

bool chooseWait(HWND parent, double& seconds) {
    WaitPrompt d;
    d.seconds = seconds;
    if (!d.run(parent, "Add a wait", WaitPrompt::kW, d.height())) return false;
    seconds = d.seconds;
    return true;
}

bool chooseFromList(HWND parent, const std::string& windowTitle,
                    const std::string& heading, const std::string& detail,
                    const std::vector<ListChoice>& items, int& selected,
                    const std::string& okText) {
    if (items.empty()) return false;
    ListSheet d;
    d.heading = heading;
    d.detail = detail;
    d.items = items;
    d.selected = selected;
    d.okText = okText.empty() ? "Save" : okText;
    if (!d.run(parent, windowTitle, ListSheet::kW, d.height())) return false;
    selected = d.selected;
    return selected >= 0;
}

// ------------------------------------------------------- explained problems
namespace {

constexpr int kFriendlyW = 520;
constexpr int kMoreDetailsId = ID_FIRST_USER;
constexpr int kDetailsBoxH = 120;

void copyText(HWND owner, const std::string& text) {
    const std::wstring w = widen(text);
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, w.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

// A danger Banner saying what went wrong, a paragraph on what to do, and the
// button that fixes it. The technical text stays out of sight until "More
// details", which shows it and puts it on the clipboard for whoever helps.
class FriendlyDialog : public Modal {
public:
    fe::Friendly f;
    static constexpr int kCW = kFriendlyW - kSheetPad * 2;

    int height() const {
        int h = kSheetPad + bannerHeight(f.title, kCW);
        if (!f.body.empty())
            h += tk::kSpace4 + textHeight(f.body, tk::Text::Body, kCW);
        if (!f.technical.empty()) h += tk::kSpace4 + tk::kTargetMin;
        return h + tk::kSpace6 + tk::kTargetBig + kSheetPad;
    }

    void build() override {
        int y = kSheetPad;
        makeBanner(hwnd_, BannerTone::Danger, f.title, kSheetPad, y, kCW);
        y += bannerHeight(f.title, kCW);
        if (!f.body.empty()) {
            y += tk::kSpace4;
            const int h = textHeight(f.body, tk::Text::Body, kCW);
            makeText(hwnd_, f.body, kSheetPad, y, kCW, h, tk::Text::Body);
            y += h;
        }
        if (!f.technical.empty()) {
            y += tk::kSpace4;
            more_ = makeButton(hwnd_, "More details", kSheetPad, y, 180,
                               tk::kTargetMin, kMoreDetailsId);
            setTextStyle(more_, tk::Text::Button);
            detailsY_ = y + tk::kTargetMin + tk::kSpace3;
        }
        if (f.action.empty())
            layoutFooter(ID_OK, "Close", ID_CANCEL, "");
        else
            layoutFooter(ID_OK, f.action, ID_CANCEL, "Close");
    }

    void onCommand(int id, int notify) override {
        if (id != kMoreDetailsId || notify != BN_CLICKED) return;
        copyText(hwnd_, f.title + "\r\n\r\n" + f.body + "\r\n\r\nDetails: " +
                            f.technical);
        setText(more_, "Details copied");
        if (shown_) return;
        shown_ = true;
        // Make room for the text above the buttons.
        const int grow = S(kDetailsBoxH + tk::kSpace3);
        RECT wr;
        GetWindowRect(hwnd_, &wr);
        SetWindowPos(hwnd_, nullptr, 0, 0, wr.right - wr.left,
                     wr.bottom - wr.top + grow,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT cr;
        GetClientRect(hwnd_, &cr);
        clientH_ = cr.bottom;
        for (int bid : {(int)ID_OK, (int)ID_CANCEL})
            if (HWND b = GetDlgItem(hwnd_, bid)) {
                RECT br;
                GetWindowRect(b, &br);
                MapWindowPoints(nullptr, hwnd_, (POINT*)&br, 2);
                SetWindowPos(b, nullptr, br.left, br.top + grow, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        std::string tech = replaceAll(f.technical, "\r\n", "\n");
        tech = replaceAll(tech, "\n", "\r\n");
        HWND box = makeEdit(hwnd_, tech, kSheetPad, detailsY_, kCW,
                            kDetailsBoxH, -1, true, true);
        setTextStyle(box, tk::Text::Compact);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

private:
    HWND more_ = nullptr;
    int detailsY_ = 0;
    bool shown_ = false;
};

}  // namespace

bool showFriendly(HWND parent, const fe::Friendly& f) {
    FriendlyDialog d;
    d.f = f;
    const bool pressed =
        d.run(parent, "Dental Macros", kFriendlyW, d.height());
    return pressed && !f.action.empty();
}

// ---------------------------------------------------------- first-run setup
// Three pages in one window: two questions, a page of checks (and, for a
// keypad, "press any key on it"), and the first button. Everything it learns
// comes back in a SetupResult; the main window turns that into a deck.
namespace {

constexpr int kWizW = 720;
constexpr int kWizH = 720;
constexpr int kWizPad = tk::kSpace6;            // 32
constexpr int kWizContentW = kWizW - kWizPad * 2;
constexpr UINT WM_WIZ_KEY = WM_APP + 31;

enum {
    kWizScreen = ID_FIRST_USER,
    kWizKeypad,
    kWizType,
    kWizClicks,
    kWizBack,
    kWizUseScreen,
    kWizRecord,
    kWizText,
    kWizName,
    kWizLogin,
    kWizFloat,
};

enum class Check { Ok, Note, Problem, Waiting };

// The first two words, without trailing punctuation: "Thank you".
std::string nameFromText(const std::string& text) {
    std::string out;
    int words = 0;
    bool inWord = false;
    for (char c : text) {
        const bool space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
        if (space) {
            if (inWord && ++words == 2) break;
            inWord = false;
            if (!out.empty() && out.back() != ' ') out += ' ';
            continue;
        }
        inWord = true;
        out += c;
    }
    out = trim(out);
    while (!out.empty() && std::strchr(".,!?;:", out.back())) out.pop_back();
    if (out.size() > 24) out = trim(out.substr(0, 24));
    return out.empty() ? "My button" : out;
}

// Three steps, one question each, as the design system's setup flows are:
//   1. How do you want to press your buttons?
//   2. The checks - for a keypad, "Which keypad should run shortcuts?"
//   3. What should your first button do?
// Each has a step badge, the question as its heading, one line of help, and
// the big main action at the bottom.
class SetupWizard : public Modal {
public:
    SetupContext ctx;
    SetupResult result;

    ~SetupWizard() override { listener_.stop(); }

    void build() override {
        const int x = kWizPad, w = kWizContentW;
        const int half = (w - tk::kSpace4) / 2;
        const int x2 = x + half + tk::kSpace4;

        // ---- step 1: where the buttons are
        heading_ = makeText(hwnd_, "", x, 66, w, 34, tk::Text::Title);
        help_ = makeText(hwnd_, "", x, 106, w, 26, tk::Text::Body, true);
        btnScreen_ = add(0, makeChoice(hwnd_, "On my screen",
                                       "A row of buttons floats over your "
                                       "other programs. Nothing to plug in.",
                                       x, 156, half, 176, kWizScreen));
        btnKeypad_ = add(0, makeChoice(hwnd_, "On a spare keypad",
                                       "Its keys run your buttons. Your main "
                                       "keyboard types as usual.",
                                       x2, 156, half, 176, kWizKeypad));

        // ---- step 2: the checks (rows are made when the step is shown)

        // ---- step 3: the first button
        btnType_ = add(2, makeChoice(hwnd_, "Type something",
                                     "A phrase or note you type all day.", x,
                                     120, half, 112, kWizType));
        btnClicks_ = add(2, makeChoice(hwnd_, "Click through screens",
                                       "The same spots in a program, in order.",
                                       x2, 120, half, 112, kWizClicks));
        typeLabel_ = add(2, makeText(hwnd_, "What should it type?", x, 250, w, 22,
                                     tk::Text::Label));
        text_ = add(2, makeEdit(hwnd_,
                                "Thank you for visiting! See you at your next "
                                "cleaning.",
                                x, 276, w, 80, kWizText, true));
        setTextStyle(text_, tk::Text::Body);
        clicksHint_ = add(2, makeText(
            hwnd_,
            "A small window will guide you. Left-click as usual to get to the "
            "right screen, then right-click each spot you want clicked.",
            x, 250, w, 52, tk::Text::Body, true));
        btnRecord_ = add(2, makeButton(hwnd_, "Record my clicks", x, 308, 240,
                                       tk::kTargetMin, kWizRecord));
        setTone(btnRecord_, Tone::Secondary);
        setTextStyle(btnRecord_, tk::Text::Button);
        clicksStatus_ = add(2, makeText(hwnd_, "No clicks yet.", x + 256, 320,
                                        w - 256, 26, tk::Text::Body, true));
        add(2, makeText(hwnd_, "Name on the button", x, 372, w, 22,
                        tk::Text::Label));
        name_ = add(2, makeEdit(hwnd_, "", x, 398, 360, tk::kTargetMin, kWizName));
        setTextStyle(name_, tk::Text::Body);
        login_ = add(2, makeCheck(hwnd_, "Start Dental Macros with Windows", x,
                                  456, w, tk::kTargetMin, kWizLogin, true));
        setTextStyle(login_, tk::Text::Body);
        float_ = add(2, makeCheck(hwnd_, "Keep my buttons on top of other windows",
                                  x, 504, w, tk::kTargetMin, kWizFloat, true));
        setTextStyle(float_, tk::Text::Body);
        tip_ = add(2, makeText(hwnd_, "", x, 560, w, 52, tk::Text::Label, true));

        // ---- the footer, on every step: Skip and Back are quiet, the step's
        // own action is the one big Primary, last.
        const int fy = (int)(clientH_ / dpiScale()) - kWizPad - tk::kTargetBig;
        HWND skip = makeButton(hwnd_, "Skip setup", x, fy, 150, tk::kTargetBig,
                               ID_CANCEL);
        setTextStyle(skip, tk::Text::Button);
        btnBack_ = makeButton(hwnd_, "Back", kWizW - kWizPad - 260 - 12 - 120, fy,
                              120, tk::kTargetBig, kWizBack);
        setTextStyle(btnBack_, tk::Text::Button);
        btnNext_ = makeButton(hwnd_, "Next", kWizW - kWizPad - 260, fy, 260,
                              tk::kTargetBig, ID_OK, true);
        setTextStyle(btnNext_, tk::Text::ButtonBig);

        setName(nameFromText(getText(text_)));
        showPage(0);
    }

    void onCommand(int id, int notify) override {
        if (id == kWizText && notify == EN_CHANGE && !nameEdited_)
            setName(nameFromText(getText(text_)));
        if (id == kWizName && notify == EN_CHANGE && !settingName_)
            nameEdited_ = true;
        if (notify != BN_CLICKED) return;
        switch (id) {
            case kWizScreen: onScreen_ = true; refreshChoices(); break;
            case kWizKeypad: onScreen_ = false; refreshChoices(); break;
            case kWizType: clicksRoute_ = false; refreshChoices(); showButtonPage(); break;
            case kWizClicks: clicksRoute_ = true; refreshChoices(); showButtonPage(); break;
            case kWizBack: if (page_ > 0) showPage(page_ - 1); break;
            case kWizUseScreen:
                onScreen_ = true;
                refreshChoices();
                showPage(1);   // the checks again, without the keypad
                break;
            case kWizRecord: recordClicks(); break;
        }
    }

    bool onOk() override {
        if (page_ == 0) { showPage(1); return false; }
        if (page_ == 1) {
            if (!readyToGoOn()) return false;
            showPage(2);
            return false;
        }
        return finish();
    }

    bool canCancel() override {
        return confirm(hwnd_, "Skip setup?",
                       "You can make buttons yourself instead, or run setup "
                       "again any time from Settings.",
                       "Skip setup", "Back to setup");
    }

    void onCancel() override { listener_.stop(); }

    bool onMessage(UINT msg, WPARAM, LPARAM, LRESULT& out) override {
        if (msg == WM_PAINT) {
            paintMarks();
            out = 0;
            return true;
        }
        if (msg != WM_WIZ_KEY) return false;
        std::vector<dev::KeyEvent> batch;
        {
            std::lock_guard<std::mutex> lk(keyMu_);
            batch.swap(keys_);
        }
        for (const auto& ev : batch) onKeypadKey(ev);
        out = 0;
        return true;
    }

private:
    HWND add(int page, HWND h) {
        pages_[page].push_back(h);
        return h;
    }

    // The step badge, and each check's mark: a tick, a warning sign or a
    // cross in its own colour, always beside the words that say it.
    void paintMarks() {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        // Painted into a bitmap and copied, like every other painted surface
        // here: drawn straight onto the window, the marks went missing.
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        {
            Gdiplus::Graphics g(mem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
            const Tok& k = tok();
            Gdiplus::SolidBrush bg(k.surfaceRaised);
            g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);
            paintStepBadge(g, Sf((float)kWizPad), Sf(28.0f),
                           L"Step " + std::to_wstring(page_ + 1) + L" of 3");
            if (page_ == 1) {
                for (const Row& r : rows_) {
                    const char* id = r.state == Check::Ok        ? "check"
                                     : r.state == Check::Problem ? "close"
                                     : r.state == Check::Note    ? "alert"
                                                                 : "info";
                    const Gdiplus::Color c = r.state == Check::Ok ? k.success
                                             : r.state == Check::Problem
                                                 ? k.dangerInk
                                             : r.state == Check::Note
                                                 ? k.warningInk
                                                 : k.primaryInk;
                    drawUiIcon(g, id,
                               Gdiplus::RectF(Sf((float)kWizPad), Sf((float)r.y + 1),
                                              Sf(24), Sf(24)),
                               c);
                }
            }
        }
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd_, &ps);
    }
    void refreshChoices() {
        setChoiceSelected(btnScreen_, onScreen_);
        setChoiceSelected(btnKeypad_, !onScreen_);
        setChoiceSelected(btnType_, !clicksRoute_);
        setChoiceSelected(btnClicks_, clicksRoute_);
    }

    void showPage(int page) {
        if (page_ == 1 && page != 1) listener_.stop();
        page_ = page;
        for (int p = 0; p < 3; ++p)
            for (HWND h : pages_[p]) ShowWindow(h, p == page ? SW_SHOW : SW_HIDE);
        clearRows();
        if (page == 0) {
            setText(heading_, "How do you want to press your buttons?");
            setText(help_, "You can add the other kind any time later.");
            refreshChoices();
        }
        if (page == 1) {
            setText(heading_, onScreen_ ? "Checking this computer"
                                        : "Which keypad should run shortcuts?");
            setText(help_, onScreen_
                               ? "A few quick checks that your buttons will work."
                               : "Press any key on the spare keypad.");
            runChecks();
        }
        if (page == 2) {
            setText(heading_, "What should your first button do?");
            setText(help_, "");
            refreshChoices();
            showButtonPage();
        }
        ShowWindow(help_, page == 2 ? SW_HIDE : SW_SHOW);
        ShowWindow(btnBack_, page > 0 ? SW_SHOW : SW_HIDE);
        updateNext();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    // ---- step 2
    struct Row {
        HWND head = nullptr, detail = nullptr, button = nullptr;
        Check state = Check::Ok;
        int y = 0;   // design px, for the mark beside it
    };

    void clearRows() {
        for (Row& r : rows_) {
            if (r.head) DestroyWindow(r.head);
            if (r.detail) DestroyWindow(r.detail);
            if (r.button) DestroyWindow(r.button);
        }
        rows_.clear();
        keypadRow_ = -1;
        rowY_ = 156;
    }

    // `roomFor` keeps space for a longer detail the row will change to.
    int addRow(Check state, const std::string& head, const std::string& detail,
               int roomFor = 0) {
        const int tx = kWizPad + 36, tw = kWizContentW - 36;
        Row r;
        r.state = state;
        r.y = rowY_;
        r.head = makeText(hwnd_, head, tx, rowY_, tw, 26, tk::Text::Button);
        int h = 28;
        if (!detail.empty()) {
            const int dh = std::max(
                roomFor, std::min(78, textHeight(detail, tk::Text::Body, tw)));
            r.detail = makeText(hwnd_, detail, tx, rowY_ + 28, tw, dh,
                                tk::Text::Body, true);
            h += dh;
        }
        rowY_ += h + tk::kSpace4;
        rows_.push_back(r);
        InvalidateRect(hwnd_, nullptr, TRUE);
        return (int)rows_.size() - 1;
    }

    void setRow(int i, Check state, const std::string& head,
                const std::string& detail) {
        if (i < 0 || i >= (int)rows_.size()) return;
        rows_[(size_t)i].state = state;
        setText(rows_[(size_t)i].head, head);
        if (rows_[(size_t)i].detail) setText(rows_[(size_t)i].detail, detail);
        InvalidateRect(hwnd_, nullptr, TRUE);
        updateNext();
    }

    void runChecks() {
        // Settings.
        if (!canCreateIn(dataDir()))
            addRow(Check::Problem, "Dental Macros can't save your settings",
                   "Windows won't let it save anything on this computer. Ask "
                   "whoever looks after your computers to check it.");
        else if (dataDirIsFallback())
            addRow(Check::Ok, "Your settings can be saved",
                   "They'll be kept in your Windows profile, because Dental "
                   "Macros' own folder can't be changed.");
        else
            addRow(Check::Ok, "Your settings can be saved", "");

        // A keypad needs its keys read and kept from typing.
        if (!onScreen_) {
            HHOOK hook = SetWindowsHookExW(
                WH_KEYBOARD_LL,
                [](int c, WPARAM w, LPARAM l) -> LRESULT {
                    return CallNextHookEx(nullptr, c, w, l);
                },
                GetModuleHandleW(nullptr), 0);
            bool listening = false;
            std::string why;
            try {
                listener_.start(
                    [this](const dev::KeyEvent& ev) {
                        if (!ev.down) return;
                        {
                            std::lock_guard<std::mutex> lk(keyMu_);
                            keys_.push_back(ev);
                        }
                        PostMessageW(hwnd_, WM_WIZ_KEY, 0, 0);
                    },
                    false);
                listening = true;
            } catch (const std::exception& e) {
                why = e.what();
            }
            if (hook) UnhookWindowsHookEx(hook);
            if (!hook || !listening)
                addRow(Check::Problem, "Dental Macros can't read your keypad",
                       "Windows or your security software stopped it. Restart "
                       "the computer and try again, or choose on-screen buttons "
                       "instead.");
            else
                addRow(Check::Ok, "Dental Macros can read your keypad", "");

            if (!ctx.offSwitch.empty())
                addRow(Check::Ok, "Off switch: " + ctx.offSwitch,
                       "Press it anywhere to stop Dental Macros listening.");
            else
                addRow(Check::Note, "No off switch",
                       "Other programs use all the usual shortcuts. You can "
                       "still stop it from the Dental Macros icon by the clock.");
        }

        // The dental programs on this computer - a reassurance, and a warning
        // for one that would silently ignore our typing and clicks.
        int shown = 0;
        for (const discover::DentalProgram& p : discover::findDentalPrograms()) {
            if (shown == 2) break;
            ++shown;
            if (p.elevated)
                addRow(Check::Note, p.name + " is running as administrator",
                       "Windows won't let other programs type or click into "
                       "it. Ask whoever set up this computer to run it "
                       "normally, or run Dental Macros as administrator too.");
            else
                addRow(Check::Ok, "Found " + p.name,
                       p.running ? "It's open now." : "It's installed.");
        }

        if (!onScreen_) {
            keypadRow_ = addRow(Check::Waiting, "Press any key on your keypad",
                                "Dental Macros will recognise it by itself.", 52);
            Row& r = rows_[(size_t)keypadRow_];
            r.button = makeButton(hwnd_, "Use on-screen buttons instead",
                                  kWizPad + 36, rowY_, 300, tk::kTargetMin,
                                  kWizUseScreen);
            setTone(r.button, Tone::Secondary);
            setTextStyle(r.button, tk::Text::Button);
            rowY_ += tk::kTargetMin + tk::kSpace4;
            // Back here from the last step: the key chosen before still counts.
            if (!result.keyName.empty()) showKeypadFound();
        }
        updateNext();
    }

    void onKeypadKey(const dev::KeyEvent& ev) {
        if (page_ != 1 || keypadRow_ < 0) return;
        // Keys this window itself answers to - Enter for Next, Space on a
        // focused button, Tab, Esc - and modifiers held for a shortcut. Pressed
        // on the main keyboard out of habit, they must not become the button.
        switch (ev.vk) {
            case VK_RETURN: case VK_TAB: case VK_ESCAPE: case VK_SPACE:
            case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
            case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
            case VK_MENU: case VK_LMENU: case VK_RMENU:
            case VK_LWIN: case VK_RWIN:
                return;
        }
        const std::vector<dev::InputDevice> all = dev::listKeyboards();
        const dev::InputDevice* hit = nullptr;
        for (const auto& d : all)
            if (d.path == ev.devicePath) { hit = &d; break; }
        if (!hit)
            for (const auto& d : all)
                if (dev::sameDevice(d.path, ev.devicePath)) { hit = &d; break; }
        if (!hit) return;   // not a keyboard at all; wait for another press

        // With one keyboard in the machine, the key can only have come from
        // the one they type on. Taking it would stop them typing.
        if (all.size() < 2) {
            setRow(keypadRow_, Check::Note, "That looks like your main keyboard",
                   "Press a key on the keypad you want to use for buttons. If "
                   "it's wireless, make sure it's switched on and connected.");
            return;
        }
        const std::string key = keys::vkName(ev.vk);
        std::string why;
        if (key.empty() || !cfg::keyBindable(key, &why)) {
            setRow(keypadRow_, Check::Note, "Press a different key", why);
            return;
        }
        bool stable = true;
        result.match = cfg::chooseMatch(*hit, all, &stable);
        result.matchStable = stable;
        result.deviceName = hit->name;
        result.keyName = key;
        showKeypadFound();
    }

    void showKeypadFound() {
        setRow(keypadRow_, Check::Ok,
               "Found your keypad" +
                   (result.deviceName.empty() ? std::string()
                                              : ": " + result.deviceName),
               "You pressed " + keys::displayName(result.keyName) +
                   ", so your first button will be on that key. Press another "
                   "key to choose a different one.");
    }

    bool readyToGoOn() const {
        for (const Row& r : rows_)
            if (r.state == Check::Problem) return false;
        if (!onScreen_ && result.keyName.empty()) return false;
        return true;
    }

    // The main action says what it does - and, while it can't be pressed,
    // what to do instead.
    void updateNext() {
        if (!btnNext_) return;
        bool problem = false;
        for (const Row& r : rows_)
            if (r.state == Check::Problem) problem = true;
        const bool blocked = page_ == 1 && !readyToGoOn();
        EnableWindow(btnNext_, !blocked);
        setText(btnNext_, page_ == 2      ? "Finish setup"
                          : !blocked      ? "Next"
                          : problem       ? "Fix the problem first"
                                          : "Press a keypad key first");
    }

    // ---- step 3
    void showButtonPage() {
        if (page_ != 2) return;
        for (HWND h : {typeLabel_, text_})
            ShowWindow(h, clicksRoute_ ? SW_HIDE : SW_SHOW);
        for (HWND h : {clicksHint_, btnRecord_, clicksStatus_})
            ShowWindow(h, clicksRoute_ ? SW_SHOW : SW_HIDE);
        if (clicksRoute_ && !nameEdited_ && clicks_.empty()) setName("My clicks");
        if (!clicksRoute_ && !nameEdited_) setName(nameFromText(getText(text_)));
        // Already starting with Windows: nothing to ask.
        ShowWindow(login_, ctx.runAtLogin ? SW_HIDE : SW_SHOW);
        ShowWindow(float_, onScreen_ ? SW_SHOW : SW_HIDE);
        if (onScreen_)
            setText(tip_,
                    "Click your button on the floating buttons, or in Dental "
                    "Macros, to run it. You can add more buttons afterwards.");
        else
            setText(tip_,
                    "Your button is the " + keys::displayName(result.keyName) +
                        " key on your keypad. The keypad's other keys still type "
                        "as normal until you give them buttons too." +
                        (ctx.offSwitch.empty()
                             ? std::string()
                             : " If it ever stops typing when you don't want "
                               "it to, press " +
                                   ctx.offSwitch + "."));
        if (!clicksRoute_) {
            SetFocus(text_);
            SendMessageW(text_, EM_SETSEL, 0, -1);
        }
    }

    void setName(const std::string& n) {
        settingName_ = true;
        setText(name_, n);
        settingName_ = false;
    }

    void recordClicks() {
        // Everything of ours out of the way while the user clicks around.
        const bool parentShown = parent_ && IsWindowVisible(parent_);
        ShowWindow(hwnd_, SW_HIDE);
        if (parentShown) ShowWindow(parent_, SW_HIDE);
        std::vector<rec::Click> got;
        rec::Speed speed = speed_;
        const bool ok = recordClicksPanel(hwnd_, got, speed);
        if (parentShown) ShowWindow(parent_, SW_SHOWNA);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        if (!ok) return;
        clicks_ = got;
        speed_ = speed;
        const rec::Target& t = clicks_.front().window;
        setText(clicksStatus_,
                "\xE2\x9C\x93 " + std::to_string(clicks_.size()) +
                    (clicks_.size() == 1 ? " click" : " clicks") + " recorded" +
                    (t.has ? " in " + t.displayName() : "") + ".");
        setDim(clicksStatus_, false);
        setText(btnRecord_, "Record again");
    }

    bool finish() {
        result.onScreen = onScreen_;
        result.title = trim(getText(name_));
        if (result.title.empty()) result.title = "My button";
        if (clicksRoute_) {
            if (clicks_.empty()) {
                showMessage(hwnd_, "Record your clicks first",
                            "Press Record my clicks and show Dental Macros "
                            "where to click.");
                return false;
            }
            result.steps = rec::buildSteps(clicks_, speed_);
        } else {
            const std::string text =
                replaceAll(getText(text_), "\r\n", "\n");
            if (trim(text).empty()) {
                showMessage(hwnd_, "Nothing to type",
                            "Type the words your button should type for you.");
                SetFocus(text_);
                return false;
            }
            cfg::Step step;
            step.action = "type";
            step.params = js::Value::object();
            step.params["text"] = text;
            result.steps = {step};
        }
        result.runAtLogin = ctx.runAtLogin || isChecked(login_);
        result.floating = onScreen_ && isChecked(float_);
        listener_.stop();
        return true;
    }

    std::vector<HWND> pages_[3];
    int page_ = 0;
    bool onScreen_ = true;
    bool clicksRoute_ = false;

    HWND heading_ = nullptr, help_ = nullptr;
    HWND btnScreen_ = nullptr, btnKeypad_ = nullptr, btnType_ = nullptr,
         btnClicks_ = nullptr, btnBack_ = nullptr, btnNext_ = nullptr;
    HWND typeLabel_ = nullptr, text_ = nullptr, clicksHint_ = nullptr,
         btnRecord_ = nullptr, clicksStatus_ = nullptr, name_ = nullptr,
         login_ = nullptr, float_ = nullptr, tip_ = nullptr;
    bool nameEdited_ = false, settingName_ = false;

    std::vector<Row> rows_;
    int rowY_ = 156;
    int keypadRow_ = -1;

    dev::RawInputListener listener_;
    std::mutex keyMu_;
    std::vector<dev::KeyEvent> keys_;

    std::vector<rec::Click> clicks_;
    rec::Speed speed_ = rec::Speed::Normal;
};

}  // namespace

bool runSetupWizard(HWND owner, const SetupContext& ctx, SetupResult& out) {
    SetupWizard w;
    w.ctx = ctx;
    if (!w.run(owner, "Set up Dental Macros", kWizW, kWizH)) return false;
    out = w.result;
    return true;
}

// ------------------------------------------------------------ file pickers
bool chooseImageFile(HWND parent, std::string& relPath) {
    std::string imagesDir = dataPath("images");
    ensureDir(imagesDir);

    wchar_t file[MAX_PATH * 2] = {0};
    if (!relPath.empty()) {
        std::wstring cur = widen(resolveDataPath(relPath));
        wcsncpy(file, cur.c_str(), MAX_PATH * 2 - 1);
    }
    std::wstring initial = widen(imagesDir);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrInitialDir = initial.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return false;
    relPath = relativeToApp(narrow(file));
    return true;
}

namespace {

// The seven key colours and nothing else: offices pick from these, so white
// text stays readable on every key (the design system has no free picker).
class ColorSheet : public Modal {
public:
    int chosen = 0;
    static constexpr int kW = 480;

    void build() override {
        makeText(hwnd_, "Pick a colour", 32, 32, kW - 64, 34, tk::Text::Title);
        const int n = (int)tk::keyColors().size();
        const int size = 48, gap = (kW - 64 - size * n) / (n - 1);
        for (int i = 0; i < n; ++i) {
            const tk::KeyColor& kc = tk::keyColors()[(size_t)i];
            HWND s = makeSwatch(hwnd_, 32 + i * (size + gap), 86, size,
                                ID_FIRST_USER + i, kc.hex, kc.name);
            swatches_.push_back(s);
        }
        name_ = makeText(hwnd_, "", 32, 146, kW - 64, 22, tk::Text::Label);
        const int by = (int)(clientH_ / dpiScale()) - 32 - tk::kTargetBig;
        const int bw = (kW - 64 - 12) / 2;
        HWND cancel = makeButton(hwnd_, "Cancel", 32, by, bw, tk::kTargetBig,
                                 ID_CANCEL);
        setTextStyle(cancel, tk::Text::Button);
        HWND ok = makeButton(hwnd_, "Use this colour", 32 + bw + 12, by, bw,
                             tk::kTargetBig, ID_OK, true);
        setTextStyle(ok, tk::Text::ButtonBig);
        select(chosen);
        SetFocus(swatches_[(size_t)chosen]);
    }

    void onCommand(int id, int notify) override {
        const int i = id - ID_FIRST_USER;
        if (notify == BN_CLICKED && i >= 0 && i < (int)swatches_.size())
            select(i);
    }

private:
    void select(int i) {
        chosen = i;
        for (size_t j = 0; j < swatches_.size(); ++j)
            setSwatchSelected(swatches_[j], (int)j == i);
        setText(name_, std::string("Chosen: ") + tk::keyColors()[(size_t)i].name);
    }

    std::vector<HWND> swatches_;
    HWND name_ = nullptr;
};

}  // namespace

bool chooseColorHex(HWND parent, std::string& hex) {
    ColorSheet d;
    const tk::KeyColor& start = tk::nearestKeyColor(hex);
    for (size_t i = 0; i < tk::keyColors().size(); ++i)
        if (&tk::keyColors()[i] == &start) d.chosen = (int)i;
    if (!d.run(parent, "Dental Macros", ColorSheet::kW, 280)) return false;
    hex = tk::keyColors()[(size_t)d.chosen].hex;
    return true;
}

}  // namespace ui
