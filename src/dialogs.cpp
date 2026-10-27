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
class KeyCaptureDialog : public Modal {
public:
    std::string prompt;
    std::string restrictMatch;
    bool comboMode = false;
    bool allowPickByName = true;
    KeyCaptureResult result;

    ~KeyCaptureDialog() override {
        listener_.stop();
    }

    void build() override {
        makeLabel(hwnd_, prompt, 20, 18, 420, 44, -1, false, true);
        status_ = makeLabel(hwnd_,
                            comboMode ? "Waiting for a keypress..."
                                      : "Waiting for a key or button...",
                            20, 66, 420, 22, -1, true);

        if (allowPickByName) {
            makeLabel(hwnd_, "...or choose a key by name:", 20, 104, 200, 20, -1,
                      true);
            std::vector<std::string> names;
            names.push_back("");
            for (const auto& n : keys::allNames()) names.push_back(n);
            combo_ = makeCombo(hwnd_, 20, 126, 200, 28, ID_FIRST_USER, names, 0);
            makeButton(hwnd_, "Use this key", 228, 126, 110, 28,
                       ID_FIRST_USER + 1);
        }

        layoutFooter(ID_OK, "Done", ID_CANCEL, "Cancel");
        EnableWindow(GetDlgItem(hwnd_, ID_OK), FALSE);

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
            setText(status_,
                    std::string("Keyboard capture is unavailable: ") + e.what());
        }
    }

    void onCommand(int id, int notify) override {
        if (id == ID_FIRST_USER + 1 && notify == BN_CLICKED && combo_) {
            int sel = (int)SendMessageW(combo_, CB_GETCURSEL, 0, 0);
            if (sel <= 0) return;
            wchar_t buf[128] = {0};
            SendMessageW(combo_, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)buf);
            result.ok = true;
            result.keyName = narrow(buf);
            result.combo = result.keyName;
            close(true);
        }
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

            // Left click is how the person in front of this dialog presses
            // Done, so it can never be the answer. Now that the dialog hears
            // the mouse, that has to be said here rather than left to the
            // caller - which would take the click, close, and only then
            // explain itself.
            if (!cfg::keyBindable(name)) {
                setText(status_,
                        "Left click has to keep working - it is how you press "
                        "Done. Any other button is fine.");
                continue;
            }

            if (!restrictMatch.empty() &&
                !cfg::specMatches(restrictMatch, ev.devicePath)) {
                setText(status_,
                        "That came from a different device - press a key or "
                        "button on the one this deck is for.");
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
    HWND status_ = nullptr;
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
    d.run(parent, title, 470, allowPickByName ? 230 : 160);
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
constexpr int kMarkerSize = 44;   // design px; the click point is the centre

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

                const Gdiplus::Color tone =
                    highlight ? theme().warn : theme().accent;
                // A ring exactly on the spot, outlined in dark so it shows up
                // on a white form and a dark one alike.
                const float c = size / 2.0f;
                const float r = Sf(6.0f);
                Gdiplus::Pen outline(Gdiplus::Color(190, 0, 0, 0), Sf(4.5f));
                Gdiplus::Pen ring(tone, Sf(2.2f));
                g.DrawEllipse(&outline, c - r, c - r, 2 * r, 2 * r);
                g.DrawEllipse(&ring, c - r, c - r, 2 * r, 2 * r);

                // The number in a badge up and to the right, clear of the
                // spot so it never hides what is being clicked.
                const float br = Sf(9.5f);
                Gdiplus::RectF badge((Gdiplus::REAL)size - 2 * br - Sf(1.0f),
                                     Sf(1.0f), 2 * br, 2 * br);
                Gdiplus::SolidBrush fill(tone);
                Gdiplus::Pen edge(Gdiplus::Color(190, 0, 0, 0), Sf(1.0f));
                g.FillEllipse(&fill, badge);
                g.DrawEllipse(&edge, badge);
                drawText(g, std::to_wstring(number), badge,
                         number > 9 ? 9.0f : 11.0f,
                         Gdiplus::Color(255, 255, 255, 255), Align::Center,
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

constexpr int kRecW = 400;
constexpr int kRecH = 470;
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
        makeLabel(hwnd_,
                  "1.  Left-click as usual to get to the right screen.\n"
                  "2.  RIGHT-click each spot you want this button to click.\n"
                  "     Right-click twice quickly for a double-click.\n"
                  "3.  Press Done.",
                  18, 14, kRecW - 36, 76);
        list_ = makeList(hwnd_, 18, 96, kRecW - 36, 150, kRecList);
        status_ = makeLabel(hwnd_, "", 18, 254, kRecW - 36, 54, -1, false,
                            true);

        makeLabel(hwnd_, "Pause between clicks", 18, 314, 200, 20, -1, true);
        btnFast_ = makeButton(hwnd_, "Fast", 18, 336, 82, 28, kRecFast);
        btnNormal_ = makeButton(hwnd_, "Normal", 106, 336, 82, 28, kRecNormal);
        btnSlow_ = makeButton(hwnd_, "Slow", 194, 336, 82, 28, kRecSlow);

        btnUndo_ = makeButton(hwnd_, "Undo last", 18, 378, 112, 30, kRecUndo);
        btnRemove_ = makeButton(hwnd_, "Remove", 136, 378, 100, 30,
                                kRecRemove);
        btnRedo_ = makeButton(hwnd_, "Redo this spot", 242, 378, 140, 30,
                              kRecRedo);

        layoutFooter(ID_OK, "Done", ID_CANCEL, "Cancel");
        btnDone_ = GetDlgItem(hwnd_, ID_OK);

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
        const bool yes = askYesNo(
            hwnd_, "Throw away your clicks?",
            "Throw away the " + std::to_string(clicks.size()) +
                (clicks.size() == 1 ? " click" : " clicks") +
                " you've recorded?\n\nChoose No to keep recording.");
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
            said += " Right-click the next spot, or press Done.";
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
        if (btnDone_) EnableWindow(btnDone_, any);
        setPrimary(btnFast_, speed == rec::Speed::Fast);
        setPrimary(btnNormal_, speed == rec::Speed::Normal);
        setPrimary(btnSlow_, speed == rec::Speed::Slow);
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

    const char* noun() const { return wantMouse ? "mouse" : "keyboard"; }

    ~AddKeyboardDialog() override { listener_.stop(); }

    void build() override {
        makeLabel(hwnd_,
                  wantMouse
                      ? "Choose the mouse or trackpad you want to use for "
                        "macros:"
                      : "Choose the keyboard you want to use for macros:",
                  20, 16, 480, 22, -1, false, true);
        list_ = makeList(hwnd_, 20, 44, 480, 150, ID_FIRST_USER);
        makeButton(hwnd_, "Rescan", 20, 202, 96, 30, ID_FIRST_USER + 1);
        identifyBtn_ = makeButton(hwnd_,
                                  wantMouse ? "Identify by pressing a button"
                                            : "Identify by pressing a key",
                                  124, 202, 200, 30, ID_FIRST_USER + 2);
        status_ = makeLabel(hwnd_, "", 20, 240, 480, 22, -1, true);

        warn_ = makeLabel(
            hwnd_,
            wantMouse
                ? "Heads up: buttons on the mouse you pick will stop working "
                  "normally in your apps while Dental Macros is listening. Pick a "
                  "spare mouse, not the one you point with. Left click is "
                  "never taken while it is the only pointer you have, so you "
                  "can always click your way back here."
                : "Heads up: keys on the keyboard you pick will stop typing "
                  "into your apps while Dental Macros is listening - they only "
                  "fire macros. Pick a spare keyboard or numpad, not the one "
                  "you type on.",
            20, 266, 480, 56, -1, true);

        layoutFooter(ID_OK, wantMouse ? "Add mouse" : "Add keyboard",
                     ID_CANCEL, "Cancel");
        rescan();
    }

    void onCommand(int id, int notify) override {
        if (id == ID_FIRST_USER + 1 && notify == BN_CLICKED) rescan();
        if (id == ID_FIRST_USER + 2 && notify == BN_CLICKED) identify();
        if (id == ID_FIRST_USER && notify == LBN_DBLCLK) {
            if (onOk()) close(true);
        }
    }

    bool onOk() override {
        int sel = (int)SendMessageW(list_, LB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= (int)devices_.size()) {
            showMessage(hwnd_, "Nothing selected",
                        std::string("Pick a ") + noun() +
                            " from the list first, or press Rescan if the "
                            "list is empty.");
            return false;
        }
        const dev::InputDevice& d = devices_[(size_t)sel];

        for (const auto& kb : config->keyboards) {
            // A keypad's dial gets added as a mouse after its keys were added
            // as a keyboard, and both decks match the same keypad. Only the
            // same kind of deck twice is a duplicate.
            if (kb.isMouse != wantMouse) continue;
            if (cfg::specMatches(kb.match, d.path)) {
                if (!askYesNo(hwnd_, "Already added",
                              std::string("This ") + noun() +
                                  " is already set up as \"" + kb.alias +
                                  "\".\n\nAdd it a second time anyway?"))
                    return false;
                break;
            }
        }

        bool stable = true;
        std::string match = cfg::chooseMatch(d, devices_, &stable);

        std::string suggested = config->uniqueAlias(
            !d.name.empty() ? d.name
                            : (wantMouse ? "Macro mouse" : "Macro pad"));
        if (!askText(hwnd_,
                     wantMouse ? "Name this mouse" : "Name this keyboard",
                     "What do you want to call it?", suggested))
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
            showMessage(hwnd_, wantMouse ? "Mouse added" : "Keyboard added",
                        std::string("Two identical ") +
                            (wantMouse ? "mice are" : "keyboards are") +
                            " attached, so Dental Macros had to identify this one "
                            "by which USB port it is plugged into.\n\nKeep it "
                            "in the same port, or it will stop being "
                            "recognised.");
        return true;
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
                SendMessageW(list_, LB_SETCURSEL, (WPARAM)hit, 0);
                setText(status_,
                        std::string(wantMouse ? "That button came from: "
                                              : "That key came from: ") +
                            devices_[hit].display());
                stopIdentify();
                out = 0;
                return true;
            }
        }
        out = 0;
        return true;
    }

