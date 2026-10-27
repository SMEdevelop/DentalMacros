// The document model: keyboards, deck layouts, bindings and their steps,
// plus the action schema that drives both the editor UI and the engine.
#pragma once
#include "json.h"

#include <climits>
#include <string>
#include <vector>

namespace dev { struct InputDevice; }

namespace cfg {

// ------------------------------------------------------------ action schema
enum class FieldKind {
    Text,        // single-line string
    Multiline,   // string, multi-line editor
    Int,
    Float,
    Bool,
    Choice,      // one of `choices` (comma separated)
    File,        // path with a Browse button
    KeyCombo,    // string with a "record combo" helper
    Layout,      // name of a saved window layout
};

struct FieldDef {
    const char* key;
    const char* label;
    FieldKind kind;
    const char* def;        // default as text ("" = empty)
    bool required;
    const char* choices;    // for Choice
    const char* hint;       // shown under the field
};

struct ActionDef {
    const char* id;
    const char* label;
    const char* category;
    const char* blurb;
    std::vector<FieldDef> fields;
};

const std::vector<ActionDef>& actionDefs();
const ActionDef* findAction(const std::string& id);
const std::vector<std::string>& actionCategories();

// ------------------------------------------------------------------- model
// Universal per-step option, stored alongside the action's own parameters.
constexpr const char* kContinueOnError = "continue_on_error";

struct Step {
    std::string action;
    js::Value params = js::Value::object();

    std::string summary() const;   // one-line description for lists
    // True if a failure here should be logged and stepped over rather than
    // ending the macro.
    bool continueOnError() const;
};

struct Binding {
    std::string key;      // canonical key name, e.g. "num1"
    std::string title;    // label shown on the tile
    std::string icon;     // optional image path, relative to the exe
    std::string color;    // "#rrggbb", empty = theme default
    int slot = -1;        // position in a fixed deck preset; -1 = auto flow
    std::vector<Step> steps;

    // Scrolling the wheel over this tile, on the deck or on the palette.
    // A dial is one control with three gestures - press, up, down - rather
    // than three controls, so they live on the binding instead of becoming
    // bindings of their own.
    std::vector<Step> scrollUp;
    std::vector<Step> scrollDown;

    bool isDial() const { return !scrollUp.empty() || !scrollDown.empty(); }

    std::string displayTitle() const;   // title, or a sensible fallback
};

// When a page should put itself in front. The three fields are the ones
// win::WindowInfo reports, matched the same way win::findWindow matches them:
// exact on the executable and the window class, case-insensitive substring on
// the title. Any field left empty is not tested, so a rule of nothing at all
// matches nothing - that is what makes a page the default rather than one
// that swallows every application.
struct AppRule {
    std::string exe;      // image name only, e.g. "photoshop.exe"
    std::string cls;      // window class
    std::string title;    // substring of the title

    bool empty() const;
    bool matches(const std::string& winExe, const std::string& winCls,
                 const std::string& winTitle) const;
    // How specific this rule is, so that the tightest matching page wins when
    // several could claim the same window.
    int specificity() const;
    std::string summary() const;   // one line for the page strip and menus
};

// One layer of the deck: a full set of bindings for the same keys. Six of
// these is what turns thirteen keys on a macro pad into seventy-eight.
struct Page {
    std::string name;
    std::string color;    // "#rrggbb", empty = theme default
    AppRule rule;         // empty = this page is not tied to an application
    std::vector<Binding> bindings;

    Binding* findKey(const std::string& key);
    const Binding* findKey(const std::string& key) const;
    Binding* findSlot(int slot);

    std::string displayName() const;   // name, or a sensible fallback
};

struct Keyboard {
    std::string alias;
    std::string match;    // substring of the Raw Input device path
    bool block = true;
    // Only consulted when block is true.
    //
    //   true  - every key on this keyboard is taken away from other programs.
    //           Right for a spare pad dedicated to macros.
    //   false - only the keys that actually run a macro are taken; everything
    //           else types normally. Right when the macro keys are F13-F24 or
    //           extra keys on a keyboard you also type on.
    //
    // Defaults to true so that configs written before this option existed keep
    // behaving exactly as they did.
    bool blockAllKeys = true;

    // A pointing device rather than a keyboard: a spare mouse, or a
    // trackpad. Everything else here treats it identically - its buttons and
    // wheel notches have virtual keys of their own (see device.h), so they
    // are "keys" like any other. Only the wording in the UI, the deck it
    // defaults to, and what may be blocked differ.
    bool isMouse = false;

