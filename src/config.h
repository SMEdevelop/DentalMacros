// The document model: keyboards, deck layouts, bindings and their steps,
// plus the action schema that drives both the editor UI and the engine.
#pragma once
#include "json.h"

#include <string>
#include <vector>

namespace dev { struct KeyboardDevice; }

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

    std::string displayTitle() const;   // title, or a sensible fallback
};

struct Keyboard {
    std::string alias;
    std::string match;    // substring of the Raw Input device path
    bool block = true;
    std::string deck = "auto";
    std::vector<Binding> bindings;

    Binding* findKey(const std::string& key);
    const Binding* findKey(const std::string& key) const;
    Binding* findSlot(int slot);
};

struct Settings {
    bool blockByDefault = true;
    double matchConfidence = 0.85;
    std::string layoutsDir = "layouts";
    bool startEngineOnLaunch = false;
    bool minimizeToTray = true;
    bool startMinimized = false;
    // System-wide shortcut that starts and stops the engine, so a macro pad
    // left armed can always be silenced without hunting for the window.
    // Empty = no shortcut registered.
    std::string toggleHotkey = "ctrl+alt+shift+m";
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

    // Loading never throws: a damaged file leaves an empty config plus a
    // populated loadError, so the app can open and explain itself.
    void load(const std::string& file);
    // Fails (without writing) if a damaged file could not be copied aside
    // first. Any successful save clears needsRescue.
    bool save(std::string* error = nullptr);
    std::string toJson() const;      // exactly what save() writes
    bool saveAs(const std::string& file, std::string* error = nullptr) const;
    // Copy the current file to backups/macros-<date>.json, keeping the newest
    // `keep` of them. Cheap enough to call once per run.
    bool writeDailyBackup(int keep = 10, std::string* outPath = nullptr) const;

    Keyboard* keyboard(const std::string& alias);
    std::string uniqueAlias(const std::string& wanted) const;

    // Which configured keyboard (if any) a device path belongs to.
    const Keyboard* keyboardForDevice(const std::string& devicePath) const;
    bool blockedForDevice(const std::string& devicePath) const;
};

// Does a stored match string select this device path?
bool specMatches(const std::string& match, const std::string& devicePath);

// Pick the most reboot-proof match string for a device. `stable` reports
// whether the firmware id was usable (false = fell back to the USB port path
// because an identical twin keyboard is attached).
std::string chooseMatch(const dev::KeyboardDevice& d,
                        const std::vector<dev::KeyboardDevice>& all,
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
