#pragma once
#include "config.h"
#include "friendly.h"
#include "recorder.h"

#include <string>
#include <windows.h>

namespace ui {

struct KeyCaptureResult {
    bool ok = false;
    std::string keyName;      // canonical name, e.g. "num1"
    std::string combo;        // "ctrl+shift+t" when captured in combo mode
    std::string devicePath;   // device the key came from ("" if picked by name)
    std::string deviceLabel;
};

// Wait for a physical keypress. `restrictMatch` (optional) keeps only keys
// from a specific configured keyboard. `comboMode` records modifiers too.
KeyCaptureResult captureKey(HWND parent, const std::string& title,
                            const std::string& prompt,
                            const std::string& restrictMatch = "",
                            bool comboMode = false, bool allowPickByName = true);

// Scan attached devices and add one to the config. Returns the new alias.
// `mouse` scans pointing devices instead of keyboards; everything else about
// the dialog, and about the deck it produces, is the same.
bool addKeyboardDialog(HWND parent, cfg::Config& config,
                       std::string& newAlias, bool mouse = false);

// The click recorder: a small always-on-top window that stays up for the
// whole recording, lists every click captured so far, and lets the user undo,
// remove or redo any of them before anything is saved. Each recorded spot is
// marked on screen with its number.
//
// A click is recorded with a physical RIGHT click, so that left click keeps
// working normally: the target app can be clicked through to the tab, menu or
// dialog the recorded click needs to land on. Both halves of the right click
// are swallowed, so no context menu opens over the thing being recorded. Two
// right-clicks in quick succession on the same spot record a double-click.
//
// Returns false if the user cancelled or recorded nothing. The caller should
// hide its own windows first; the recorder places itself in a corner.
bool recordClicksPanel(HWND owner, std::vector<rec::Click>& clicks,
                       rec::Speed& speed);

// Build or edit one macro step.
bool editStepDialog(HWND parent, cfg::Step& step, const cfg::Settings& settings);

// Fullscreen snip: drag a box, save it as a PNG template.
// `outRelPath` receives the path relative to the exe folder.
bool snipTemplate(HWND parent, std::string& outRelPath);

// Things in the Settings window that the main window has to carry out,
// because they replace or reload the whole configuration. The settings shown
// are saved first.
enum class SettingsAction {
    None, SaveCopy, LoadCopy, Reload, ImportMacroKb, RunSetup
};

bool settingsDialog(HWND parent, cfg::Settings& settings,
                    SettingsAction* action = nullptr);

// The text of the About box.
std::string aboutText();

// Common file / colour pickers.
// Simple mode's prompts. A few lines of text to type (Enter makes a new
// line), and a pause picked from four buttons.
bool askLongText(HWND parent, const std::string& title,
                 const std::string& prompt, std::string& value,
                 const std::string& hint = "");
bool chooseWait(HWND parent, double& seconds);

// ---- first-run setup
struct SetupContext {
    std::string offSwitch;     // the start/stop shortcut in force, readable;
                               // "" when there is none
    bool runAtLogin = false;   // already starts with Windows
};

struct SetupResult {
    bool onScreen = true;      // on-screen buttons rather than a keypad
    // The keypad, when there is one: its match string, its product name
    // (may be empty) and the key pressed on it, which carries the button.
    std::string match;
    bool matchStable = true;   // false: identified by USB port (see chooseMatch)
    std::string deviceName;
    std::string keyName;
    // The first button.
    std::string title;
    std::vector<cfg::Step> steps;
    bool runAtLogin = true;
    bool floating = false;     // show the floating buttons (on-screen only)
};

// Welcome - checks - first button. False if the user skipped it.
bool runSetupWizard(HWND owner, const SetupContext& ctx, SetupResult& out);

// A problem explained: its title, what to do, and - when there is one - a
// button that fixes it. "Copy details" puts the technical text on the
// clipboard for whoever helps. Returns true if the fix button was pressed.
bool showFriendly(HWND parent, const fe::Friendly& f);

bool chooseImageFile(HWND parent, std::string& relPath);
bool chooseColorHex(HWND parent, std::string& hex);

}  // namespace ui