    // A deck with no keyboard behind it: buttons that exist only on screen,
    // fired by clicking them on the deck or the always-on-top palette. It
    // matches no device, takes no key from anybody, and - the point - never
    // causes the low-level hook to be installed, so it cannot cost a
    // keystroke anywhere in the machine.
    bool softwareOnly = false;

    std::string deck = "auto";

    // Never empty: load(), and every operation that can remove one, keep at
    // least a single page, so active() always has something to return and no
    // caller has to check.
    std::vector<Page> pages{Page{}};
    // Which page is in front. Not saved: a deck should come back on the page
    // its rules ask for, not the one that happened to be up when the app was
    // last closed.
    int activePage = 0;

    Page& active();
    const Page& active() const;
    // Clamps to a real page and guarantees at least one exists. Called by
    // load() and by anything that edits the page list.
    void normalizePages();

    // The page shown when no application rule matches - the first page
    // without a rule, or the first page if they all have one.
    int defaultPage() const;
    // The page that claims this window, or defaultPage() if none does. The
    // most specific rule wins, so a page tied to one document in an app beats
    // a page tied to the whole app.
    int pageForApp(const std::string& winExe, const std::string& winCls,
                   const std::string& winTitle) const;
    int pageByName(const std::string& name) const;   // -1 if there is no such page
    std::string uniquePageName(const std::string& wanted) const;

    // Search the page that is currently in front.
    Binding* findKey(const std::string& key);
    const Binding* findKey(const std::string& key) const;
    Binding* findSlot(int slot);
    // Does this key carry a macro on ANY page? Blocking asks this rather than
    // asking the active page, so that a macro key does not start typing into
    // your document the moment you switch to a page that leaves it empty.
    bool anyPageHasMacro(const std::string& key) const;
};

struct Settings {
    bool blockByDefault = true;
    // What blockAllKeys a newly added keyboard starts with.
    bool blockAllKeysByDefault = true;
    double matchConfidence = 0.85;
    std::string layoutsDir = "layouts";
    bool startEngineOnLaunch = false;
    bool minimizeToTray = true;
    bool startMinimized = false;
    // Whether a deck follows the application in front, switching to the page
    // whose rule matches it. On by default: a page with an application rule
    // has said what it wants, and a rule that never fires is a puzzle rather
    // than a preference. The padlock in the page strip turns it off.
    bool switchPageWithApp = true;

    // System-wide shortcut that starts and stops the engine, so a macro pad
    // left armed can always be silenced without hunting for the window.
    // Empty = no shortcut registered.
    std::string toggleHotkey = "ctrl+alt+shift+m";

    // The small always-on-top palette of buttons. Off until asked for, and
    // remembered where it was left: a launcher that reappears in the middle
    // of the screen every morning is one you stop opening.
    bool miniDeckVisible = false;
    int miniDeckX = INT_MIN;      // INT_MIN = never placed, pick a corner
    int miniDeckY = INT_MIN;
    int miniDeckTile = 38;        // design px per button
    int miniDeckCols = 5;

    // Which main window to show: Simple (type, press keys, recorded clicks,
    // wait) or Advanced (everything). Only what is shown changes - Simple
    // mode keeps every step it doesn't make. A brand-new setup starts in
    // Simple; a settings file from before this option existed loads as
    // Advanced, so nobody upgrading finds half their editor gone.
    bool simpleMode = true;

    // First-run setup has been finished or skipped. False only for a
    // brand-new setup; a settings file from before this option existed was
    // set up long ago.
    bool setupDone = false;
};

// Match confidence outside this range is refused by the settings dialog and
// reset to the default on load, so the file and the UI agree on what is valid.
constexpr double kMinConfidence = 0.5;
constexpr double kMaxConfidence = 1.0;
constexpr double kDefaultConfidence = 0.85;

struct Config {
    Settings settings;
    std::vector<Keyboard> keyboards;
    std::string path;
    bool dirty = false;

    // How many pointing devices are attached, filled in by the app from
    // dev::listMice(). Blocking consults it so that the primary button is
    // never taken away on a machine with a single pointer - see
    // blockedForKey(). Counted when devices change rather than per event,
    // because the hook cannot afford to enumerate anything.
    int pointerCount = 1;