private:
    void rescan() {
        devices_ = wantMouse ? dev::listMice() : dev::listKeyboards();
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        for (const auto& d : devices_)
            SendMessageW(list_, LB_ADDSTRING, 0,
                         (LPARAM)widen(d.display()).c_str());
        if (!devices_.empty()) SendMessageW(list_, LB_SETCURSEL, 0, 0);
        setText(status_,
                devices_.empty()
                    ? (wantMouse ? "No mice detected." : "No keyboards detected.")
                    : std::to_string(devices_.size()) + std::string(" ") +
                          noun() + " device(s) found.");
    }

    void identify() {
        if (identifying_) {
            stopIdentify();
            return;
        }
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
            identifying_ = true;
            setText(identifyBtn_, "Stop listening");
            setText(status_,
                    wantMouse
                        ? "Press a button on the mouse you want to use..."
                        : "Press a key on the keyboard you want to use...");
        } catch (const std::exception& e) {
            setText(status_, std::string("Could not listen for input: ") +
                                 e.what());
        }
    }

    void stopIdentify() {
        if (!identifying_) return;
        listener_.stop();
        identifying_ = false;
        setText(identifyBtn_, "Identify by pressing a key");
    }

    HWND list_ = nullptr, status_ = nullptr, warn_ = nullptr,
         identifyBtn_ = nullptr;
    std::vector<dev::InputDevice> devices_;
    dev::RawInputListener listener_;
    bool identifying_ = false;
    std::mutex mu_;
    std::vector<dev::KeyEvent> pending_;
};

}  // namespace

bool addKeyboardDialog(HWND parent, cfg::Config& config,
                       std::string& newAlias, bool mouse) {
    AddKeyboardDialog d;
    d.config = &config;
    d.wantMouse = mouse;
    if (!d.run(parent, mouse ? "Add a macro mouse" : "Add a macro keyboard",
               540, 390))
        return false;
    newAlias = d.newAlias;
    return !newAlias.empty();
}

