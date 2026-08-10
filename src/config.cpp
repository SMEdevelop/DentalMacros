#include "config.h"

#include "device.h"
#include "keys.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <regex>

namespace cfg {

// ------------------------------------------------------------ action schema
const std::vector<ActionDef>& actionDefs() {
    static const std::vector<ActionDef> defs = {
        {"type", "Type text", "Text & keys",
         "Types text into whatever window has focus.",
         {{"text", "Text", FieldKind::Multiline, "", true, "",
           "Use \\n in the box by pressing Enter for a new line."},
          {"interval_ms", "Delay between characters (ms)", FieldKind::Int, "",
           false, "", "Leave blank to type instantly."}}},

        {"keys", "Press a hotkey", "Text & keys",
         "Sends a key combination, like Ctrl+Shift+T.",
         {{"combo", "Key combo", FieldKind::KeyCombo, "", true, "",
           "For example: ctrl+shift+t"},
          {"hold_ms", "Hold for (ms)", FieldKind::Int, "", false, "",
           "Leave blank for a normal tap."}}},

        {"click", "Mouse click", "Mouse",
         "Clicks at a screen position, or wherever the pointer already is.",
         {{"x", "X", FieldKind::Int, "", false, "",
           "Leave X and Y blank to click at the current pointer position."},
          {"y", "Y", FieldKind::Int, "", false, "", ""},
          {"button", "Button", FieldKind::Choice, "left", false,
           "left,right,middle", ""},
          {"double", "Double-click", FieldKind::Bool, "", false, "", ""}}},

        {"move_mouse", "Move the pointer", "Mouse",
         "Moves the mouse pointer to a screen position.",
         {{"x", "X", FieldKind::Int, "", true, "", ""},
          {"y", "Y", FieldKind::Int, "", true, "", ""}}},

        {"scroll", "Scroll the wheel", "Mouse",
         "Scrolls the wheel by a number of notches.",
         {{"amount", "Notches", FieldKind::Int, "3", true, "",
           "Positive scrolls up, negative scrolls down."}}},

        {"click_image", "Find an image and click it", "Screen",
         "Looks for a picture on screen and clicks it when found.",
         {{"image", "Template image", FieldKind::File, "", true, "",
           "Capture one with the Snip tool."},
          {"timeout", "Give up after (seconds)", FieldKind::Float, "5", false,
           "", ""},
          {"confidence", "Match confidence (0-1)", FieldKind::Float, "", false,
           "", "Blank uses the app default. Lower is more forgiving."},
          {"offset_x", "Click offset X", FieldKind::Int, "", false, "",
           "Click this far from the centre of the match."},
          {"offset_y", "Click offset Y", FieldKind::Int, "", false, "", ""},
          {"button", "Button", FieldKind::Choice, "left", false,
           "left,right,middle", ""},
          {"double", "Double-click", FieldKind::Bool, "", false, "", ""},
          {"required", "Stop the macro if not found", FieldKind::Bool, "true",
           false, "", ""}}},

        {"wait_image", "Wait for an image", "Screen",
         "Pauses until a picture appears on screen.",
         {{"image", "Template image", FieldKind::File, "", true, "", ""},
          {"timeout", "Give up after (seconds)", FieldKind::Float, "10", false,
           "", ""},
          {"confidence", "Match confidence (0-1)", FieldKind::Float, "", false,
           "", ""},
          {"required", "Stop the macro if not found", FieldKind::Bool, "true",
           false, "", ""}}},

        {"run", "Run a program", "Programs",
         "Launches a program or shell command.",
         {{"cmd", "Program or command", FieldKind::Text, "", true, "",
           "For example: notepad.exe"},
          {"wait", "Wait for it to finish", FieldKind::Bool, "", false, "",
           ""}}},

        {"open", "Open a file or link", "Programs",
         "Opens a file, folder or URL with its default app.",
         {{"target", "File, folder or URL", FieldKind::Text, "", true, "",
           "For example: https://example.com"}}},

        {"sleep", "Wait", "Timing", "Pauses before the next step.",
         {{"seconds", "Seconds", FieldKind::Float, "1", true, "", ""}}},

        {"window_focus", "Focus a window", "Windows",
         "Brings a window to the front.",
         {{"title", "Title contains", FieldKind::Text, "", false, "", ""},
          {"exe", "Program", FieldKind::Text, "", false, "",
           "For example: chrome.exe. Fill in either field, or both."}}},

        {"window_move", "Move or resize a window", "Windows",
         "Repositions a window.",
         {{"title", "Title contains", FieldKind::Text, "", false, "", ""},
          {"exe", "Program", FieldKind::Text, "", false, "", ""},
          {"x", "X", FieldKind::Int, "", false, "",
           "Blank fields keep the window's current value."},
          {"y", "Y", FieldKind::Int, "", false, "", ""},
          {"w", "Width", FieldKind::Int, "", false, "", ""},
          {"h", "Height", FieldKind::Int, "", false, "", ""}}},

        {"window_minimize", "Minimise a window", "Windows", "",
         {{"title", "Title contains", FieldKind::Text, "", false, "", ""},
          {"exe", "Program", FieldKind::Text, "", false, "", ""}}},

        {"window_maximize", "Maximise a window", "Windows", "",
         {{"title", "Title contains", FieldKind::Text, "", false, "", ""},
          {"exe", "Program", FieldKind::Text, "", false, "", ""}}},

        {"window_save", "Save the window layout", "Windows",
         "Remembers where every window is right now.",
         {{"layout", "Layout name", FieldKind::Layout, "main", true, "", ""}}},

        {"window_restore", "Restore a window layout", "Windows",
         "Puts every window back where a saved layout had it.",
         {{"layout", "Layout name", FieldKind::Layout, "main", true, "", ""}}},
    };
    return defs;
}

const ActionDef* findAction(const std::string& id) {
    for (const auto& a : actionDefs())
        if (id == a.id) return &a;
    return nullptr;
}

const std::vector<std::string>& actionCategories() {
    static std::vector<std::string> cats = [] {
        std::vector<std::string> out;
        for (const auto& a : actionDefs()) {
            std::string c = a.category;
            if (std::find(out.begin(), out.end(), c) == out.end())
                out.push_back(c);
        }
        return out;
    }();
    return cats;
}

// ------------------------------------------------------------------ Step
namespace {

std::string valueToText(const js::Value& v) {
    switch (v.type) {
        case js::Type::Str: return v.s;
        case js::Type::Bool: return v.b ? "yes" : "no";
        case js::Type::Num: {
            if (v.n == std::floor(v.n) && std::fabs(v.n) < 1e15)
                return std::to_string((long long)v.n);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", v.n);
            return buf;
        }
        default: return "";
    }
}

std::string ellipsis(const std::string& s, size_t max) {
    std::string one = replaceAll(replaceAll(s, "\r", ""), "\n", " ");
    if (one.size() <= max) return one;
    return one.substr(0, max - 1) + "\xE2\x80\xA6";  // ...
}

std::string summaryRaw(const Step& step) {
    const std::string& action = step.action;
    const js::Value& params = step.params;
    const ActionDef* def = findAction(action);
    std::string label = def ? def->label : action;

    // A short, human phrase per action reads far better in the step list
    // than a dump of every parameter.
    auto p = [&](const char* k) { return params.str(k); };
    if (action == "type") return "Type \"" + ellipsis(p("text"), 34) + "\"";
    if (action == "keys") return "Press " + p("combo");
    if (action == "run") return "Run " + ellipsis(p("cmd"), 40);
    if (action == "open") return "Open " + ellipsis(p("target"), 40);
    if (action == "sleep") {
        const js::Value* v = params.find("seconds");
        return "Wait " + (v ? valueToText(*v) : std::string("1")) + "s";
    }
    if (action == "scroll") {
        int n = params.integer("amount");
        return n >= 0 ? "Scroll up " + std::to_string(n)
                      : "Scroll down " + std::to_string(-n);
    }
    if (action == "click_image") return "Click image " + baseName(p("image"));
    if (action == "wait_image") return "Wait for image " + baseName(p("image"));
    if (action == "window_save") return "Save layout \"" + p("layout") + "\"";
    if (action == "window_restore") return "Restore layout \"" + p("layout") + "\"";
    if (action.rfind("window_", 0) == 0) {
        std::string who = p("title");
        if (who.empty()) who = p("exe");
        if (who.empty()) who = "window";
        return label + ": " + ellipsis(who, 30);
    }
    if (action == "click" || action == "move_mouse") {
        if (params.has("x") && params.has("y"))
            return label + " at " + std::to_string(params.integer("x")) + ", " +
                   std::to_string(params.integer("y"));
        return label + " at pointer";
    }
    return label;
}

}  // namespace

std::string Step::summary() const {
    // Hard cap. Several branches interpolate config text straight through, and
    // macros.json is meant to be hand-editable, so nothing downstream may
    // assume a summary fits in some fixed buffer.
    return ellipsis(summaryRaw(*this), 96);
}

bool Step::continueOnError() const {
    const js::Value* v = params.find(kContinueOnError);
    if (!v) return false;
    if (v->isBool()) return v->b;
    if (v->isNum()) return v->n != 0;
    if (v->isStr()) {
        std::string t = lower(trim(v->s));
        return t == "true" || t == "yes" || t == "1";
    }
    return false;
}

std::string Binding::displayTitle() const {
    if (!title.empty()) return title;
    if (!steps.empty()) return steps[0].summary();
    return "Empty";
}

// -------------------------------------------------------------- Keyboard
Binding* Keyboard::findKey(const std::string& key) {
    for (auto& b : bindings)
        if (b.key == key) return &b;
    return nullptr;
}

const Binding* Keyboard::findKey(const std::string& key) const {
    for (const auto& b : bindings)
        if (b.key == key) return &b;
    return nullptr;
}

Binding* Keyboard::findSlot(int slot) {
    if (slot < 0) return nullptr;
    for (auto& b : bindings)
        if (b.slot == slot) return &b;
    return nullptr;
}

// ----------------------------------------------------------------- Config
namespace {

Step stepFromJson(const js::Value& v) {
    Step s;
    s.action = v.str("do");
    s.params = js::Value::object();
    if (v.isObj())
        for (const auto& kv : v.obj)
            if (kv.first != "do") s.params[kv.first] = kv.second;
    return s;
}

js::Value stepToJson(const Step& s) {
    js::Value v = js::Value::object();
    v["do"] = s.action;
    if (s.params.isObj())
        for (const auto& kv : s.params.obj) v[kv.first] = kv.second;
    return v;
}

}  // namespace

void Config::load(const std::string& file) {
    path = file;
    settings = Settings();
    keyboards.clear();
    loadError.clear();
    loadWarnings.clear();
    needsRescue = false;
    lastBackup.clear();
    dirty = false;

    std::string text;
    switch (readFileEx(file, text)) {
        case ReadResult::Missing:
            return;   // brand new install - defaults are fine
        case ReadResult::Unreadable:
            // Emphatically not the same as "no config yet". Something is
            // there and we could not read it, so nothing may overwrite it.
            loadError =
                "the file is there but Windows would not open it - it may be "
                "open in another program, or locked by sync or backup software";
            needsRescue = true;
            return;
        case ReadResult::Ok:
            break;
    }

    std::string err;
    js::Value root = js::Value::parse(text, &err);
    if (!root.isObj()) {
        loadError = err.empty() ? "the file is not a JSON object" : err;
        needsRescue = true;
        return;
    }

    if (const js::Value* s = root.find("settings")) {
        settings.blockByDefault = s->boolean("blockMacroKeyboards", true);
        settings.matchConfidence = s->num("matchConfidence", kDefaultConfidence);
        settings.layoutsDir = s->str("layoutsDir", "layouts");
        settings.startEngineOnLaunch = s->boolean("startEngineOnLaunch", false);
        settings.minimizeToTray = s->boolean("minimizeToTray", true);
        settings.startMinimized = s->boolean("startMinimized", false);
        settings.toggleHotkey = s->str("toggleHotkey", settings.toggleHotkey);
    }
    if (settings.matchConfidence < kMinConfidence ||
        settings.matchConfidence > kMaxConfidence) {
        loadWarnings.push_back(
            "matchConfidence must be between 0.5 and 1.0 - using the default "
            "of 0.85 instead.");
        settings.matchConfidence = kDefaultConfidence;
    }
    if (settings.layoutsDir.empty()) settings.layoutsDir = "layouts";

    if (const js::Value* kbs = root.find("keyboards")) {
        if (kbs->isArr()) {
            for (const js::Value& k : kbs->arr) {
                Keyboard kb;
                kb.alias = k.str("alias");
                kb.match = k.str("match");
                kb.block = k.boolean("block", settings.blockByDefault);
                kb.deck = k.str("deck", "auto");
                if (kb.alias.empty()) continue;
                if (const js::Value* bs = k.find("bindings")) {
                    if (bs->isArr())
                        for (const js::Value& b : bs->arr) {
                            Binding bind;
                            bind.key = lower(trim(b.str("key")));
                            if (bind.key.empty()) continue;
                            bind.title = b.str("title");
                            bind.icon = b.str("icon");
                            bind.color = b.str("color");
                            bind.slot = b.integer("slot", -1);
                            if (const js::Value* st = b.find("steps"))
                                if (st->isArr())
                                    for (const js::Value& s : st->arr) {
                                        Step step = stepFromJson(s);
                                        if (!step.action.empty())
                                            bind.steps.push_back(std::move(step));
                                    }

                            if (!keys::toVk(bind.key))
                                loadWarnings.push_back(
                                    "\"" + kb.alias + "\": no key called \"" +
                                    bind.key +
                                    "\" - that macro can never fire.");

                            // Lookup resolves to the first binding for a key,
                            // so a second one is dead weight. Prefer whichever
                            // copy actually has steps rather than blindly
                            // keeping the first and silently dropping work.
                            if (Binding* clash = kb.findKey(bind.key)) {
                                const std::string dupKey = bind.key;
                                if (clash->steps.empty() && !bind.steps.empty())
                                    *clash = std::move(bind);
                                loadWarnings.push_back(
                                    "\"" + kb.alias + "\": \"" + dupKey +
                                    "\" is listed more than once - only one "
                                    "macro per key is kept.");
                                continue;
                            }
                            kb.bindings.push_back(std::move(bind));
                        }
                }
                keyboards.push_back(std::move(kb));
            }
        }
    }
}

std::string Config::toJson() const {
    js::Value root = js::Value::object();

    js::Value s = js::Value::object();
    s["blockMacroKeyboards"] = settings.blockByDefault;
    s["matchConfidence"] = settings.matchConfidence;
    s["layoutsDir"] = settings.layoutsDir;
    s["startEngineOnLaunch"] = settings.startEngineOnLaunch;
    s["minimizeToTray"] = settings.minimizeToTray;
    s["startMinimized"] = settings.startMinimized;
    s["toggleHotkey"] = settings.toggleHotkey;
    root["settings"] = std::move(s);

    js::Value kbs = js::Value::array();
    for (const Keyboard& kb : keyboards) {
        js::Value k = js::Value::object();
        k["alias"] = kb.alias;
        k["match"] = kb.match;
        k["block"] = kb.block;
        k["deck"] = kb.deck;
        js::Value bs = js::Value::array();
        for (const Binding& b : kb.bindings) {
            js::Value jb = js::Value::object();
            jb["key"] = b.key;
            if (!b.title.empty()) jb["title"] = b.title;
            if (!b.icon.empty()) jb["icon"] = b.icon;
            if (!b.color.empty()) jb["color"] = b.color;
            if (b.slot >= 0) jb["slot"] = b.slot;
            js::Value st = js::Value::array();
            for (const Step& s2 : b.steps) st.push(stepToJson(s2));
            jb["steps"] = std::move(st);
            bs.push(std::move(jb));
        }
        k["bindings"] = std::move(bs);
        kbs.push(std::move(k));
    }
    root["keyboards"] = std::move(kbs);
    return root.dump(2);
}

bool Config::saveAs(const std::string& file, std::string* error) const {
    if (!writeFileAtomic(file, toJson())) {
        if (error) *error = "could not write " + file;
        return false;
    }
    return true;
}

bool Config::save(std::string* error) {
    // A file we could not read is a file we must not destroy. Copy it aside
    // first, and if even that fails, refuse the save - losing one edit beats
    // losing the whole configuration.
    if (needsRescue) {
        if (fileExists(path)) {
            std::string bak = freeBackupPath(path);
            if (!copyFile(path, bak, false)) {
                if (error)
                    *error = "refusing to overwrite " + baseName(path) +
                             ": it could not be read earlier and a backup copy "
                             "could not be made (" + lastErrorMessage() +
                             "). Move the file aside by hand, then try again.";
                return false;
            }
            lastBackup = bak;
        }
        needsRescue = false;
    }

    if (!saveAs(path, error)) return false;
    dirty = false;
    return true;
}

bool Config::writeDailyBackup(int keep, std::string* outPath) const {
    if (path.empty() || !fileExists(path)) return false;
    const std::string dir = appPath("backups");
    const std::string dest =
        joinPath(dir, "macros-" + dateStamp() + ".json");
    if (fileExists(dest)) return false;   // already have today's

    ensureDir(dir);
    if (!copyFile(path, dest, false)) return false;
    if (outPath) *outPath = dest;

    // Keep the newest `keep`. listFiles sorts by name, and the names are
    // ISO dates, so the oldest are simply the ones at the front.
    std::vector<std::string> all = listFiles(dir, ".json");
    for (size_t i = 0; keep > 0 && i + (size_t)keep < all.size(); ++i)
        deleteFile(joinPath(dir, all[i]));
    return true;
}

Keyboard* Config::keyboard(const std::string& alias) {
    for (auto& k : keyboards)
        if (k.alias == alias) return &k;
    return nullptr;
}

std::string Config::uniqueAlias(const std::string& wanted) const {
    std::string base = trim(wanted);
    if (base.empty()) base = "Macro pad";
    std::string candidate = base;
    int n = 2;
    while (true) {
        bool taken = false;
        for (const auto& k : keyboards)
            if (iequals(k.alias, candidate)) { taken = true; break; }
        if (!taken) return candidate;
        candidate = base + " " + std::to_string(n++);
    }
}

const Keyboard* Config::keyboardForDevice(const std::string& devicePath) const {
    for (const auto& k : keyboards)
        if (specMatches(k.match, devicePath)) return &k;
    return nullptr;
}

bool Config::blockedForDevice(const std::string& devicePath) const {
    const Keyboard* k = keyboardForDevice(devicePath);
    return k && k->block;
}

bool specMatches(const std::string& match, const std::string& devicePath) {
    if (match.empty() || devicePath.empty()) return false;
    return lower(devicePath).find(lower(match)) != std::string::npos;
}

std::string chooseMatch(const dev::KeyboardDevice& d,
                        const std::vector<dev::KeyboardDevice>& all,
                        bool* stable) {
    if (stable) *stable = true;
    if (!d.vid.empty()) {
        bool twin = false;
        for (const auto& k : all)
            if (&k != &d && k.vid == d.vid && k.pid == d.pid && k.path != d.path)
                twin = true;
        if (!twin) return "VID_" + d.vid + "&PID_" + d.pid;
    }
    // Two identical keyboards: only the port-specific instance path can tell
    // them apart, so the user must keep this one in the same USB port.
    if (stable) *stable = false;
    return d.path;
}

// ------------------------------------------------------------ deck presets
const std::vector<DeckPreset>& deckPresets() {
    static const std::vector<DeckPreset> presets = [] {
        std::vector<DeckPreset> out;

        DeckPreset autoP;
        autoP.id = "auto";
        autoP.label = "Auto (grows as you add keys)";
        autoP.cols = 4;
        autoP.rows = 3;
        autoP.flow = true;
        out.push_back(autoP);

        auto grid = [](const char* id, const char* label, int cols, int rows) {
            DeckPreset p;
            p.id = id;
            p.label = label;
            p.cols = cols;
            p.rows = rows;
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c) {
                    DeckSlot s;
                    s.row = r;
                    s.col = c;
                    p.slots.push_back(s);
                }
            return p;
        };
        out.push_back(grid("pad3x2", "Macro pad - 3 x 2", 3, 2));
        out.push_back(grid("pad3x3", "Macro pad - 3 x 3", 3, 3));
        out.push_back(grid("pad4x3", "Macro pad - 4 x 3", 4, 3));
        out.push_back(grid("pad5x3", "Stream deck - 5 x 3", 5, 3));
        out.push_back(grid("pad8x4", "Stream deck XL - 8 x 4", 8, 4));

        DeckPreset np;
        np.id = "numpad";
        np.label = "Numeric keypad";
        np.cols = 4;
        np.rows = 5;
        np.fixedKeys = true;
        auto slot = [&](int r, int c, const char* key, int cs = 1, int rs = 1) {
            DeckSlot s;
            s.row = r;
            s.col = c;
            s.key = key;
            s.colSpan = cs;
            s.rowSpan = rs;
            np.slots.push_back(s);
        };
        slot(0, 0, "numlock");   slot(0, 1, "numdivide");
        slot(0, 2, "nummultiply"); slot(0, 3, "numsubtract");
        slot(1, 0, "num7"); slot(1, 1, "num8"); slot(1, 2, "num9");
        slot(1, 3, "numadd", 1, 2);
        slot(2, 0, "num4"); slot(2, 1, "num5"); slot(2, 2, "num6");
        slot(3, 0, "num1"); slot(3, 1, "num2"); slot(3, 2, "num3");
        slot(3, 3, "enter", 1, 2);
        slot(4, 0, "num0", 2, 1); slot(4, 2, "numdecimal");
        out.push_back(np);

        DeckPreset fk;
        fk.id = "fkeys";
        fk.label = "Function keys F1-F12";
        fk.cols = 6;
        fk.rows = 2;
        fk.fixedKeys = true;
        for (int i = 0; i < 12; ++i) {
            DeckSlot s;
            s.row = i / 6;
            s.col = i % 6;
            s.key = "f" + std::to_string(i + 1);
            fk.slots.push_back(s);
        }
        out.push_back(fk);

        return out;
    }();
    return presets;
}

const DeckPreset* findPreset(const std::string& id) {
    for (const auto& p : deckPresets())
        if (p.id == id) return &p;
    return &deckPresets()[0];   // "auto" is always a safe fallback
}

}  // namespace cfg