    std::string loadError;   // set when the file existed but couldn't be used
    // Non-fatal complaints about the file's contents (duplicate keys, key
    // names this build doesn't know). The file still loaded.
    std::vector<std::string> loadWarnings;
    // Set when load() failed on a file that is really there. Until the file
    // has been copied aside, save() must not run: overwriting is how a
    // recoverable typo turns into lost work.
    bool needsRescue = false;
    // Where save() put the rescued copy, for reporting back to the user.
    std::string lastBackup;
    // The Windows error behind the last failed save(), 0 after a good one.
    unsigned long lastSaveError = 0;
    // Set by load() when the file was there but Windows would not open it -
    // usually sync or backup software holding it - as opposed to a file that
    // opened but is not valid. Worth retrying; a damaged file is not.
    bool loadLocked = false;

    // Loading never throws: a damaged file leaves an empty config plus a
    // populated loadError, so the app can open and explain itself.
    void load(const std::string& file);
    // Fails (without writing) if a damaged file could not be copied aside
    // first. Any successful save clears needsRescue.
    bool save(std::string* error = nullptr);
    std::string toJson() const;      // exactly what save() writes
    // `win32Error` receives the Windows error code when writing fails.
    bool saveAs(const std::string& file, std::string* error = nullptr,
                unsigned long* win32Error = nullptr) const;
    // Copy the current file to backups/macros-<date>.json, keeping the newest
    // `keep` of them. Cheap enough to call once per run.
    bool writeDailyBackup(int keep = 10, std::string* outPath = nullptr) const;

    Keyboard* keyboard(const std::string& alias);
    std::string uniqueAlias(const std::string& wanted) const;

    // Which configured keyboard (if any) a device path belongs to. `vk`, when
    // given, settles the case of two decks on one device - a Bluetooth keypad
    // added once as a keyboard for its keys and again as a mouse for its
    // dial - by preferring the deck of the same kind as the key. With only one
    // deck on the device, that deck gets everything, as it always has.
    const Keyboard* keyboardForDevice(const std::string& devicePath,
                                      int vk = 0) const;
    // True if this device blocks anything at all. Used to decide whether the
    // low-level hook is worth installing; it does NOT mean every key is taken.
    bool blockedForDevice(const std::string& devicePath) const;
    // Could this virtual key be taken from another program by ANY configured
    // keyboard? The hook asks this first, before it goes to the trouble of
    // working out which keyboard a keystroke came from. Answering no lets an
    // ordinary keystroke straight through, which is the difference between
    // typing normally and typing through a stall on every key.
    bool couldBlockVk(int vk) const;

    // Is any configured mouse blocking anything? Deciding whether the
    // WH_MOUSE_LL hook is worth installing at all: that hook sees every
    // pointer movement in the machine, so a setup with no macro mouse should
    // never pay for one.
    bool anyMouseBlocks() const;

    // True if this exact keystroke should be taken away from other programs.
    // This is the one the hook asks, and the only one that understands
    // blockAllKeys: with it off, only a key carrying a macro is taken.
    bool blockedForKey(const std::string& devicePath, int vk) const;
};

// Can this key carry a macro at all? Left click cannot: it is how you work
// the machine, and a macro on it would fire on every click you make anywhere,
// blocked or not. `why` receives a sentence to show the user.
//
// This is a flat rule rather than the situational one blocking uses, because
// a binding is written into a file and read back on another machine, while
// blocking is decided live. A config that is legal here and catastrophic
// there would be worse than simply never allowing it.
bool keyBindable(const std::string& key, std::string* why = nullptr);

// Does a stored match string select this device path? A match naming one input
// of a Bluetooth keypad by its full path - what older builds stored - selects
// every input of that keypad.
bool specMatches(const std::string& match, const std::string& devicePath);

// Pick the most reboot-proof match string for a device: a Bluetooth keypad's
// address, or a USB device's firmware id. `stable` reports whether that was
// usable (false = fell back to the USB port path because an identical twin
// keyboard is attached).
std::string chooseMatch(const dev::InputDevice& d,
                        const std::vector<dev::InputDevice>& all,
                        bool* stable);

// ------------------------------------------------------------ deck presets
struct DeckSlot {
    int row = 0, col = 0;
    int colSpan = 1, rowSpan = 1;
    std::string key;      // pre-assigned key, empty = ask the user
};

struct DeckPreset {
    std::string id;
    std::string label;
    int cols = 4, rows = 3;
    bool flow = false;        // true = ignore slots, lay out bindings in order
    bool fixedKeys = false;   // true = slots carry known key names
    std::vector<DeckSlot> slots;
};

const std::vector<DeckPreset>& deckPresets();
const DeckPreset* findPreset(const std::string& id);

// Import a macrokb (Python) macros.yaml. Returns false and fills `error` if
// the file can't be understood.
bool importMacroKbYaml(const std::string& yamlPath, Config& out,
                       std::string* error);

}  // namespace cfg