// ------------------------------------------------------------- step editor
namespace {

struct FieldCtl {
    cfg::FieldDef def;
    HWND label = nullptr;
    HWND input = nullptr;
    HWND extra = nullptr;   // Browse / Record / dropdown button
    HWND extra2 = nullptr;  // Snip, beside Browse on a picture field
    HWND hint = nullptr;
};

// Design-pixel geometry. The action picker sits above the scrolling field
// area and the buttons below it, so a step with a lot of parameters scrolls
// rather than pushing OK off the bottom of the screen.
constexpr int kStepDlgW = 480;
constexpr int kStepHeaderH = 100;
constexpr int kStepFooterH = 58;
constexpr int kFieldLeft = 20;
constexpr int kFieldW = 420;

class StepDialog : public Modal {
public:
    cfg::Step step;
    cfg::Settings settings;

    void build() override {
        makeLabel(hwnd_, "What should this step do?", 20, 16, 300, 20, -1, false,
                  true);

        std::vector<std::string> items;
        for (const auto& cat : cfg::actionCategories())
            for (const auto& a : cfg::actionDefs())
                if (cat == a.category) {
                    order_.push_back(a.id);
                    items.push_back(std::string(a.category) + "   -   " +
                                    a.label);
                }
        int sel = 0;
        for (size_t i = 0; i < order_.size(); ++i)
            if (order_[i] == step.action) sel = (int)i;
        if (step.action.empty()) step.action = order_.empty() ? "" : order_[0];

        combo_ = makeCombo(hwnd_, 20, 40, 440, 28, ID_FIRST_USER, items, sel);
        blurb_ = makeLabel(hwnd_, "", 20, 72, 440, 20, -1, true);

        pane_ = makeScrollPane(hwnd_, 0, kStepHeaderH, kStepDlgW, 200,
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
                    hwnd_, "Record a hotkey",
                    "Hold the modifiers you want and press the final key.", "",
                    true, false);
                if (r.ok) setText(f.input, r.combo);
                break;
            }
            case cfg::FieldKind::Layout: {
                HMENU menu = CreatePopupMenu();
                auto names = win::listLayouts(settings.layoutsDir);
                if (names.empty())
                    AppendMenuW(menu, MF_STRING | MF_GRAYED, 1,
                                L"(no layouts saved yet)");
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
                    showMessage(hwnd_, "Missing information",
                                std::string("\"") + d.label +
                                    "\" needs a value.");
                    SetFocus(f.input);
                    return false;
                }
                continue;
            }

