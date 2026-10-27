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
           "For example: chrome.exe. Fill in any of these, or several."},
          {"class", "Window class", FieldKind::Text, "", false, "",
           "The steadiest way to name a window. Recording fills it in."}}},

        {"window_move", "Move or resize a window", "Windows",
         "Repositions a window.",
         {{"title", "Title contains", FieldKind::Text, "", false, "", ""},
          {"exe", "Program", FieldKind::Text, "", false, "", ""},
          {"class", "Window class", FieldKind::Text, "", false, "", ""},
          {"x", "X", FieldKind::Int, "", false, "",
           "Blank fields keep the window's current value."},
          {"y", "Y", FieldKind::Int, "", false, "", ""},
          {"w", "Width", FieldKind::Int, "", false, "", ""},
          {"h", "Height", FieldKind::Int, "", false, "", ""}}},

        {"window_minimize", "Minimise a window", "Windows", "",
         {{"title", "Title contains", FieldKind::Text, "", false, "", ""},
          {"exe", "Program", FieldKind::Text, "", false, "", ""},
          {"class", "Window class", FieldKind::Text, "", false, "", ""}}},

        {"window_maximize", "Maximise a window", "Windows", "",
         {{"title", "Title contains", FieldKind::Text, "", false, "", ""},
          {"exe", "Program", FieldKind::Text, "", false, "", ""},
          {"class", "Window class", FieldKind::Text, "", false, "", ""}}},

        // The pair that makes recorded click sequences repeatable. Clicks are
        // stored as absolute screen coordinates, which are only meaningful if
        // the target window is exactly where it was when they were recorded -
        // so put it back there first, and undo that afterwards.
        {"window_prepare", "Lock a window in place for clicking", "Windows",
         "Brings a window to the front and puts it back at the exact size and "
         "position it had when the clicks were recorded, so they land where "
         "they should. Pair it with \"Release the locked window\".",
         {{"class", "Window class", FieldKind::Text, "", false, "",
           "Recording fills this in from the window you clicked."},
          {"exe", "Program", FieldKind::Text, "", false, "", ""},
          {"title", "Title contains", FieldKind::Text, "", false, "", ""},
          {"x", "X", FieldKind::Int, "", false, "",
           "Where the window was when the clicks were recorded. Leave the "
           "size blank to move it without resizing."},
          {"y", "Y", FieldKind::Int, "", false, "", ""},
          {"w", "Width", FieldKind::Int, "", false, "", ""},
          {"h", "Height", FieldKind::Int, "", false, "", ""},
          {"activate_timeout", "Wait for it to come forward (seconds)",
           FieldKind::Float, "2", false, "", ""},
          {"settle_ms", "Settle for (ms)", FieldKind::Int, "300", false, "",
           "Time for the window to finish redrawing before the first click."},
          {"block_input", "Block my keyboard and mouse while it runs",
           FieldKind::Bool, "true", false, "",
           "Stops a stray movement derailing the sequence. Windows only "
           "permits this when Dental Macros runs as administrator."}}},

        {"window_release", "Release the locked window", "Windows",
         "Puts the window back where it was before \"Lock a window in place\" "
         "moved it, and hands your keyboard and mouse back.",
         {{"restore_window", "Put the window back", FieldKind::Bool, "true",
           false, "", ""},
          {"unblock_input", "Give my keyboard and mouse back",
           FieldKind::Bool, "true", false, "", ""}}},

        {"window_save", "Save the window layout", "Windows",
         "Remembers where every window is right now.",
         {{"layout", "Layout name", FieldKind::Layout, "main", true, "", ""}}},

        {"window_restore", "Restore a window layout", "Windows",
         "Puts every window back where a saved layout had it.",
         {{"layout", "Layout name", FieldKind::Layout, "main", true, "", ""}}},

        {"page_goto", "Go to a page", "Pages",
         "Brings another page of this deck to the front, and leaves it there.",
         {{"page", "Page", FieldKind::Text, "", false, "",
           "The page's name. Leave blank and use the direction below "
           "instead."},
          {"direction", "Or move", FieldKind::Choice, "named", false,
           "named,next,previous,default",
           "\"Default\" is the page with no application rule."}}},

        {"page_hold", "Hold for a page", "Pages",
         "Shows another page only while this key is held down, and goes back "
         "the moment it is released. A key on the pad doing what the touch "
         "strip does on a hardware macro pad.",
         {{"page", "Page", FieldKind::Text, "", true, "",
           "The page to show while the key is held."}}},
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
    if (action == "window_release") return "Release the locked window";
    if (action == "window_prepare") {
        std::string who = p("class");
        if (who.empty()) who = p("exe");
        if (who.empty()) who = p("title");
        if (who.empty()) who = "window";
        std::string out = "Lock " + ellipsis(who, 22);
        if (params.has("x") && params.has("y"))
            out += " at " + std::to_string(params.integer("x")) + ", " +
                   std::to_string(params.integer("y"));
        if (params.has("w") && params.has("h"))
            out += " (" + std::to_string(params.integer("w")) + "x" +
                   std::to_string(params.integer("h")) + ")";
        return out;
    }
    if (action.rfind("window_", 0) == 0) {
        std::string who = p("title");
        if (who.empty()) who = p("exe");
        if (who.empty()) who = p("class");
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

// --------------------------------------------------------------- AppRule
bool AppRule::empty() const {
    return trim(exe).empty() && trim(cls).empty() && trim(title).empty();
}

bool AppRule::matches(const std::string& winExe, const std::string& winCls,
                      const std::string& winTitle) const {
    // A rule with nothing in it belongs to no window at all. Treating it as a
    // wildcard would mean the first unconfigured page grabbed every app and
    // no other page ever got a turn.
    if (empty()) return false;
    if (!trim(exe).empty() && lower(trim(exe)) != lower(trim(winExe)))
        return false;
    if (!trim(cls).empty() && trim(cls) != trim(winCls)) return false;
    if (!trim(title).empty() &&
        lower(winTitle).find(lower(trim(title))) == std::string::npos)
        return false;
    return true;
}

int AppRule::specificity() const {
    int n = 0;
    if (!trim(exe).empty()) ++n;
    if (!trim(cls).empty()) ++n;
    if (!trim(title).empty()) ++n;
    return n;
}

std::string AppRule::summary() const {
    if (empty()) return "Any application";
    std::string s;
    auto add = [&](const std::string& label, const std::string& v) {
        if (trim(v).empty()) return;
        if (!s.empty()) s += ", ";
        s += label + " " + trim(v);
    };
    add("", exe);
    add("class", cls);
    add("title contains", title);
    return s;
}

// ------------------------------------------------------------------ Page
Binding* Page::findKey(const std::string& key) {
    for (auto& b : bindings)
        if (b.key == key) return &b;
    return nullptr;
}

const Binding* Page::findKey(const std::string& key) const {
    for (const auto& b : bindings)
        if (b.key == key) return &b;
    return nullptr;
}

Binding* Page::findSlot(int slot) {
    if (slot < 0) return nullptr;
    for (auto& b : bindings)
        if (b.slot == slot) return &b;
    return nullptr;
}

std::string Page::displayName() const {
    if (!trim(name).empty()) return trim(name);
    if (!rule.empty()) return rule.summary();
    return "Page";
}

// -------------------------------------------------------------- Keyboard
void Keyboard::normalizePages() {
    if (pages.empty()) pages.push_back(Page{});
    if (activePage < 0 || activePage >= (int)pages.size())
        activePage = defaultPage();
}

Page& Keyboard::active() {
    normalizePages();
    return pages[(size_t)activePage];
}

const Page& Keyboard::active() const {
    if (pages.empty()) {
        // Only reachable if a caller emptied the vector by hand. A static
        // stand-in is better than undefined behaviour on a const path that
        // cannot repair itself.
        static const Page fallback;
        return fallback;
    }
    int i = activePage;
    if (i < 0 || i >= (int)pages.size()) i = defaultPage();
    return pages[(size_t)i];
}

int Keyboard::defaultPage() const {
    for (size_t i = 0; i < pages.size(); ++i)
        if (pages[i].rule.empty()) return (int)i;
    return 0;
}

int Keyboard::pageForApp(const std::string& winExe, const std::string& winCls,
                         const std::string& winTitle) const {
    int best = -1, bestScore = 0;
    for (size_t i = 0; i < pages.size(); ++i) {
        if (!pages[i].rule.matches(winExe, winCls, winTitle)) continue;
        const int score = pages[i].rule.specificity();
        // Strictly greater, so the first page written wins a tie. Page order
        // is something you can see and drag; "whichever we happened to visit
        // last" is not.
        if (score > bestScore) { bestScore = score; best = (int)i; }
    }
    return best >= 0 ? best : defaultPage();
}

int Keyboard::pageByName(const std::string& name) const {
    const std::string want = lower(trim(name));
    if (want.empty()) return -1;
    for (size_t i = 0; i < pages.size(); ++i)
        if (lower(trim(pages[i].name)) == want) return (int)i;
    return -1;
}

std::string Keyboard::uniquePageName(const std::string& wanted) const {
    std::string base = trim(wanted);
    if (base.empty()) base = "Page";
    if (pageByName(base) < 0) return base;
    for (int n = 2; n < 1000; ++n) {
        std::string tryName = base + " " + std::to_string(n);
        if (pageByName(tryName) < 0) return tryName;
    }
    return base;
}

Binding* Keyboard::findKey(const std::string& key) {
    return active().findKey(key);
}

const Binding* Keyboard::findKey(const std::string& key) const {
    return active().findKey(key);
}

Binding* Keyboard::findSlot(int slot) {
    return active().findSlot(slot);
}

bool Keyboard::anyPageHasMacro(const std::string& key) const {
    for (const Page& p : pages)
        if (const Binding* b = p.findKey(key))
            if (!b->steps.empty()) return true;
    return false;
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
    loadLocked = false;
    lastBackup.clear();
    dirty = false;

    std::string text;
    const ReadResult read = readFileEx(file, text);
    if (read == ReadResult::Missing) return;   // brand new - defaults are fine
    // A file that is really there was written by someone already using the
    // app - even a damaged one - so Advanced unless it says otherwise. Only a
    // missing file (a brand-new setup) keeps the Simple default.
    settings.simpleMode = false;
    settings.setupDone = true;
    switch (read) {
        case ReadResult::Missing:
            return;
        case ReadResult::Unreadable:
            // Emphatically not the same as "no config yet". Something is
            // there and we could not read it, so nothing may overwrite it.
            loadError =
                "the file is there but Windows would not open it - it may be "
                "open in another program, or locked by sync or backup software";
            needsRescue = true;
            loadLocked = true;
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
        settings.blockAllKeysByDefault =
            s->boolean("blockAllKeysByDefault", true);
        settings.matchConfidence = s->num("matchConfidence", kDefaultConfidence);
        settings.layoutsDir = s->str("layoutsDir", "layouts");
        settings.startEngineOnLaunch = s->boolean("startEngineOnLaunch", false);
        settings.switchPageWithApp = s->boolean("switchPageWithApp", true);
        settings.minimizeToTray = s->boolean("minimizeToTray", true);
        settings.startMinimized = s->boolean("startMinimized", false);
        settings.toggleHotkey = s->str("toggleHotkey", settings.toggleHotkey);
        settings.miniDeckVisible = s->boolean("miniDeckVisible", false);
        // INT_MIN survives a round trip as "no position saved yet", which is
        // what tells the palette to pick a corner rather than trust a 0,0
        // that would put it under the Start button.
        settings.miniDeckX = (int)s->num("miniDeckX", (double)INT_MIN);
        settings.miniDeckY = (int)s->num("miniDeckY", (double)INT_MIN);
        settings.miniDeckTile = (int)s->num("miniDeckTile", 38);
        settings.miniDeckCols = (int)s->num("miniDeckCols", 5);
        settings.simpleMode = s->str("uiMode", "") == "simple";
        settings.setupDone = s->boolean("setupDone", true);
    }
    // Clamped rather than refused: a hand-edited file with a silly size still
    // opens, it just opens with a usable palette.
    settings.miniDeckTile = std::max(30, std::min(46, settings.miniDeckTile));
    settings.miniDeckCols = std::max(2, std::min(10, settings.miniDeckCols));
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
                // Absent in files written before this option existed, and
                // true is what those files meant.
                kb.blockAllKeys = k.boolean("blockAllKeys", true);
                kb.softwareOnly = k.boolean("softwareOnly", false);
                // Absent in every file written before mice were supported,
                // and "keyboard" is what those files meant.
                kb.isMouse = lower(trim(k.str("device"))) == "mouse";
                // Belt and braces: a software deck that somehow arrived with
                // block set must not install the hook.
                if (kb.softwareOnly) { kb.block = false; kb.match.clear(); }
                kb.deck = k.str("deck", "auto");
                if (kb.alias.empty()) continue;

                // Reading one page's worth of bindings. Duplicate keys are
                // still refused, but only within a page: the whole point of
                // pages is that the same key means different things on each
                // of them, so comparing across pages would reject exactly
                // what the feature is for.
                auto readBindings = [&](const js::Value& src, Page& page,
                                        const std::string& where) {
                    if (!src.isArr()) return;
                    for (const js::Value& b : src.arr) {
                        Binding bind;
                        bind.key = lower(trim(b.str("key")));
                        if (bind.key.empty()) continue;
                        bind.title = b.str("title");
                        bind.icon = b.str("icon");
                        bind.color = b.str("color");
                        bind.slot = b.integer("slot", -1);
                        auto readSteps = [&](const char* field,
                                             std::vector<Step>& into) {
                            const js::Value* st = b.find(field);
                            if (!st || !st->isArr()) return;
                            for (const js::Value& s : st->arr) {
                                Step step = stepFromJson(s);
                                if (!step.action.empty())
                                    into.push_back(std::move(step));
                            }
                        };
                        readSteps("steps", bind.steps);
                        readSteps("scrollUp", bind.scrollUp);
                        readSteps("scrollDown", bind.scrollDown);

                        if (!keyBindable(bind.key)) {
                            loadWarnings.push_back(
                                where + ": \"" + bind.key +
                                "\" cannot carry a macro - left click has to "
                                "keep working. That binding is ignored.");
                            continue;
                        }

                        // A software button's key is an opaque id, not a key
                        // name, so there is nothing to warn about.
                        if (!kb.softwareOnly && !keys::toVk(bind.key))
                            loadWarnings.push_back(
                                where + ": no key called \"" + bind.key +
                                "\" - that macro can never fire.");

                        // Lookup resolves to the first binding for a key, so
                        // a second one is dead weight. Prefer whichever copy
                        // actually has steps rather than blindly keeping the
                        // first and silently dropping work.
                        if (Binding* clash = page.findKey(bind.key)) {
                            const std::string dupKey = bind.key;
                            if (clash->steps.empty() && !bind.steps.empty())
                                *clash = std::move(bind);
                            loadWarnings.push_back(
                                where + ": \"" + dupKey +
                                "\" is listed more than once - only one "
                                "macro per key is kept.");
                            continue;
                        }
                        page.bindings.push_back(std::move(bind));
                    }
                };

                kb.pages.clear();
                const js::Value* ps = k.find("pages");
                if (ps && ps->isArr() && !ps->arr.empty()) {
                    for (const js::Value& p : ps->arr) {
                        Page page;
                        page.name = p.str("name");
                        page.color = p.str("color");
                        if (const js::Value* r = p.find("appRule")) {
                            if (r->isObj()) {
                                page.rule.exe = r->str("exe");
                                page.rule.cls = r->str("class");
                                page.rule.title = r->str("title");
                            }
                        }
                        if (page.name.empty())
                            page.name = "Page " +
                                        std::to_string(kb.pages.size() + 1);
                        if (const js::Value* bs = p.find("bindings"))
                            readBindings(*bs, page,
                                         "\"" + kb.alias + "\" / \"" +
                                             page.name + "\"");
                        kb.pages.push_back(std::move(page));
                    }
                } else {
                    // No pages in the file: everything written before this
                    // feature existed, and one page is exactly what it meant.
                    Page page;
                    if (const js::Value* bs = k.find("bindings"))
                        readBindings(*bs, page, "\"" + kb.alias + "\"");
                    kb.pages.push_back(std::move(page));
                }
                kb.normalizePages();
                kb.activePage = kb.defaultPage();
                keyboards.push_back(std::move(kb));
            }
        }
    }
}

