#include "dialogs.h"

#include "device.h"
#include "keys.h"
#include "theme.h"
#include "ui_common.h"
#include "util.h"
#include "vision.h"
#include "winman.h"

#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
        status_ = makeLabel(hwnd_, "Waiting for a keypress...", 20, 66, 420, 22,
                            -1, true);

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
            listener_.start([this](const dev::KeyEvent& ev) {
                if (!ev.down) return;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    pending_.push_back(ev);
                }
                PostMessageW(hwnd_, WM_KEY_ARRIVED, 0, 0);
            });
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

            if (!restrictMatch.empty() &&
                !cfg::specMatches(restrictMatch, ev.devicePath)) {
                setText(status_,
                        "That key came from a different keyboard - press a key "
                        "on the macro keyboard.");
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
    dev::RawKeyboardListener listener_;
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

// ----------------------------------------------------------- click capture
namespace {

constexpr UINT WM_CLICK_ARRIVED = WM_APP + 22;

// Only one of these is ever open at a time (it's modal), so a single static
// slot is enough to route the low-level hook callback - Windows hook procs
// have to be free functions and carry no instance pointer of their own.
class ClickCaptureDialog;
ClickCaptureDialog* g_activeClickCapture = nullptr;

class ClickCaptureDialog : public Modal {
public:
    std::string prompt;
    bool ok = false;
    POINT pt{0, 0};
    HWND wnd = nullptr;   // top-level window the click landed in

    ~ClickCaptureDialog() override { uninstallHook(); }

    void build() override {
        makeLabel(hwnd_, prompt, 20, 20, 400, 64, -1, false, true);
        status_ = makeLabel(hwnd_, "Waiting for a click...", 20, 92, 400, 22,
                            -1, true);
        layoutFooter(ID_CANCEL, "Cancel", ID_CANCEL, "");

        g_activeClickCapture = this;
        hook_ = SetWindowsHookExW(WH_MOUSE_LL, &ClickCaptureDialog::mouseProc,
                                  GetModuleHandleW(nullptr), 0);
        if (!hook_) {
            g_activeClickCapture = nullptr;
            setText(status_, "Could not listen for mouse clicks.");
        }
    }

    void onCancel() override { uninstallHook(); }

    bool onMessage(UINT msg, WPARAM, LPARAM, LRESULT& out) override {
        if (msg != WM_CLICK_ARRIVED) return false;
        ok = true;
        pt = pending_;
        wnd = pendingWnd_;
        uninstallHook();
        close(true);
        out = 0;
        return true;
    }

private:
    void uninstallHook() {
        if (hook_) {
            UnhookWindowsHookEx(hook_);
            hook_ = nullptr;
        }
        if (g_activeClickCapture == this) g_activeClickCapture = nullptr;
    }

    static LRESULT CALLBACK mouseProc(int code, WPARAM wp, LPARAM lp) {
        if (code == HC_ACTION && wp == WM_LBUTTONDOWN && g_activeClickCapture) {
            ClickCaptureDialog* self = g_activeClickCapture;
            g_activeClickCapture = nullptr;   // one-shot: ignore further clicks
            self->pending_ = ((MSLLHOOKSTRUCT*)lp)->pt;
            // Grab the window handle here, at the instant of the click. By the
            // time the posted message is handled a menu may have closed or the
            // z-order changed, and WindowFromPoint would answer about a
            // different window. Anything slower than this - the process name,
            // the rectangle - is resolved off the hook.
            self->pendingWnd_ = WindowFromPoint(self->pending_);
            PostMessageW(self->hwnd_, WM_CLICK_ARRIVED, 0, 0);
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    HWND status_ = nullptr;
    HHOOK hook_ = nullptr;
    POINT pending_{0, 0};
    HWND pendingWnd_ = nullptr;
};

}  // namespace

bool captureClick(HWND parent, const std::string& prompt, ClickCapture& out) {
    ClickCaptureDialog d;
    d.prompt = prompt;
    d.run(parent, "Recording a click", 440, 160);
    if (!d.ok) return false;

    out = ClickCapture{};
    out.x = d.pt.x;
    out.y = d.pt.y;

    // The hook caught the child control under the pointer; the useful thing
    // is the top-level window that owns it.
    HWND top = d.wnd ? GetAncestor(d.wnd, GA_ROOT) : nullptr;
    win::WindowInfo info;
    if (top && win::describeWindow(top, info) && !info.cls.empty()) {
        // MacroDeck's own windows are hidden while recording, but never
        // record ourselves as the target even so.
        DWORD pid = 0;
        GetWindowThreadProcessId(top, &pid);
        if (pid != GetCurrentProcessId()) {
            out.hasWindow = true;
            out.cls = info.cls;
            out.exe = info.exe;
            out.title = info.title;
            out.winX = info.x;
            out.winY = info.y;
            out.winW = info.w;
            out.winH = info.h;
            out.maximized = info.maximized;
        }
    }
    return true;
}

// ----------------------------------------------------------- device picker
namespace {

class AddKeyboardDialog : public Modal {
public:
    cfg::Config* config = nullptr;
    std::string newAlias;

    ~AddKeyboardDialog() override { listener_.stop(); }

    void build() override {
        makeLabel(hwnd_, "Choose the keyboard you want to use for macros:", 20,
                  16, 480, 22, -1, false, true);
        list_ = makeList(hwnd_, 20, 44, 480, 150, ID_FIRST_USER);
        makeButton(hwnd_, "Rescan", 20, 202, 96, 30, ID_FIRST_USER + 1);
        identifyBtn_ = makeButton(hwnd_, "Identify by pressing a key", 124, 202,
                                  200, 30, ID_FIRST_USER + 2);
        status_ = makeLabel(hwnd_, "", 20, 240, 480, 22, -1, true);

        warn_ = makeLabel(
            hwnd_,
            "Heads up: keys on the keyboard you pick will stop typing into "
            "your apps while MacroDeck is listening - they only fire macros. "
            "Pick a spare keyboard or numpad, not the one you type on.",
            20, 266, 480, 56, -1, true);

        layoutFooter(ID_OK, "Add keyboard", ID_CANCEL, "Cancel");
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
                        "Pick a keyboard from the list first, or press Rescan "
                        "if the list is empty.");
            return false;
        }
        const dev::KeyboardDevice& d = devices_[(size_t)sel];

        for (const auto& kb : config->keyboards) {
            if (cfg::specMatches(kb.match, d.path)) {
                if (!askYesNo(hwnd_, "Already added",
                              "This keyboard is already set up as \"" +
                                  kb.alias +
                                  "\".\n\nAdd it a second time anyway?"))
                    return false;
                break;
            }
        }

        bool stable = true;
        std::string match = cfg::chooseMatch(d, devices_, &stable);

        std::string suggested =
            config->uniqueAlias(d.name.empty() ? "Macro pad" : d.name);
        if (!askText(hwnd_, "Name this keyboard",
                     "What do you want to call it?", suggested))
            return false;

        cfg::Keyboard kb;
        kb.alias = config->uniqueAlias(suggested);
        kb.match = match;
        kb.block = config->settings.blockByDefault;
        kb.deck = "auto";
        config->keyboards.push_back(kb);
        config->dirty = true;
        newAlias = kb.alias;

        if (!stable)
            showMessage(hwnd_, "Keyboard added",
                        "Two identical keyboards are attached, so MacroDeck "
                        "had to identify this one by which USB port it is "
                        "plugged into.\n\nKeep it in the same port, or it will "
                        "stop being recognised.");
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
            for (size_t i = 0; i < devices_.size(); ++i) {
                if (devices_[i].path == ev.devicePath) {
                    SendMessageW(list_, LB_SETCURSEL, (WPARAM)i, 0);
                    setText(status_, "That key came from: " +
                                         devices_[i].display());
                    stopIdentify();
                    out = 0;
                    return true;
                }
            }
        }
        out = 0;
        return true;
    }

private:
    void rescan() {
        devices_ = dev::listKeyboards();
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        for (const auto& d : devices_)
            SendMessageW(list_, LB_ADDSTRING, 0,
                         (LPARAM)widen(d.display()).c_str());
        if (!devices_.empty()) SendMessageW(list_, LB_SETCURSEL, 0, 0);
        setText(status_, devices_.empty()
                             ? "No keyboards detected."
                             : std::to_string(devices_.size()) +
                                   " keyboard device(s) found.");
    }

    void identify() {
        if (identifying_) {
            stopIdentify();
            return;
        }
        try {
            listener_.start([this](const dev::KeyEvent& ev) {
                if (!ev.down) return;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    pending_.push_back(ev);
                }
                PostMessageW(hwnd_, WM_KEY_ARRIVED, 0, 0);
            });
            identifying_ = true;
            setText(identifyBtn_, "Stop listening");
            setText(status_, "Press a key on the keyboard you want to use...");
        } catch (const std::exception& e) {
            setText(status_, std::string("Could not listen for keys: ") +
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
    std::vector<dev::KeyboardDevice> devices_;
    dev::RawKeyboardListener listener_;
    bool identifying_ = false;
    std::mutex mu_;
    std::vector<dev::KeyEvent> pending_;
};

}  // namespace

bool addKeyboardDialog(HWND parent, cfg::Config& config, std::string& newAlias) {
    AddKeyboardDialog d;
    d.config = &config;
    if (!d.run(parent, "Add a macro keyboard", 540, 390)) return false;
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
        // per-field helper buttons live at ID_FIRST_USER + 100 + index*2
        if (notify == BN_CLICKED && id >= ID_FIRST_USER + 100) {
            int index = (id - (ID_FIRST_USER + 100)) / 2;
            if (index >= 0 && index < (int)fields_.size())
                helperClicked(fields_[(size_t)index]);
        }
    }

    bool onOk() override { return collect(true); }

private:
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
            wc.lpszClassName = L"MacroDeckSnip";
            wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
            RegisterClassExW(&wc);
            registered = true;
        }

        hwnd_ = CreateWindowExW(WS_EX_TOPMOST, L"MacroDeckSnip", L"Snip",
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
constexpr int kSettingsH = 430;
constexpr int kSettingsFooterH = 58;
constexpr int kHotkeyRecordId = ID_FIRST_USER + 10;

class SettingsDialog : public Modal {
public:
    cfg::Settings settings;

    void build() override {
        pane_ = makeScrollPane(hwnd_, 0, 0, kSettingsW,
                               kSettingsH - kSettingsFooterH,
                               ID_FIRST_USER + 9);

        int y = 16;
        makeLabel(pane_, "Startup", 20, y, 300, 20, -1, false, true);
        y += 26;
        startEngine_ = makeCheck(pane_, "Start listening as soon as MacroDeck opens",
                                 20, y, 420, 24, ID_FIRST_USER,
                                 settings.startEngineOnLaunch);
        y += 28;
        startMin_ = makeCheck(pane_, "Start minimised to the notification area",
                              20, y, 420, 24, ID_FIRST_USER + 1,
                              settings.startMinimized);
        y += 28;
        toTray_ = makeCheck(pane_, "Closing the window keeps MacroDeck running in the tray",
                            20, y, 420, 24, ID_FIRST_USER + 2,
                            settings.minimizeToTray);
        y += 28;
        runAtLogin_ = makeCheck(pane_, "Run MacroDeck when I sign in to Windows",
                                20, y, 420, 24, ID_FIRST_USER + 5,
                                runAtLoginEnabled());
        y += 40;

        makeLabel(pane_, "Start / stop listening shortcut", 20, y, 300, 20, -1,
                  false, true);
        y += 26;
        hotkey_ = makeEdit(pane_, settings.toggleHotkey, 20, y, 320, 26,
                           ID_FIRST_USER + 6);
        makeButton(pane_, "Record", 348, y, 92, 26, kHotkeyRecordId);
        y += 30;
        makeLabel(pane_,
                  "Works anywhere in Windows, so a macro pad that has stopped "
                  "typing can always be handed back. Needs a modifier. Leave "
                  "blank for none.",
                  20, y, 420, 36, -1, true);
        y += 48;

        makeLabel(pane_, "Macro keyboards", 20, y, 300, 20, -1, false, true);
        y += 26;
        blockDefault_ = makeCheck(
            pane_, "New keyboards block their keys from apps by default", 20, y,
            420, 24, ID_FIRST_USER + 3, settings.blockByDefault);
        y += 40;

        makeLabel(pane_, "Image matching", 20, y, 300, 20, -1, false, true);
        y += 26;
        makeLabel(pane_, "Default match confidence (0.5 - 1.0)", 20, y, 300, 20,
                  -1, true);
        y += 22;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", settings.matchConfidence);
        confidence_ = makeEdit(pane_, buf, 20, y, 90, 26, ID_FIRST_USER + 4);
        y += 30;
        makeLabel(pane_,
                  "Lower is more forgiving but risks false matches. 0.85 suits "
                  "most buttons.",
                  20, y, 420, 36, -1, true);
        y += 44;

        setScrollPaneContent(pane_, y);
        layoutFooter(ID_OK, "Save", ID_CANCEL, "Cancel");
    }

    void onCommand(int id, int notify) override {
        if (id == kHotkeyRecordId && notify == BN_CLICKED) {
            KeyCaptureResult r = captureKey(
                hwnd_, "Record the start/stop shortcut",
                "Hold Ctrl, Alt, Shift or Win and press the final key.", "",
                true, false);
            if (r.ok) setText(hotkey_, r.combo);
        }
    }

    bool onOk() override {
        settings.startEngineOnLaunch = isChecked(startEngine_);
        settings.startMinimized = isChecked(startMin_);
        settings.minimizeToTray = isChecked(toTray_);
        settings.blockByDefault = isChecked(blockDefault_);

        const std::string combo = trim(getText(hotkey_));
        if (!combo.empty()) {
            unsigned mods = 0;
            int vk = 0;
            std::string err;
            if (!keys::parseHotkey(combo, mods, vk, &err)) {
                showMessage(hwnd_, "Check that shortcut",
                            "\"" + combo + "\" won't work: " + err + ".");
                SetFocus(hotkey_);
                return false;
            }
        }
        settings.toggleHotkey = combo;

        double c = atof(getText(confidence_).c_str());
        if (c < cfg::kMinConfidence || c > cfg::kMaxConfidence) {
            showMessage(hwnd_, "Check that number",
                        "Match confidence should be between 0.5 and 1.0.");
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
                showMessage(hwnd_, "Could not change startup",
                            "MacroDeck could not update the Windows startup "
                            "entry:\n\n" + err,
                            true);
        }
        return true;
    }

private:
    HWND pane_ = nullptr;
    HWND startEngine_ = nullptr, startMin_ = nullptr, toTray_ = nullptr,
         blockDefault_ = nullptr, confidence_ = nullptr, runAtLogin_ = nullptr,
         hotkey_ = nullptr;
};

}  // namespace

bool settingsDialog(HWND parent, cfg::Settings& settings) {
    SettingsDialog d;
    d.settings = settings;
    if (!d.run(parent, "MacroDeck settings", kSettingsW, kSettingsH))
        return false;
    settings = d.settings;
    return true;
}

// ------------------------------------------------------------ file pickers
bool chooseImageFile(HWND parent, std::string& relPath) {
    std::string imagesDir = appPath("images");
    ensureDir(imagesDir);

    wchar_t file[MAX_PATH * 2] = {0};
    if (!relPath.empty()) {
        std::wstring cur = widen(joinPath(exeDir(), relPath));
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