            if (d.kind == cfg::FieldKind::Int || d.kind == cfg::FieldKind::Float) {
                char* end = nullptr;
                double v = strtod(text.c_str(), &end);
                if (!end || *end != '\0') {
                    if (validate) {
                        showMessage(hwnd_, "Not a number",
                                    std::string("\"") + d.label +
                                        "\" should be a number.");
                        SetFocus(f.input);
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

    void rebuildFields() {
        for (auto& f : fields_) {
            if (f.label) DestroyWindow(f.label);
            if (f.input) DestroyWindow(f.input);
            if (f.extra) DestroyWindow(f.extra);
            if (f.extra2) DestroyWindow(f.extra2);
            if (f.hint) DestroyWindow(f.hint);
        }
        fields_.clear();
        if (continueChk_) { DestroyWindow(continueChk_); continueChk_ = nullptr; }

        const cfg::ActionDef* def = cfg::findAction(step.action);
        if (!def) return;
        setText(blurb_, def->blurb);

        int y = 8;
        int index = 0;
        for (const cfg::FieldDef& fd : def->fields) {
            FieldCtl f;
            f.def = fd;
            const int helperId = ID_FIRST_USER + 100 + index * 2;

            bool wide = fd.kind == cfg::FieldKind::Multiline;
            int inputH = wide ? 76 : 26;

            if (fd.kind != cfg::FieldKind::Bool)
                f.label = makeLabel(pane_, std::string(fd.label) +
                                               (fd.required ? " *" : ""),
                                    kFieldLeft, y, 200, 20, -1, true);

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

            int inputY = (fd.kind == cfg::FieldKind::Bool) ? y : y + 22;
            switch (fd.kind) {
                case cfg::FieldKind::Bool: {
                    bool on = v ? (v->isBool() ? v->b : lower(current) == "true")
                                : lower(trim(fd.def)) == "true";
                    f.input = makeCheck(pane_, fd.label, kFieldLeft, inputY,
                                        kFieldW, 24,
                                        ID_FIRST_USER + 500 + index, on);
                    break;
                }
                case cfg::FieldKind::Choice: {
                    std::vector<std::string> choices = split(fd.choices, ',');
                    int sel = 0;
                    for (size_t i = 0; i < choices.size(); ++i)
                        if (choices[i] == current) sel = (int)i;
                    f.input = makeCombo(pane_, kFieldLeft, inputY, 200, 26,
                                        ID_FIRST_USER + 500 + index, choices,
                                        sel);
                    break;
                }
                case cfg::FieldKind::Multiline:
                    f.input = makeEdit(pane_, current, kFieldLeft, inputY,
                                       kFieldW, inputH,
                                       ID_FIRST_USER + 500 + index, true);
                    break;
                case cfg::FieldKind::File:
                    // A picture to find on screen: Snip is how most people
                    // will make one, so it sits right beside Browse.
                    if (std::string(fd.key) == "image") {
                        f.input = makeEdit(pane_, current, kFieldLeft, inputY,
                                           224, inputH,
                                           ID_FIRST_USER + 500 + index);
                        f.extra = makeButton(pane_, "Browse", kFieldLeft + 232,
                                             inputY, 92, 26, helperId);
                        f.extra2 = makeButton(pane_, "Snip...",
                                              kFieldLeft + 328, inputY, 92, 26,
                                              helperId + 1);
                        break;
                    }
                    [[fallthrough]];
                case cfg::FieldKind::KeyCombo:
                case cfg::FieldKind::Layout: {
                    f.input = makeEdit(pane_, current, kFieldLeft, inputY, 320,
                                       inputH, ID_FIRST_USER + 500 + index);
                    const char* btn = fd.kind == cfg::FieldKind::File ? "Browse"
                                      : fd.kind == cfg::FieldKind::KeyCombo
                                          ? "Record"
                                          : "Choose";
                    f.extra = makeButton(pane_, btn, kFieldLeft + 328, inputY,
                                         92, 26, helperId);
                    break;
                }
                default:
                    f.input = makeEdit(pane_, current, kFieldLeft, inputY,
                                       kFieldW, inputH,
                                       ID_FIRST_USER + 500 + index);
                    break;
            }

            y = inputY + inputH + 4;
            if (fd.hint && *fd.hint) {
                f.hint = makeLabel(pane_, fd.hint, kFieldLeft, y, kFieldW, 18,
                                   -1, true);
                y += 20;
            }
            y += 8;
            fields_.push_back(f);
            ++index;
        }

        // Applies to every action, so it lives outside the schema.
        y += 4;
        continueChk_ = makeCheck(pane_, "Keep going if this step fails",
                                 kFieldLeft, y, kFieldW, 24, ID_FIRST_USER + 3,
                                 step.continueOnError());
        y += 32;

        layoutForContent(y);
    }

    // Size the window to the fields, but never past the edge of the screen -
    // beyond that the field area scrolls instead.
    void layoutForContent(int contentH) {
        const int roomForClient = availableClientHeight(parent_);
        const int wantClient =
            std::min(kStepHeaderH + contentH + kStepFooterH, roomForClient);
        const int paneH = std::max(60, wantClient - kStepHeaderH - kStepFooterH);

        RECT rc{0, 0, S(kStepDlgW), S(wantClient)};
        DWORD style = (DWORD)GetWindowLongPtrW(hwnd_, GWL_STYLE);
        AdjustWindowRectEx(&rc, style, FALSE, 0);
        SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left,
                     rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
        RECT cr;
        GetClientRect(hwnd_, &cr);
        clientW_ = cr.right;
        clientH_ = cr.bottom;

        MoveWindow(pane_, 0, S(kStepHeaderH), S(kStepDlgW), S(paneH), TRUE);
        setScrollPaneContent(pane_, contentH);

        if (ok_) DestroyWindow(ok_);
        if (cancel_) DestroyWindow(cancel_);
        const int bw = 104, bh = 32, pad = 16;
        const int by = wantClient - bh - pad;
        ok_ = makeButton(hwnd_, "OK", kStepDlgW - bw - pad, by, bw, bh, ID_OK,
                         true);
        cancel_ = makeButton(hwnd_, "Cancel", kStepDlgW - bw * 2 - pad - 8, by,
                             bw, bh, ID_CANCEL, false);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    HWND combo_ = nullptr, blurb_ = nullptr, ok_ = nullptr, cancel_ = nullptr;
    HWND pane_ = nullptr, continueChk_ = nullptr;
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
    if (!d.run(parent, "Macro step", kStepDlgW, 420)) return false;
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
            showMessage(parent, "Snip failed", err, true);
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
            showMessage(parent, "Selection too small",
                        "Drag a box around the button or icon you want to "
                        "match.");
            return false;
        }

        std::string name = "template";
        if (!askText(parent, "Save template",
                     "Name for this template image:", name,
                     "Saved into the images folder as a .png"))
            return false;
        std::string rel = "images\\" + name + ".png";
        std::string saveErr;
        if (!vis::savePng(rel, cap_, crop, &saveErr)) {
            showMessage(parent, "Could not save", saveErr, true);
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
        HDC mem = CreateCompatibleDC(hdc);
        HGDIOBJ old = SelectObject(mem, bmp_);
        BitBlt(hdc, 0, 0, cap_.width, cap_.height, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);

        // Dim everything, then punch the selection back through at full
        // brightness so the user sees exactly what will be saved.
        RECT sel = selection();
        HDC dim = CreateCompatibleDC(hdc);
        HBITMAP dimBmp = CreateCompatibleBitmap(hdc, cap_.width, cap_.height);
        HGDIOBJ oldDim = SelectObject(dim, dimBmp);
        HBRUSH black = CreateSolidBrush(RGB(10, 12, 16));
        RECT full{0, 0, cap_.width, cap_.height};
        FillRect(dim, &full, black);
        DeleteObject(black);
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 130, 0};
        AlphaBlend(hdc, 0, 0, cap_.width, cap_.height, dim, 0, 0, cap_.width,
                   cap_.height, bf);
        SelectObject(dim, oldDim);
        DeleteObject(dimBmp);
        DeleteDC(dim);

        if (dragging_ || accepted_) {
            HDC mem2 = CreateCompatibleDC(hdc);
            HGDIOBJ o2 = SelectObject(mem2, bmp_);
            BitBlt(hdc, sel.left, sel.top, sel.right - sel.left,
                   sel.bottom - sel.top, mem2, sel.left, sel.top, SRCCOPY);
            SelectObject(mem2, o2);
            DeleteDC(mem2);

            HPEN pen = CreatePen(PS_SOLID, 2, RGB(79, 140, 255));
            HGDIOBJ op = SelectObject(hdc, pen);
            HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, sel.left, sel.top, sel.right, sel.bottom);
            SelectObject(hdc, op);
            SelectObject(hdc, ob);
            DeleteObject(pen);

            wchar_t size[64];
            swprintf(size, 64, L"%d x %d", (int)(sel.right - sel.left),
                     (int)(sel.bottom - sel.top));
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            HGDIOBJ of = SelectObject(hdc, uiFont(14, true));
            TextOutW(hdc, sel.left, std::max(0L, sel.top - S(22)), size,
                     (int)wcslen(size));
            SelectObject(hdc, of);
        }

        const wchar_t* help =
            L"Drag a box around the button or icon  \x2022  Esc to cancel";
        SetBkMode(hdc, TRANSPARENT);
        HGDIOBJ of = SelectObject(hdc, uiFont(18, true));
        RECT tr{0, S(24), cap_.width, S(64)};
        SetTextColor(hdc, RGB(0, 0, 0));
        OffsetRect(&tr, 1, 1);
        DrawTextW(hdc, help, -1, &tr, DT_CENTER | DT_SINGLELINE);
        OffsetRect(&tr, -1, -1);
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, help, -1, &tr, DT_CENTER | DT_SINGLELINE);
        SelectObject(hdc, of);
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

constexpr int kSettingsW = 480;
constexpr int kSettingsH = 500;
constexpr int kSettingsFooterH = 58;

enum {
    kSetStartEngine = ID_FIRST_USER,
    kSetStartMin,
    kSetToTray,
    kSetRunAtLogin,
    kSetHotkey,
    kSetHotkeyRecord,
    kSetBlocking,
    kSetConfidence,
    kSetSaveCopy,
    kSetLoadCopy,
    kSetShowFile,
    kSetReload,
    kSetImport,
    kSetAbout,
    kSetRunSetup,
    kSetPane,
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
                               kSettingsH - kSettingsFooterH, kSetPane);

        int y = 16;
        heading("Startup", y);
        runAtLogin_ = makeCheck(pane_, "Start Dental Macros when I sign in to Windows",
                                20, y, 420, 24, kSetRunAtLogin,
                                runAtLoginEnabled());
        y += 28;
        startEngine_ = makeCheck(pane_, "Start listening as soon as it opens",
                                 20, y, 420, 24, kSetStartEngine,
                                 settings.startEngineOnLaunch);
        y += 28;
        toTray_ = makeCheck(pane_,
                            "Closing the window keeps it running by the clock",
                            20, y, 420, 24, kSetToTray,
                            settings.minimizeToTray);
        y += 28;
        startMin_ = makeCheck(pane_, "Start hidden, with just the icon by the clock",
                              20, y, 420, 24, kSetStartMin,
                              settings.startMinimized);
        y += 40;

        heading("Keypads", y);
        makeLabel(pane_, "Emergency off switch", 20, y, 300, 20, -1, true);
        y += 22;
        hotkey_ = makeEdit(pane_, settings.toggleHotkey, 20, y, 320, 26,
                           kSetHotkey);
        makeButton(pane_, "Record", 348, y, 92, 26, kSetHotkeyRecord);
        y += 30;
        makeLabel(pane_,
                  "Press it anywhere to stop or start listening, so a keypad "
                  "that has stopped typing can always be given back. Needs "
                  "Ctrl, Alt, Shift or Win. Leave blank for none.",
                  20, y, 420, 54, -1, true);
        y += 62;

        makeLabel(pane_, "New keypads", 20, y, 300, 20, -1, true);
        y += 22;
        const int choice = !settings.blockByDefault ? kBlockNone
                           : settings.blockAllKeysByDefault ? kBlockAll
                                                            : kBlockMacroKeys;
        blocking_ = makeCombo(
            pane_, 20, y, 420, 28, kSetBlocking,
            {"Stop typing all their keys (best for a spare keypad)",
             "Stop typing only the keys that run buttons",
             "Keep typing as well as running buttons"},
            choice);
        y += 32;
        makeLabel(pane_,
                  "Choose the middle one if your button keys are F13-F24 or "
                  "extra keys on a keyboard you also type on. Right-click a "
                  "keypad in the list on the left to change just that one.",
                  20, y, 420, 52, -1, true);
        y += 58;

        makeLabel(pane_, "How closely a picture on screen must match (0.5 - 1.0)",
                  20, y, 420, 20, -1, true);
        y += 22;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", settings.matchConfidence);
        confidence_ = makeEdit(pane_, buf, 20, y, 90, 26, kSetConfidence);
        makeLabel(pane_, "Lower forgives more but may match the wrong thing. "
                         "0.85 suits most buttons.",
                  120, y - 2, 320, 36, -1, true);
        y += 46;

        heading("Backup and restore", y);
        makeLabel(pane_,
                  "Every change saves as you make it, and a copy of each "
                  "day's settings is kept automatically (the last 10).",
                  20, y, 420, 36, -1, true);
        y += 42;
        makeButton(pane_, "Save a copy...", 20, y, 206, 30, kSetSaveCopy);
        makeButton(pane_, "Load a copy...", 234, y, 206, 30, kSetLoadCopy);
        y += 36;
        makeButton(pane_, "Show my settings file", 20, y, 206, 30,
                   kSetShowFile);
        makeButton(pane_, "Reload from file", 234, y, 206, 30, kSetReload);
        y += 36;
        makeButton(pane_, "Import from macrokb...", 20, y, 206, 30, kSetImport);
        makeButton(pane_, "Run setup again...", 234, y, 206, 30, kSetRunSetup);
        y += 44;

        setScrollPaneContent(pane_, y);
        layoutFooter(ID_OK, "Save", ID_CANCEL, "Cancel");
        // The footer has room on the left for About.
        makeButton(hwnd_, "About", 16,
                   (int)(clientH_ / dpiScale()) - 32 - 16, 104, 32, kSetAbout);
    }