std::string Config::toJson() const {
    js::Value root = js::Value::object();

    js::Value s = js::Value::object();
    s["blockMacroKeyboards"] = settings.blockByDefault;
    s["blockAllKeysByDefault"] = settings.blockAllKeysByDefault;
    s["matchConfidence"] = settings.matchConfidence;
    s["layoutsDir"] = settings.layoutsDir;
    s["startEngineOnLaunch"] = settings.startEngineOnLaunch;
    s["switchPageWithApp"] = settings.switchPageWithApp;
    s["minimizeToTray"] = settings.minimizeToTray;
    s["startMinimized"] = settings.startMinimized;
    s["toggleHotkey"] = settings.toggleHotkey;
    s["miniDeckVisible"] = settings.miniDeckVisible;
    s["miniDeckX"] = (double)settings.miniDeckX;
    s["miniDeckY"] = (double)settings.miniDeckY;
    s["miniDeckTile"] = (double)settings.miniDeckTile;
    s["miniDeckCols"] = (double)settings.miniDeckCols;
    s["uiMode"] = settings.simpleMode ? "simple" : "advanced";
    s["setupDone"] = settings.setupDone;
    root["settings"] = std::move(s);

    js::Value kbs = js::Value::array();
    for (const Keyboard& kb : keyboards) {
        js::Value k = js::Value::object();
        k["alias"] = kb.alias;
        k["match"] = kb.match;
        k["block"] = kb.block;
        k["blockAllKeys"] = kb.blockAllKeys;
        if (kb.softwareOnly) k["softwareOnly"] = true;
        if (kb.isMouse) k["device"] = std::string("mouse");
        k["deck"] = kb.deck;

        auto bindingsJson = [](const Page& p) {
            js::Value bs = js::Value::array();
            for (const Binding& b : p.bindings) {
                js::Value jb = js::Value::object();
                jb["key"] = b.key;
                if (!b.title.empty()) jb["title"] = b.title;
                if (!b.icon.empty()) jb["icon"] = b.icon;
                if (!b.color.empty()) jb["color"] = b.color;
                if (b.slot >= 0) jb["slot"] = b.slot;
                js::Value st = js::Value::array();
                for (const Step& s2 : b.steps) st.push(stepToJson(s2));
                jb["steps"] = std::move(st);
                // Only written when there is something to write: a tile that
                // is not a dial should not grow two empty arrays.
                auto putSteps = [&](const char* field,
                                    const std::vector<Step>& from) {
                    if (from.empty()) return;
                    js::Value arr = js::Value::array();
                    for (const Step& s3 : from) arr.push(stepToJson(s3));
                    jb[field] = std::move(arr);
                };
                putSteps("scrollUp", b.scrollUp);
                putSteps("scrollDown", b.scrollDown);
                bs.push(std::move(jb));
            }
            return bs;
        };

        // A deck that uses one plain page is written the way it always was.
        // The file only grows the new shape once there is something in it
        // that the old shape cannot hold, which means an ordinary config
        // stays readable by a build from before pages existed.
        // The name counts too. Without it here, naming your one page and
        // saving would write it in the old shape and quietly lose the name -
        // the file only earns the old shape when it holds nothing the old
        // shape cannot express.
        const bool plain = kb.pages.size() == 1 && kb.pages[0].rule.empty() &&
                           kb.pages[0].color.empty() &&
                           trim(kb.pages[0].name).empty();
        if (plain) {
            k["bindings"] = bindingsJson(kb.pages[0]);
        } else {
            js::Value ps = js::Value::array();
            for (size_t i = 0; i < kb.pages.size(); ++i) {
                const Page& p = kb.pages[i];
                js::Value jp = js::Value::object();
                jp["name"] = p.name.empty()
                                 ? "Page " + std::to_string(i + 1)
                                 : p.name;
                if (!p.color.empty()) jp["color"] = p.color;
                if (!p.rule.empty()) {
                    js::Value r = js::Value::object();
                    if (!p.rule.exe.empty()) r["exe"] = p.rule.exe;
                    if (!p.rule.cls.empty()) r["class"] = p.rule.cls;
                    if (!p.rule.title.empty()) r["title"] = p.rule.title;
                    jp["appRule"] = std::move(r);
                }
                jp["bindings"] = bindingsJson(p);
                ps.push(std::move(jp));
            }
            k["pages"] = std::move(ps);
        }
        kbs.push(std::move(k));
    }
    root["keyboards"] = std::move(kbs);
    return root.dump(2);
}