    void onCommand(int id, int notify) override {
        if (notify != BN_CLICKED) return;
        switch (id) {
            case kSetHotkeyRecord: {
                KeyCaptureResult r = captureKey(
                    hwnd_, "Record the emergency off switch",
                    "Hold Ctrl, Alt, Shift or Win and press the final key.", "",
                    true, false);
                if (r.ok) setText(hotkey_, r.combo);
                break;
            }
            case kSetShowFile: showSettingsFile(); break;
            case kSetAbout:
                showMessage(hwnd_, "About Dental Macros", aboutText());
                break;
            case kSetSaveCopy: finishWith(SettingsAction::SaveCopy); break;
            case kSetLoadCopy: finishWith(SettingsAction::LoadCopy); break;
            case kSetReload: finishWith(SettingsAction::Reload); break;
            case kSetImport: finishWith(SettingsAction::ImportMacroKb); break;
            case kSetRunSetup: finishWith(SettingsAction::RunSetup); break;
        }
    }

    bool onOk() override {
        settings.startEngineOnLaunch = isChecked(startEngine_);
        settings.startMinimized = isChecked(startMin_);
        settings.minimizeToTray = isChecked(toTray_);

        const int choice = (int)SendMessageW(blocking_, CB_GETCURSEL, 0, 0);
        settings.blockByDefault = choice != kBlockNone;
        // "Keep typing" leaves the second setting as it was: it only matters
        // once blocking is back on, and then the old answer is still right.
        if (choice == kBlockAll) settings.blockAllKeysByDefault = true;
        if (choice == kBlockMacroKeys) settings.blockAllKeysByDefault = false;

        const std::string combo = trim(getText(hotkey_));
        if (!combo.empty()) {
            unsigned mods = 0;
            int vk = 0;
            std::string err;
            if (!keys::parseHotkey(combo, mods, vk, &err)) {
                showMessage(hwnd_, "Check the off switch",
                            "\"" + combo + "\" won't work as a shortcut: " +
                                err + ".\n\nPress Record and hold Ctrl, Alt, "
                                "Shift or Win while you press a key.");
                SetFocus(hotkey_);
                return false;
            }
        }
        settings.toggleHotkey = combo;

        double c = atof(getText(confidence_).c_str());
        if (c < cfg::kMinConfidence || c > cfg::kMaxConfidence) {
            showMessage(hwnd_, "Check that number",
                        "How closely a picture must match should be between "
                        "0.5 and 1.0. Most buttons work with 0.85.");
            SetFocus(confidence_);
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
                showMessage(hwnd_, "Couldn't change startup",
                            "Windows didn't let Dental Macros change whether "
                            "it starts when you sign in. Everything else was "
                            "saved.\n\nDetails: " + err,
                            true);
        }
        return true;
    }

private:
    void heading(const char* text, int& y) {
        makeLabel(pane_, text, 20, y, 300, 20, -1, false, true);
        y += 26;
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
         runAtLogin_ = nullptr, hotkey_ = nullptr, blocking_ = nullptr,
         confidence_ = nullptr;
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
    if (!d.run(parent, "Dental Macros settings", kSettingsW, kSettingsH))
        return false;
    settings = d.settings;
    if (action) *action = d.action;
    return true;
}

// ------------------------------------------------------ simple-mode prompts
namespace {

class LongTextPrompt : public Modal {
public:
    std::string prompt, hint, value;

    void build() override {
        makeLabel(hwnd_, prompt, 18, 16, 404, 22, -1, false, true);
        edit_ = makeEdit(hwnd_, value, 18, 44, 404, 110, ID_FIRST_USER, true);
        if (!hint.empty()) makeLabel(hwnd_, hint, 18, 160, 404, 20, -1, true);
        layoutFooter(ID_OK, "OK", ID_CANCEL, "Cancel");
        SetFocus(edit_);
        SendMessageW(edit_, EM_SETSEL, 0, -1);
    }

    bool onOk() override {
        // A multi-line edit gives "\r\n"; the step stores plain "\n".
        value = replaceAll(getText(edit_), "\r\n", "\n");
        if (trim(value).empty()) {
            showMessage(hwnd_, "Nothing to type",
                        "Type the words this button should type for you.");
            SetFocus(edit_);
            return false;
        }
        return true;
    }

private:
    HWND edit_ = nullptr;
};

constexpr int kWaitChoiceFirst = ID_FIRST_USER;
const double kWaitChoices[] = {0.5, 1.0, 2.0, 5.0};
const char* kWaitLabels[] = {"Half a second", "1 second", "2 seconds",
                             "5 seconds"};

class WaitPrompt : public Modal {
public:
    double seconds = 1.0;

    void build() override {
        makeLabel(hwnd_, "How long should it wait?", 18, 16, 380, 22, -1, false,
                  true);
        makeLabel(hwnd_,
                  "Give a program time to open a screen or a menu before "
                  "the next step.",
                  18, 42, 380, 36, -1, true);
        for (int i = 0; i < 4; ++i) {
            HWND b = makeButton(hwnd_, kWaitLabels[i], 18 + (i % 2) * 192,
                                86 + (i / 2) * 40, 184, 32,
                                kWaitChoiceFirst + i);
            if (std::fabs(kWaitChoices[i] - seconds) < 1e-9) setPrimary(b, true);
        }
        layoutFooter(ID_CANCEL, "Cancel", ID_CANCEL, "");
    }

    void onCommand(int id, int notify) override {
        if (notify != BN_CLICKED) return;
        const int i = id - kWaitChoiceFirst;
        if (i < 0 || i >= 4) return;
        seconds = kWaitChoices[i];
        close(true);
    }
};

}  // namespace

bool askLongText(HWND parent, const std::string& title,
                 const std::string& prompt, std::string& value,
                 const std::string& hint) {
    LongTextPrompt d;
    d.prompt = prompt;
    d.hint = hint;
    d.value = value;
    if (!d.run(parent, title, 440, 250)) return false;
    value = d.value;
    return true;
}

bool chooseWait(HWND parent, double& seconds) {
    WaitPrompt d;
    d.seconds = seconds;
    if (!d.run(parent, "Wait", 416, 230)) return false;
    seconds = d.seconds;
    return true;
}

// ------------------------------------------------------- explained problems
namespace {

constexpr int kFriendlyW = 440;
constexpr int kCopyDetailsId = ID_FIRST_USER;

// How tall `text` wraps to at `width` design px in the body font, in design px.
int wrappedHeight(const std::string& text, int width) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, uiFont(14));
    RECT r{0, 0, S(width), 0};
    const std::wstring w = widen(text);
    DrawTextW(dc, w.c_str(), (int)w.size(), &r,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return (int)std::ceil((r.bottom - r.top) / dpiScale()) + 6;
}

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

class FriendlyDialog : public Modal {
public:
    fe::Friendly f;
    int bodyH = 60;