bool Config::saveAs(const std::string& file, std::string* error,
                    unsigned long* win32Error) const {
    if (!writeFileAtomic(file, toJson())) {
        const DWORD code = GetLastError();
        if (win32Error) *win32Error = code;
        if (error)
            *error = "could not write " + file + ": " + lastErrorMessage(code);
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
                lastSaveError = GetLastError();
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

    lastSaveError = 0;
    if (!saveAs(path, error, &lastSaveError)) return false;
    dirty = false;
    return true;
}

bool Config::writeDailyBackup(int keep, std::string* outPath) const {
    if (path.empty() || !fileExists(path)) return false;
    const std::string dir = dataPath("backups");
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

const Keyboard* Config::keyboardForDevice(const std::string& devicePath,
                                          int vk) const {
    const bool pointer = dev::isMouseVk(vk);
    const Keyboard* otherKind = nullptr;
    for (const auto& k : keyboards) {
        if (!specMatches(k.match, devicePath)) continue;
        if (vk == 0 || k.isMouse == pointer) return &k;
        if (!otherKind) otherKind = &k;
    }
    return otherKind;
}

bool Config::blockedForDevice(const std::string& devicePath) const {
    const Keyboard* k = keyboardForDevice(devicePath);
    return k && k->block;
}

bool Config::couldBlockVk(int vk) const {
    // Same rule as blockedForKey, applied to the cheap pre-filter so the
    // primary button never even reaches the correlation wait.
    if (dev::isPrimaryButtonVk(vk) && pointerCount <= 1) return false;
    const std::string name = keys::vkName(vk);
    for (const Keyboard& k : keyboards) {
        if (!k.block) continue;
        if (k.blockAllKeys) return true;   // every key on that device is fair game
        if (k.anyPageHasMacro(name)) return true;
    }
    return false;
}

bool keyBindable(const std::string& key, std::string* why) {
    if (lower(trim(key)) == "mouseleft") {
        if (why)
            *why =
                "Left click is how you work the machine, so it cannot carry a "
                "macro. A macro there would fire on every click you made "
                "anywhere - and if it were blocked as well, there would be no "
                "way left to click your way out of it.\n\nEvery other button "
                "on the mouse is yours, including the wheel and the thumb "
                "buttons.";
        return false;
    }
    return true;
}

bool Config::anyMouseBlocks() const {
    for (const Keyboard& k : keyboards)
        if (k.isMouse && k.block && !k.softwareOnly) return true;
    return false;
}

bool Config::blockedForKey(const std::string& devicePath, int vk) const {
    const Keyboard* k = keyboardForDevice(devicePath, vk);
    if (!k || !k->block) return false;

    // The escape hatch for a keyboard is that you can still type
    // Ctrl+Alt+Shift+M. There is no equivalent for a pointer: swallow the
    // primary button on the only mouse in the machine and there is no way
    // left to click the thing that would turn this off. So that one button is
    // never taken, whatever the configuration says - not even under "block
    // every key". With a second pointer attached there is still a way out, so
    // a genuinely spare mouse can give up all of its buttons.
    if (dev::isPrimaryButtonVk(vk) && pointerCount <= 1) return false;

    if (k->blockAllKeys) return true;

    // Selective mode: take only the keys that actually run something. A
    // binding with no steps is a half-finished tile, not a macro - taking its
    // key would silence a key for no benefit, which is precisely the
    // complaint this option exists to fix.
    //
    // Asked of every page, not just the one in front. A key that types into
    // your document on page 2 and runs a macro on page 3 would be worse than
    // either answer on its own: what a key does could still change with the
    // page, but whether it reaches the application underneath should not.
    return k->anyPageHasMacro(keys::vkName(vk));
}

bool specMatches(const std::string& match, const std::string& devicePath) {
    if (match.empty() || devicePath.empty()) return false;
    if (lower(devicePath).find(lower(match)) != std::string::npos) return true;
    // A Bluetooth keypad added by a build that did not understand Bluetooth
    // was tied to the full path of the one input it was identified by - its
    // keyboard, usually - so its media keys, which arrive on an input of their
    // own, could never reach its deck. The address in that path names the
    // whole keypad.
    const std::string addr = dev::bluetoothAddress(match);
    return !addr.empty() && dev::bluetoothAddress(devicePath) == addr;
}

std::string chooseMatch(const dev::InputDevice& d,
                        const std::vector<dev::InputDevice>& all,
                        bool* stable) {
    if (stable) *stable = true;
    // A Bluetooth keypad is matched by its address. It is the one thing all
    // of the keypad's inputs share, so its keys and its media keys land on the
    // same deck, and it is the keypad's own, so it survives restarts and
    // reconnecting. The ids would share just as well, but they cannot rule
    // out an identical twin: a keypad that is asleep is not listed at all.
    if (!d.btAddress.empty()) return "_" + d.btAddress;
    if (!d.vid.empty()) {
        bool twin = false;
        for (const auto& k : all)
            if (&k != &d && k.vid == d.vid && k.pid == d.pid && k.path != d.path)
                twin = true;
        if (!twin)
            return !d.idMatch.empty() ? d.idMatch
                                      : "VID_" + d.vid + "&PID_" + d.pid;
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

        DeckPreset ms;
        ms.id = "mouse";
        ms.label = "Mouse buttons";
        ms.cols = 3;
        ms.rows = 3;
        ms.fixedKeys = true;
        auto mslot = [&](int r, int c, const char* key) {
            DeckSlot s;
            s.row = r;
            s.col = c;
            s.key = key;
            ms.slots.push_back(s);
        };
        // Laid out the way the hand meets it: thumb buttons out to the
        // sides, the wheel running down the middle. There is deliberately no
        // slot for left click - see keyBindable() - and the space it would
        // have taken goes to the first of the extra buttons a multi-button
        // mouse has instead.
        mslot(0, 0, "mouse4"); mslot(0, 1, "wheelup"); mslot(0, 2, "mouse5");
        mslot(1, 0, "mouse6"); mslot(1, 1, "mousemiddle");
        mslot(1, 2, "mouseright");
        mslot(2, 0, "mouse7"); mslot(2, 1, "wheeldown"); mslot(2, 2, "mouse8");
        out.push_back(ms);

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