    void build() override {
        makeLabel(hwnd_, f.title, 20, 18, kFriendlyW - 40, 24, -1, false, true);
        makeLabel(hwnd_, f.body, 20, 50, kFriendlyW - 40, bodyH);
        if (f.action.empty())
            layoutFooter(ID_OK, "OK", ID_CANCEL, "");
        else
            layoutFooter(ID_OK, f.action, ID_CANCEL, "Close");
        // The technical text is never shown, only offered for whoever helps.
        if (!f.technical.empty())
            copy_ = makeButton(hwnd_, "Copy details", 16,
                               (int)(clientH_ / dpiScale()) - 32 - 16, 120, 32,
                               kCopyDetailsId);
    }

    void onCommand(int id, int notify) override {
        if (id != kCopyDetailsId || notify != BN_CLICKED) return;
        copyText(hwnd_, f.title + "\r\n\r\n" + f.body + "\r\n\r\nDetails: " +
                            f.technical);
        setText(copy_, "Copied");
    }

private:
    HWND copy_ = nullptr;
};

}  // namespace

bool showFriendly(HWND parent, const fe::Friendly& f) {
    FriendlyDialog d;
    d.f = f;
    d.bodyH = wrappedHeight(f.body, kFriendlyW - 40);
    const int h = 50 + d.bodyH + 16 + 64;
    const bool pressed = d.run(parent, "Dental Macros", kFriendlyW, h);
    return pressed && !f.action.empty();
}

// ---------------------------------------------------------- first-run setup
// Three pages in one window: two questions, a page of checks (and, for a
// keypad, "press any key on it"), and the first button. Everything it learns
// comes back in a SetupResult; the main window turns that into a deck.
namespace {

constexpr int kWizW = 560;
constexpr int kWizH = 500;
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

const char* checkMark(Check c) {
    switch (c) {
        case Check::Ok: return "\xE2\x9C\x93  ";        // check mark
        case Check::Note: return "\xE2\x9A\xA0  ";      // warning sign
        case Check::Problem: return "\xE2\x9C\x97  ";   // ballot x
        case Check::Waiting: default: return "\xE2\x80\xA6  ";
    }
}

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

class SetupWizard : public Modal {
public:
    SetupContext ctx;
    SetupResult result;

    ~SetupWizard() override { listener_.stop(); }

    void build() override {
        const int fy = kWizH - 32 - 16;
        // ---- page 1: two questions
        add(0, makeLabel(hwnd_, "Welcome to Dental Macros", 24, 20, 512, 26, -1,
                         false, true));
        add(0, makeLabel(hwnd_,
                         "Make buttons that type and click for you. Let's set up "
                         "your first one - it takes about a minute.",
                         24, 50, 512, 40, -1, true));
        add(0, makeLabel(hwnd_, "Where will your buttons be?", 24, 104, 512, 22,
                         -1, false, true));
        btnScreen_ = add(0, makeButton(hwnd_, "On my screen", 24, 132, 250, 34,
                                       kWizScreen));
        btnKeypad_ = add(0, makeButton(hwnd_, "On a keypad", 286, 132, 250, 34,
                                       kWizKeypad));
        add(0, makeLabel(hwnd_,
                         "A small row of buttons that floats over your other "
                         "programs. Nothing to plug in.",
                         24, 172, 250, 40, -1, true));
        add(0, makeLabel(hwnd_,
                         "A spare keypad or number pad. Its keys run your "
                         "buttons instead of typing.",
                         286, 172, 250, 40, -1, true));
        add(0, makeLabel(hwnd_, "What should your first button do?", 24, 232,
                         512, 22, -1, false, true));
        btnType_ = add(0, makeButton(hwnd_, "Type something", 24, 260, 250, 34,
                                     kWizType));
        btnClicks_ = add(0, makeButton(hwnd_, "Click through screens", 286, 260,
                                       250, 34, kWizClicks));
        add(0, makeLabel(hwnd_,
                         "A phrase, a note or a code you type all day.", 24,
                         300, 250, 40, -1, true));
        add(0, makeLabel(hwnd_,
                         "Click the same spots in a program for you, in order.",
                         286, 300, 250, 40, -1, true));

        // ---- page 2: checks (rows are made when the page is shown)
        add(1, makeLabel(hwnd_, "Checking everything works", 24, 20, 512, 26,
                         -1, false, true));
        checkSub_ = add(1, makeLabel(hwnd_, "", 24, 50, 512, 22, -1, true));

        // ---- page 3: the first button
        add(2, makeLabel(hwnd_, "Make your first button", 24, 20, 512, 26, -1,
                         false, true));
        typeLabel_ = add(2, makeLabel(hwnd_, "What should it type?", 24, 58,
                                      512, 22, -1, false, true));
        text_ = add(2, makeEdit(hwnd_,
                                "Thank you for visiting! See you at your next "
                                "cleaning.",
                                24, 84, 512, 86, kWizText, true));
        clicksLabel_ = add(2, makeLabel(hwnd_, "Show us where to click", 24, 58,
                                        512, 22, -1, false, true));
        clicksHint_ = add(2, makeLabel(
            hwnd_,
            "A small window will guide you: left-click as usual to get to the "
            "right screen, then right-click each spot you want clicked.",
            24, 84, 512, 40, -1, true));
        btnRecord_ = add(2, makeButton(hwnd_, "Record my clicks", 24, 130, 200,
                                       34, kWizRecord, true));
        clicksStatus_ = add(2, makeLabel(hwnd_, "No clicks yet.", 236, 137, 300,
                                         22, -1, true));
        add(2, makeLabel(hwnd_, "Name on the button", 24, 184, 300, 20, -1,
                         true));
        name_ = add(2, makeEdit(hwnd_, "", 24, 206, 300, 26, kWizName));
        login_ = add(2, makeCheck(hwnd_,
                                  "Start Dental Macros when this computer starts",
                                  24, 250, 512, 24, kWizLogin, true));
        float_ = add(2, makeCheck(hwnd_,
                                  "Show my buttons on top of other windows", 24,
                                  278, 512, 24, kWizFloat, true));
        tip_ = add(2, makeLabel(hwnd_, "", 24, 316, 512, 60, -1, true));

        // ---- footer, on every page
        makeButton(hwnd_, "Skip setup", 24, fy, 120, 32, ID_CANCEL);
        btnBack_ = makeButton(hwnd_, "Back", kWizW - 24 - 120 - 8 - 120, fy, 120,
                              32, kWizBack);
        btnNext_ = makeButton(hwnd_, "Next", kWizW - 24 - 120, fy, 120, 32,
                              ID_OK, true);

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
            case kWizType: clicksRoute_ = false; refreshChoices(); break;
            case kWizClicks: clicksRoute_ = true; refreshChoices(); break;
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
        return askYesNo(hwnd_, "Skip setup?",
                        "You can make buttons yourself instead, or run this "
                        "again any time from Settings.");
    }

    void onCancel() override { listener_.stop(); }

    bool onMessage(UINT msg, WPARAM, LPARAM, LRESULT& out) override {
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

    void refreshChoices() {
        setPrimary(btnScreen_, onScreen_);
        setPrimary(btnKeypad_, !onScreen_);
        setPrimary(btnType_, !clicksRoute_);
        setPrimary(btnClicks_, clicksRoute_);
    }

    void showPage(int page) {
        if (page_ == 1 && page != 1) listener_.stop();
        page_ = page;
        for (int p = 0; p < 3; ++p)
            for (HWND h : pages_[p]) ShowWindow(h, p == page ? SW_SHOW : SW_HIDE);
        clearRows();
        if (page == 0) refreshChoices();
        if (page == 1) runChecks();
        if (page == 2) showButtonPage();
        ShowWindow(btnBack_, page > 0 ? SW_SHOW : SW_HIDE);
        setText(btnNext_, page == 2 ? "Finish" : "Next");
        updateNext();
    }

    // ---- page 2
    struct Row {
        HWND head = nullptr, detail = nullptr, button = nullptr;
        Check state = Check::Ok;
    };

    void clearRows() {
        for (Row& r : rows_) {
            if (r.head) DestroyWindow(r.head);
            if (r.detail) DestroyWindow(r.detail);
            if (r.button) DestroyWindow(r.button);
        }
        rows_.clear();
        keypadRow_ = -1;
        rowY_ = 84;
    }

    // `roomFor` keeps space for a longer detail the row will change to.
    int addRow(Check state, const std::string& head, const std::string& detail,
               int roomFor = 0) {
        Row r;
        r.state = state;
        r.head = makeLabel(hwnd_, checkMark(state) + head, 24, rowY_, 512, 22,
                           -1, false, true);
        int h = 24;
        if (!detail.empty()) {
            const int dh =
                std::max(roomFor, std::min(56, wrappedHeight(detail, 486)));
            r.detail = makeLabel(hwnd_, detail, 50, rowY_ + 22, 486, dh, -1, true);
            h += dh;
        }
        rowY_ += h + 8;
        rows_.push_back(r);
        return (int)rows_.size() - 1;
    }

    void setRow(int i, Check state, const std::string& head,
                const std::string& detail) {
        if (i < 0 || i >= (int)rows_.size()) return;
        rows_[(size_t)i].state = state;
        setText(rows_[(size_t)i].head, checkMark(state) + head);
        if (rows_[(size_t)i].detail) setText(rows_[(size_t)i].detail, detail);
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
                       "the computer and try again, or choose On my screen "
                       "instead.");
            else
                addRow(Check::Ok, "Dental Macros can read your keypad", "");

            if (!ctx.offSwitch.empty())
                addRow(Check::Ok, "Emergency off switch: " + ctx.offSwitch,
                       "Press it anywhere to stop Dental Macros listening.");
            else
                addRow(Check::Note, "No emergency off switch",
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
                                "We'll recognise it automatically.", 40);
            Row& r = rows_[(size_t)keypadRow_];
            r.button = makeButton(hwnd_, "Use on-screen buttons instead", 24,
                                  rowY_, 250, 30, kWizUseScreen);
            rowY_ += 38;
            // Back here from the last page: the key chosen before still counts.
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

    void updateNext() {
        if (!btnNext_) return;
        EnableWindow(btnNext_, page_ != 1 || readyToGoOn());
        if (page_ == 1 && checkSub_) {
            bool problem = false;
            for (const Row& r : rows_)
                if (r.state == Check::Problem) problem = true;
            setText(checkSub_,
                    problem ? "Something needs your attention before going on."
                    : !onScreen_ && result.keyName.empty()
                        ? "Almost done - press a key on your keypad."
                        : "All set.");
        }
    }

    // ---- page 3
    void showButtonPage() {
        for (HWND h : {typeLabel_, text_})
            ShowWindow(h, clicksRoute_ ? SW_HIDE : SW_SHOW);
        for (HWND h : {clicksLabel_, clicksHint_, btnRecord_, clicksStatus_})
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
                             : "\nIf it ever stops typing when you don't want "
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
                checkMark(Check::Ok) + std::to_string(clicks_.size()) +
                    (clicks_.size() == 1 ? " click" : " clicks") + " recorded" +
                    (t.has ? " in " + t.displayName() : "") + ".");
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

    HWND btnScreen_ = nullptr, btnKeypad_ = nullptr, btnType_ = nullptr,
         btnClicks_ = nullptr, btnBack_ = nullptr, btnNext_ = nullptr;
    HWND checkSub_ = nullptr;
    HWND typeLabel_ = nullptr, text_ = nullptr, clicksLabel_ = nullptr,
         clicksHint_ = nullptr, btnRecord_ = nullptr, clicksStatus_ = nullptr,
         name_ = nullptr, login_ = nullptr, float_ = nullptr, tip_ = nullptr;
    bool nameEdited_ = false, settingName_ = false;

    std::vector<Row> rows_;
    int rowY_ = 84;
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

bool chooseColorHex(HWND parent, std::string& hex) {
    static COLORREF custom[16] = {0};
    Gdiplus::Color start = fromHex(hex, theme().tile);
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = parent;
    cc.lpCustColors = custom;
    cc.rgbResult = RGB(start.GetR(), start.GetG(), start.GetB());
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return false;
    Gdiplus::Color picked(255, GetRValue(cc.rgbResult), GetGValue(cc.rgbResult),
                          GetBValue(cc.rgbResult));
    hex = toHex(picked);
    return true;
}

}  // namespace ui
