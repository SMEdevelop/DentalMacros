// MacroDeck self-test.
//
// Exercises every non-UI subsystem against the real machine: JSON, the config
// model, the macrokb YAML importer, key tables, deck presets, window
// management and the image matcher.
//
// Build and run with run_tests.bat. It deliberately does NOT link the
// blocking hook, so it stays a plain console program.

#include "config.h"
#include "device.h"
#include "engine.h"
#include "iconlib.h"
#include "json.h"
#include "keys.h"
#include "util.h"
#include "vision.h"
#include "winman.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;

static void ok(bool cond, const std::string& what,
               const std::string& detail = "") {
    if (cond) {
        ++g_pass;
        std::printf("  [ok]   %s\n", what.c_str());
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s%s%s\n", what.c_str(),
                    detail.empty() ? "" : "  --  ", detail.c_str());
    }
}

static void section(const char* name) {
    std::printf("\n%s\n", name);
}

// ---------------------------------------------------------------- json
static void testJson() {
    section("JSON");

    const std::string src = R"({
      "name": "test \"quoted\"",
      "n": 12,
      "f": 0.5,
      "yes": true,
      "nothing": null,
      "arr": [1, 2, {"deep": "value"}],
      "unicode": "caf\u00e9 \u2192 ok"
    })";

    std::string err;
    js::Value v = js::Value::parse(src, &err);
    ok(err.empty() && v.isObj(), "parses a document", err);
    ok(v.str("name") == "test \"quoted\"", "unescapes quotes");
    ok(v.integer("n") == 12, "reads integers");
    ok(std::fabs(v.num("f") - 0.5) < 1e-9, "reads floats");
    ok(v.boolean("yes"), "reads booleans");
    ok(v.find("nothing") && v.find("nothing")->isNull(), "reads null");
    ok(v.find("arr") && v.find("arr")->size() == 3, "reads arrays");
    ok(v.find("arr")->arr[2].str("deep") == "value", "reads nested objects");
    ok(v.str("unicode") == "caf\xC3\xA9 \xE2\x86\x92 ok",
       "decodes \\u escapes to UTF-8");

    // round trip
    std::string dumped = v.dump(2);
    std::string err2;
    js::Value v2 = js::Value::parse(dumped, &err2);
    ok(err2.empty(), "re-parses its own output", err2);
    ok(v2.str("name") == v.str("name") && v2.integer("n") == v.integer("n") &&
           v2.str("unicode") == v.str("unicode"),
       "survives a round trip");

    // key order is preserved so saved files stay readable
    js::Value ordered = js::Value::object();
    ordered["zebra"] = 1;
    ordered["apple"] = 2;
    ok(ordered.obj[0].first == "zebra" && ordered.obj[1].first == "apple",
       "keeps object keys in insertion order");

    // malformed input must be reported, not silently half-parsed
    js::Value bad = js::Value::parse("{\"a\": }", &err);
    ok(!err.empty() && bad.isNull(), "rejects malformed JSON");
    js::Value trailing = js::Value::parse("{} garbage", &err);
    ok(!err.empty(), "rejects trailing junk");

    // comments and trailing commas are tolerated for hand-edited files
    js::Value lenient = js::Value::parse("{ /* hi */ \"a\": 1, }", &err);
    ok(err.empty() && lenient.integer("a") == 1,
       "tolerates comments and trailing commas", err);
}

// ---------------------------------------------------------------- keys
static void testKeys() {
    section("Key tables");

    ok(keys::toVk("a") == 0x41, "letters map to VK codes");
    ok(keys::toVk("num1") == 0x61, "numpad keys map");
    ok(keys::toVk("f12") == 0x7B, "function keys map");
    ok(keys::toVk("escape") == 0x1B, "aliases resolve (escape -> esc)");
    ok(keys::toVk("MENU") == 0x5D, "menu maps to VK_APPS, not Alt");
    ok(keys::toVk("not_a_key") == 0, "unknown names return 0");

    ok(keys::vkName(0x61) == "num1", "reverse lookup works");
    ok(keys::vkName(0x11) == "ctrl", "prefers the generic modifier name");

    // every canonical name must survive name -> vk -> name
    int mismatches = 0;
    for (const auto& n : keys::allNames()) {
        int vk = keys::toVk(n);
        if (vk == 0) { ++mismatches; continue; }
        if (keys::toVk(keys::vkName(vk)) != vk) ++mismatches;
    }
    ok(mismatches == 0, "all key names round trip",
       std::to_string(mismatches) + " mismatched");

    std::string err;
    auto combo = keys::parseCombo("ctrl+shift+t", &err);
    ok(combo.size() == 3 && combo[0] == 0x11 && combo[1] == 0x10 &&
           combo[2] == 0x54,
       "parses ctrl+shift+t in order", err);
    keys::parseCombo("ctrl+nope", &err);
    ok(!err.empty(), "reports unknown keys in a combo");

    ok(keys::displayName("num1") == "Num 1", "pretty-prints numpad keys");
    ok(keys::displayName("pageup") == "PgUp", "pretty-prints navigation keys");

    // global shortcut parsing
    unsigned mods = 0;
    int vk = 0;
    ok(keys::parseHotkey("ctrl+alt+shift+m", mods, vk, &err) &&
           mods == (MOD_CONTROL | MOD_ALT | MOD_SHIFT) && vk == 0x4D,
       "splits a shortcut into modifiers and a key", err);
    ok(keys::parseHotkey("Win + F9", mods, vk, &err) && mods == MOD_WIN &&
           vk == 0x78,
       "tolerates spaces and capitals", err);
    ok(!keys::parseHotkey("m", mods, vk, &err),
       "refuses a shortcut with no modifier");
    ok(!keys::parseHotkey("ctrl+alt", mods, vk, &err),
       "refuses a shortcut with no key");
    ok(!keys::parseHotkey("ctrl+a+b", mods, vk, &err),
       "refuses two non-modifier keys");
    ok(!keys::parseHotkey("ctrl+nope", mods, vk, &err),
       "refuses an unknown key name");

    // The listen button prints the shortcut, so it has to read like Windows
    // writes one whatever order it was typed or stored in.
    ok(keys::prettyCombo("ctrl+alt+shift+m") == "Ctrl+Alt+Shift+M",
       "formats a shortcut in Windows order",
       keys::prettyCombo("ctrl+alt+shift+m"));
    ok(keys::prettyCombo("shift+alt+ctrl+m") == "Ctrl+Alt+Shift+M",
       "reorders modifiers into Windows order",
       keys::prettyCombo("shift+alt+ctrl+m"));
    ok(keys::prettyCombo("win+f9") == "Win+F9", "formats function keys",
       keys::prettyCombo("win+f9"));
    ok(keys::prettyCombo("ctrl+numadd") == "Ctrl+Num +",
       "formats numpad keys", keys::prettyCombo("ctrl+numadd"));
    ok(keys::prettyCombo("") == "", "an empty shortcut formats to nothing");
    ok(keys::prettyCombo("garbage") == "garbage",
       "leaves an unparseable shortcut alone");
}

// -------------------------------------------------------------- presets
static void testPresets() {
    section("Deck presets");

    ok(cfg::deckPresets().size() >= 6, "presets are defined");
    ok(cfg::findPreset("auto")->flow, "the auto preset flows");
    ok(cfg::findPreset("does-not-exist")->id == "auto",
       "unknown preset ids fall back to auto");

    const cfg::DeckPreset* np = cfg::findPreset("numpad");
    ok(np->fixedKeys, "the numpad preset carries fixed keys");
    ok(np->slots.size() == 17, "the numpad has 17 keys",
       std::to_string(np->slots.size()));

    int badKeys = 0;
    for (const auto& s : np->slots)
        if (s.key.empty() || keys::toVk(s.key) == 0) ++badKeys;
    ok(badKeys == 0, "every numpad slot names a real key",
       std::to_string(badKeys) + " bad");

    // slots must not overlap
    const cfg::DeckPreset* fk = cfg::findPreset("fkeys");
    ok(fk->slots.size() == 12, "F-key preset has 12 keys");

    // every action in the schema must have a unique id and a label
    int dupes = 0;
    for (size_t i = 0; i < cfg::actionDefs().size(); ++i)
        for (size_t j = i + 1; j < cfg::actionDefs().size(); ++j)
            if (std::string(cfg::actionDefs()[i].id) ==
                cfg::actionDefs()[j].id)
                ++dupes;
    ok(dupes == 0, "action ids are unique");
    ok(cfg::findAction("click_image") != nullptr, "actions are findable by id");
    ok(cfg::findAction("nope") == nullptr, "unknown actions return null");
}

// --------------------------------------------------------------- config
static void testConfig() {
    section("Config model");

    std::string path = appPath("selftest-config.json");

    cfg::Config c;
    c.path = path;
    c.settings.matchConfidence = 0.77;
    c.settings.startEngineOnLaunch = true;

    cfg::Keyboard kb;
    kb.alias = "Test pad";
    kb.match = "VID_1234&PID_5678";
    kb.block = true;
    kb.deck = "numpad";

    cfg::Binding b;
    b.key = "num1";
    b.title = "Say hello";
    b.color = "#3b82f6";
    cfg::Step s;
    s.action = "type";
    s.params["text"] = "hello \"world\"\nsecond line";
    b.steps.push_back(s);
    cfg::Step s2;
    s2.action = "sleep";
    s2.params["seconds"] = 1.5;
    b.steps.push_back(s2);
    kb.bindings.push_back(b);
    c.keyboards.push_back(kb);

    std::string err;
    ok(c.save(&err), "saves", err);

    cfg::Config loaded;
    loaded.load(path);
    ok(loaded.loadError.empty(), "reloads without error", loaded.loadError);
    ok(loaded.keyboards.size() == 1, "keeps the keyboard");
    ok(std::fabs(loaded.settings.matchConfidence - 0.77) < 1e-9,
       "keeps settings");
    ok(loaded.settings.startEngineOnLaunch, "keeps boolean settings");

    if (!loaded.keyboards.empty()) {
        const cfg::Keyboard& k = loaded.keyboards[0];
        ok(k.alias == "Test pad" && k.deck == "numpad", "keeps keyboard fields");
        ok(k.bindings.size() == 1, "keeps the binding");
        if (!k.bindings.empty()) {
            const cfg::Binding& lb = k.bindings[0];
            ok(lb.key == "num1" && lb.title == "Say hello", "keeps key + title");
            ok(lb.color == "#3b82f6", "keeps the colour");
            ok(lb.steps.size() == 2, "keeps both steps");
            if (lb.steps.size() == 2) {
                ok(lb.steps[0].params.str("text") ==
                       "hello \"world\"\nsecond line",
                   "preserves quotes and newlines in text");
                ok(std::fabs(lb.steps[1].params.num("seconds") - 1.5) < 1e-9,
                   "preserves fractional numbers");
            }
        }
    }

    // device matching
    ok(cfg::specMatches("VID_1234&PID_5678",
                        "\\\\?\\HID#VID_1234&PID_5678&MI_00#9&abc#{guid}"),
       "matches a device by firmware id");
    ok(cfg::specMatches("vid_1234&pid_5678",
                        "\\\\?\\HID#VID_1234&PID_5678&MI_00#9&abc#{guid}"),
       "device matching is case-insensitive");
    ok(!cfg::specMatches("VID_9999&PID_0000",
                         "\\\\?\\HID#VID_1234&PID_5678&MI_00#9&abc#{guid}"),
       "does not match a different device");
    ok(!cfg::specMatches("", "anything"), "an empty match never matches");

    ok(loaded.keyboardForDevice(
           "\\\\?\\HID#VID_1234&PID_5678&MI_00#9&abc#{guid}") != nullptr,
       "finds the keyboard for a device path");
    ok(loaded.blockedForDevice(
           "\\\\?\\HID#VID_1234&PID_5678&MI_00#9&abc#{guid}"),
       "reports blocking for that device");

    // unique aliases
    ok(loaded.uniqueAlias("Test pad") != "Test pad",
       "de-duplicates keyboard names");

    // a damaged file must not lose data or throw
    writeFileAtomic(path, "{ this is not json ");
    cfg::Config broken;
    broken.load(path);
    ok(!broken.loadError.empty(), "reports a damaged config");
    ok(broken.keyboards.empty(), "starts empty rather than half-loaded");

    // a missing file is just a fresh install
    DeleteFileW(widen(path).c_str());
    cfg::Config fresh;
    fresh.load(path);
    ok(fresh.loadError.empty() && fresh.keyboards.empty(),
       "a missing config is not an error");

    // step summaries
    cfg::Step t;
    t.action = "type";
    t.params["text"] = "hello";
    ok(t.summary() == "Type \"hello\"", "summarises a type step", t.summary());
    cfg::Step w;
    w.action = "sleep";
    w.params["seconds"] = 2;
    ok(w.summary() == "Wait 2s", "summarises a sleep step", w.summary());
    cfg::Step noSec;
    noSec.action = "sleep";
    ok(noSec.summary() == "Wait 1s", "handles a sleep step with no value",
       noSec.summary());

    // Summaries are drawn into list controls, so config text must not be able
    // to make one arbitrarily long.
    cfg::Step huge;
    huge.action = "keys";
    huge.params["combo"] = std::string(5000, 'x');
    ok(huge.summary().size() <= 128, "caps a summary built from a huge field",
       std::to_string(huge.summary().size()));
    cfg::Step hugePath;
    hugePath.action = "click_image";
    hugePath.params["image"] = std::string(5000, 'y') + ".png";
    ok(hugePath.summary().size() <= 128, "caps an image step summary",
       std::to_string(hugePath.summary().size()));
    cfg::Step hugeLayout;
    hugeLayout.action = "window_save";
    hugeLayout.params["layout"] = std::string(5000, 'z');
    ok(hugeLayout.summary().size() <= 128, "caps a layout step summary",
       std::to_string(hugeLayout.summary().size()));
}

// ------------------------------------------------- selective key blocking
// blockAllKeys off means "take only the keys that run a macro and let the rest
// of the keyboard type normally" - the setting that makes it safe to put a
// macro on F13 of the keyboard you actually type on.
static void testSelectiveBlocking() {
    section("Which keys get taken from other programs");

    const std::string devPath =
        "\\\\?\\HID#VID_1234&PID_5678&MI_00#9&abc#{guid}";
    const std::string otherDev =
        "\\\\?\\HID#VID_9999&PID_0000&MI_00#9&xyz#{guid}";

    const int vkF13 = keys::toVk("f13");
    const int vkA = keys::toVk("a");
    const int vkNum1 = keys::toVk("num1");
    ok(vkF13 && vkA && vkNum1, "the keys this section needs all resolve");

    cfg::Config c;
    cfg::Keyboard kb;
    kb.alias = "Typing keyboard";
    kb.match = "VID_1234&PID_5678";
    kb.block = true;
    kb.blockAllKeys = true;

    cfg::Binding bound;          // f13 runs something
    bound.key = "f13";
    cfg::Step st;
    st.action = "type";
    st.params["text"] = "hi";
    bound.steps.push_back(st);
    kb.bindings.push_back(bound);

    cfg::Binding empty;          // num1 is a tile with no steps yet
    empty.key = "num1";
    kb.bindings.push_back(empty);

    c.keyboards.push_back(kb);
    cfg::Keyboard& live = c.keyboards[0];

    ok(live.blockAllKeys, "a keyboard blocks every key by default");

    // ---- blockAllKeys on: the whole keyboard goes quiet
    ok(c.blockedForKey(devPath, vkF13), "all-keys mode takes the macro key");
    ok(c.blockedForKey(devPath, vkA),
       "all-keys mode takes an ordinary letter too");
    ok(c.blockedForKey(devPath, vkNum1),
       "all-keys mode takes a key whose binding has no steps");

    // ---- blockAllKeys off: only keys carrying a macro
    live.blockAllKeys = false;
    ok(c.blockedForKey(devPath, vkF13),
       "selective mode still takes the key that runs a macro");
    ok(!c.blockedForKey(devPath, vkA),
       "selective mode lets an ordinary letter type normally");
    ok(!c.blockedForKey(devPath, vkNum1),
       "selective mode ignores a binding with no steps");

    // ---- the master switch still wins
    live.block = false;
    ok(!c.blockedForKey(devPath, vkF13),
       "a keyboard that blocks nothing keeps its macro key typing");
    live.block = true;

    // ---- other keyboards are never touched
    ok(!c.blockedForKey(otherDev, vkF13),
       "a keyboard we were never told about is left alone");
    ok(!c.blockedForKey("", vkF13), "an empty device path blocks nothing");

    // ---- blockedForDevice stays coarse: it only decides whether the hook is
    // worth installing, so it must say yes even in selective mode.
    live.blockAllKeys = false;
    ok(c.blockedForDevice(devPath),
       "selective mode still reports the device as blocking, so the hook runs");

    // ---- round trip
    std::string path = appPath("selftest-blocking.json");
    c.path = path;
    std::string err;
    ok(c.save(&err), "saves the blocking settings", err);
    cfg::Config reloaded;
    reloaded.load(path);
    ok(!reloaded.keyboards.empty() && !reloaded.keyboards[0].blockAllKeys,
       "selective mode survives save and load");
    ok(!reloaded.blockedForKey(devPath, vkA),
       "and still lets ordinary keys through after reloading");

    // ---- a file written before this option existed must not change how the
    // user's keyboard behaves: no blockAllKeys means the old all-keys meaning.
    writeFileAtomic(path,
                    "{\"settings\":{},\"keyboards\":[{\"alias\":\"Old\","
                    "\"match\":\"VID_1234&PID_5678\",\"block\":true,"
                    "\"bindings\":[]}]}");
    cfg::Config old;
    old.load(path);
    ok(old.loadError.empty(), "reads a config written before this option",
       old.loadError);
    ok(!old.keyboards.empty() && old.keyboards[0].blockAllKeys,
       "a config with no blockAllKeys still blocks every key");
    ok(old.blockedForKey(devPath, vkA),
       "so an old config keeps taking the whole keyboard");

    DeleteFileW(widen(path).c_str());
}

// --------------------------------------------------------- yaml import
static void testYamlImport(const std::string& yamlPath) {
    section("macrokb YAML import");

    if (!fileExists(yamlPath)) {
        std::printf("  [skip] %s not found\n", yamlPath.c_str());
        return;
    }

    cfg::Config c;
    std::string err;
    bool okImport = cfg::importMacroKbYaml(yamlPath, c, &err);
    ok(okImport, "imports the real macros.yaml", err);
    if (!okImport) return;

    ok(!c.keyboards.empty(), "found at least one keyboard");
    if (c.keyboards.empty()) return;

    const cfg::Keyboard& kb = c.keyboards[0];
    ok(!kb.match.empty(), "keyboard has a match string: " + kb.match);
    ok(!kb.bindings.empty(),
       "imported bindings: " + std::to_string(kb.bindings.size()));

    int totalSteps = 0, badActions = 0;
    bool foundType = false, foundMultiStep = false;
    for (const auto& b : kb.bindings) {
        totalSteps += (int)b.steps.size();
        if (b.steps.size() > 1) foundMultiStep = true;
        for (const auto& s : b.steps) {
            if (!cfg::findAction(s.action)) ++badActions;
            if (s.action == "type" && !s.params.str("text").empty())
                foundType = true;
        }
    }
    ok(badActions == 0, "every imported step is a known action",
       std::to_string(badActions) + " unknown");
    ok(totalSteps > 0, "imported " + std::to_string(totalSteps) + " steps");
    ok(foundType, "imported a 'type' step with its text intact");
    ok(foundMultiStep, "imported a multi-step macro");
    ok(c.settings.matchConfidence > 0 && c.settings.matchConfidence <= 1,
       "imported a sensible match confidence");

    for (const auto& b : kb.bindings)
        if (b.key == "d") {
            ok(b.steps.size() == 3, "the 'd' macro kept all 3 steps",
               std::to_string(b.steps.size()));
            if (b.steps.size() == 3) {
                ok(b.steps[0].action == "run" &&
                       b.steps[0].params.str("cmd") == "notepad.exe",
                   "step order and values survived");
                ok(std::fabs(b.steps[1].params.num("seconds") - 0.8) < 1e-9,
                   "fractional seconds survived");
                ok(b.steps[2].params.integer("w") == 900,
                   "integer window size survived");
            }
        }
}

// --------------------------------------------------------------- vision
static void testVision() {
    section("Image matching");

    vis::Capture cap;
    std::string err;
    bool grabbed = vis::captureScreen(cap, &err);
    ok(grabbed, "captures the screen", err);
    if (!grabbed) return;
    std::printf("         (screen %dx%d at %d,%d)\n", cap.width, cap.height,
                cap.originX, cap.originY);

    // Take a second capture so we can tell which parts of the screen are
    // actually holding still. Cropping the template out of one frame and then
    // searching a later frame only proves anything if the region did not
    // change in between - a clock or a caret in the patch makes the "expected"
    // position meaningless.
    Sleep(400);
    vis::Capture cap2;
    if (!vis::captureScreen(cap2, &err) || cap2.width != cap.width ||
        cap2.height != cap.height) {
        std::printf("  [skip] could not take a second screen capture\n");
        return;
    }

    // Find a 120x60 patch that is (a) identical in both captures and (b) has
    // structure along BOTH axes.
    //
    // (b) matters more than it looks. Correlation is invariant to brightness
    // and contrast, so a patch that only varies vertically - a toolbar band, a
    // window edge - correlates perfectly at *every* horizontal offset. Its
    // position genuinely is not unique, and demanding an exact x would be
    // testing something that isn't true. Scoring on the weaker of the two axes
    // picks a patch whose position the matcher can actually pin down.
    int bestX = -1, bestY = -1;
    double bestScore = -1, bestCol = 0, bestRow = 0;
    for (int y = 0; y + 60 < cap.height; y += 60)
        for (int x = 0; x + 120 < cap.width; x += 120) {
            bool stable = true;
            for (int r = 0; r < 60 && stable; ++r) {
                const uint32_t* a = &cap.bgra[(size_t)(y + r) * cap.width + x];
                const uint32_t* b = &cap2.bgra[(size_t)(y + r) * cap.width + x];
                if (memcmp(a, b, 120 * sizeof(uint32_t)) != 0) stable = false;
            }
            if (!stable) continue;

            double colSum[120] = {0}, rowSum[60] = {0};
            for (int r = 0; r < 60; ++r)
                for (int c = 0; c < 120; ++c) {
                    uint32_t p = cap.bgra[(size_t)(y + r) * cap.width + x + c];
                    double v = 0.299 * ((p >> 16) & 0xFF) +
                               0.587 * ((p >> 8) & 0xFF) + 0.114 * (p & 0xFF);
                    colSum[c] += v;
                    rowSum[r] += v;
                }
            auto variance = [](const double* vals, int n, double perCell) {
                double s = 0, s2 = 0;
                for (int i = 0; i < n; ++i) {
                    double m = vals[i] / perCell;
                    s += m;
                    s2 += m * m;
                }
                return s2 / n - (s / n) * (s / n);
            };
            double colVar = variance(colSum, 120, 60);   // horizontal detail
            double rowVar = variance(rowSum, 60, 120);   // vertical detail
            double score = std::min(colVar, rowVar);
            if (score > bestScore) {
                bestScore = score;
                bestCol = colVar;
                bestRow = rowVar;
                bestX = x;
                bestY = y;
            }
        }

    if (bestX < 0 || bestScore < 20) {
        std::printf(
            "  [skip] nothing on screen is both still and detailed in two "
            "directions (best score %.1f)\n", bestScore);
        return;
    }
    std::printf(
        "         (stable template from %d,%d - detail across %.0f, down %.0f)\n",
        bestX, bestY, bestCol, bestRow);

    RECT crop{bestX, bestY, bestX + 120, bestY + 60};
    ok(vis::savePng("selftest-template.png", cap, crop, &err),
       "saves a PNG template", err);

    int tw = 0, th = 0;
    ok(vis::imageSize("selftest-template.png", tw, th) && tw == 120 && th == 60,
       "the saved template is the requested size",
       std::to_string(tw) + "x" + std::to_string(th));

    const int wantX = cap.originX + bestX + 60;
    const int wantY = cap.originY + bestY + 30;

    // Searching a window around the known location is unambiguous, so the
    // result must be pixel-exact.
    RECT around{bestX - 40, bestY - 40, bestX + 160, bestY + 100};
    if (around.left < 0) around.left = 0;
    if (around.top < 0) around.top = 0;
    vis::Match local;
    err.clear();
    bool localFound =
        vis::findImage("selftest-template.png", 0.85, &around, local, &err);
    ok(localFound, "finds the template inside a search region", err);
    if (localFound) {
        std::printf("         (region search: %d,%d conf %.4f - expected %d,%d)\n",
                    local.x, local.y, local.confidence, wantX, wantY);
        ok(std::abs(local.x - wantX) <= 1 && std::abs(local.y - wantY) <= 1,
           "the region match is pixel-exact");
        ok(local.confidence > 0.99, "the region match is near-perfect");
    }

    // Full-screen search. The screen may legitimately contain several
    // near-identical copies of a small patch (toolbar icons, list rows), so
    // the requirement is that the true position is among the matches - and
    // that the best one is not beaten by a worse-scoring lookalike.
    err.clear();
    auto all = vis::findAll("selftest-template.png", 0.85, nullptr, 10, &err);
    ok(!all.empty(), "finds the template on the whole screen", err);
    if (!all.empty()) {
        std::printf("         (%d match(es); best %d,%d conf %.4f - expected %d,%d)\n",
                    (int)all.size(), all[0].x, all[0].y, all[0].confidence,
                    wantX, wantY);
        bool hit = false;
        for (const auto& mm2 : all)
            if (std::abs(mm2.x - wantX) <= 1 && std::abs(mm2.y - wantY) <= 1)
                hit = true;
        ok(hit, "the true position is among the matches");
        ok(all[0].confidence > 0.99, "the best match is near-perfect",
           std::to_string(all[0].confidence));
        for (size_t i = 1; i < all.size(); ++i)
            if (all[i].confidence > all[i - 1].confidence + 1e-9) {
                ok(false, "matches are sorted best first");
                break;
            }
    }

    // A blank template correlates perfectly everywhere, which would report a
    // meaningless "match". The matcher must refuse it instead.
    vis::Capture flat;
    flat.width = 120;
    flat.height = 60;
    flat.bgra.assign(120 * 60, 0xFF808080u);
    ok(vis::savePng("selftest-flat.png", flat, RECT{0, 0, 120, 60}, &err),
       "saves a flat template", err);
    err.clear();
    vis::Match fm;
    bool flatFound = vis::findImage("selftest-flat.png", 0.85, nullptr, fm, &err);
    ok(!flatFound && !err.empty(), "refuses a featureless template", err);
    std::printf("         (refusal message: %s)\n", err.c_str());

    // Missing files must fail fast rather than burn the whole timeout.
    err.clear();
    vis::Match mm;
    DWORD t0 = GetTickCount();
    bool missing = vis::waitForImage("does-not-exist.png", 5.0, 0.85, nullptr,
                                     mm, &err);
    DWORD elapsed = GetTickCount() - t0;
    ok(!missing && elapsed < 1500,
       "a missing template fails immediately instead of waiting",
       std::to_string(elapsed) + " ms");

    DeleteFileW(widen(appPath("selftest-template.png")).c_str());
    DeleteFileW(widen(appPath("selftest-flat.png")).c_str());
}

// -------------------------------------------------------------- windows
static void testWindows() {
    section("Window management");

    auto wins = win::listWindows();
    ok(!wins.empty(), "lists open windows (" + std::to_string(wins.size()) + ")");

    int withExe = 0, withTitle = 0, withClass = 0;
    for (const auto& w : wins) {
        if (!w.exe.empty()) ++withExe;
        if (!w.title.empty()) ++withTitle;
        if (!w.cls.empty()) ++withClass;
    }
    ok(withExe > 0, "resolves process names (" + std::to_string(withExe) + ")");
    ok(withTitle > 0, "reads window titles");
    ok(withClass == (int)wins.size(),
       "every window reports a class (" + std::to_string(withClass) + "/" +
           std::to_string(wins.size()) + ")");

    win::WindowInfo scratch;
    if (!wins.empty()) {
        std::printf("         (e.g. %s [%s] - \"%s\" %dx%d)\n",
                    wins[0].exe.c_str(), wins[0].cls.c_str(),
                    wins[0].title.substr(0, 32).c_str(), wins[0].w, wins[0].h);
        win::WindowInfo found;
        bool byExe = win::findWindow("", wins[0].exe, "", found);
        ok(byExe, "finds a window by program name");
        std::string part = wins[0].title.substr(0, std::min<size_t>(6, wins[0].title.size()));
        if (!part.empty())
            ok(win::findWindow(part, "", "", found), "finds a window by title");
        ok(win::findWindow("", "", wins[0].cls, found) &&
               found.cls == wins[0].cls,
           "finds a window by class");
        ok(!win::findWindow("", "", "NoSuchWindowClassZZZ", found),
           "does not match an unknown class");

        // describeWindow must agree with what the enumeration reported.
        win::WindowInfo described;
        ok(win::describeWindow(wins[0].hwnd, described) &&
               described.cls == wins[0].cls && described.exe == wins[0].exe,
           "describes a specific window handle");
    }
    ok(!win::findWindow("no such window anywhere zzz", "", "", scratch),
       "reports when nothing matches");
    ok(!win::describeWindow(nullptr, scratch), "rejects a null window handle");

    std::string err;
    ok(win::saveLayout("selftest", "layouts", &err), "saves a layout", err);
    auto layouts = win::listLayouts("layouts");
    bool listed = false;
    for (const auto& l : layouts)
        if (l == "selftest") listed = true;
    ok(listed, "the saved layout is listed");

    err.clear();
    int restored = win::restoreLayout("selftest", "layouts", &err);
    ok(restored > 0, "restores the layout (" + std::to_string(restored) +
                         " windows)", err);

    err.clear();
    win::restoreLayout("no-such-layout", "layouts", &err);
    ok(!err.empty(), "reports a missing layout");

    ok(win::deleteLayout("selftest", "layouts"), "removes the test layout");
}

// ----------------------------------------------------------------- util
static void testUtil() {
    section("Utilities");

    ok(narrow(widen("caf\xC3\xA9 \xE2\x86\x92")) == "caf\xC3\xA9 \xE2\x86\x92",
       "UTF-8 survives a widen/narrow round trip");
    ok(trim("  hi  ") == "hi", "trims whitespace");
    ok(lower("ABC") == "abc", "lowercases");
    ok(iequals("Abc", "aBC"), "compares case-insensitively");
    ok(icontains("Hello World", "lo wo"), "finds substrings insensitively");
    ok(baseName("C:\\a\\b\\c.txt") == "c.txt", "takes a base name");
    ok(stripExt("c.txt") == "c", "strips an extension");
    ok(stripExt("C:\\a.b\\c") == "C:\\a.b\\c",
       "does not strip a dot from a directory");
    ok(replaceAll("aXbXc", "X", "--") == "a--b--c", "replaces all");
    ok(split("a,b,,c", ',').size() == 4, "splits keeping empties");
    ok(joinPath("C:\\a", "b") == "C:\\a\\b", "joins paths");
    ok(joinPath("C:\\a", "D:\\abs") == "D:\\abs", "keeps absolute paths");

    std::string p = appPath("selftest-io.txt");
    std::string payload = "line one\nline \"two\"\n";
    ok(writeFileAtomic(p, payload), "writes a file atomically");
    std::string back;
    ok(readFile(p, back) && back == payload, "reads it back byte for byte");
    ok(writeFileAtomic(p, "\xEF\xBB\xBFwith bom"), "writes a file with a BOM");
    ok(readFile(p, back) && back == "with bom", "strips a UTF-8 BOM on read");

    // Telling "no file" apart from "cannot open the file" is what stops the
    // app from mistaking a locked config for a fresh install and writing over
    // it, so it gets its own checks.
    ok(readFileEx(p, back) == ReadResult::Ok, "readFileEx reports Ok");
    {
        HANDLE lock = CreateFileW(widen(p).c_str(), GENERIC_READ,
                                  0 /* no sharing */, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        ok(lock != INVALID_HANDLE_VALUE, "can take an exclusive lock");
        ok(readFileEx(p, back) == ReadResult::Unreadable,
           "a locked file reads as Unreadable, not Missing");
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    }

    // backup naming never reuses a name
    std::string b1 = freeBackupPath(p);
    ok(b1 == p + ".bak", "first backup is .bak", b1);
    ok(writeFileAtomic(b1, "old"), "writes a backup");
    std::string b2 = freeBackupPath(p);
    ok(b2 == p + ".bak.2", "second backup does not overwrite the first", b2);
    ok(copyFile(p, b2, false), "copies a file");
    ok(fileExists(b2), "the copy exists");
    ok(!copyFile(p, b2, false), "refuses to clobber without overwrite");
    deleteFile(b1);
    deleteFile(b2);

    deleteFile(p);
    ok(!readFile(p, back), "reports a missing file");
    ok(readFileEx(p, back) == ReadResult::Missing, "readFileEx reports Missing");
}

// ------------------------------------------------- config safety net
static void testConfigRescue() {
    section("Config safety");

    const std::string path = appPath("selftest-rescue.json");
    deleteFile(path);
    deleteFile(path + ".bak");

    // A file we cannot parse must survive the next save.
    ok(writeFileAtomic(path, "{ \"keyboards\": [ broken "),
       "writes a damaged config");
    cfg::Config c;
    c.load(path);
    ok(!c.loadError.empty(), "reports the damage");
    ok(c.needsRescue, "flags the file as needing rescue");

    cfg::Keyboard kb;
    kb.alias = "Pad";
    kb.match = "VID_1111&PID_2222";
    c.keyboards.push_back(kb);

    std::string err;
    ok(c.save(&err), "saves over it", err);
    ok(!c.needsRescue, "clears the rescue flag once saved");
    ok(fileExists(path + ".bak"), "kept the damaged file as .bak");
    std::string rescued;
    ok(readFile(path + ".bak", rescued) &&
           rescued.find("broken") != std::string::npos,
       "the .bak holds the original bytes");
    ok(c.lastBackup == path + ".bak", "reports where the copy went",
       c.lastBackup);

    cfg::Config again;
    again.load(path);
    ok(again.loadError.empty() && again.keyboards.size() == 1,
       "the new file loads cleanly");
    ok(!again.needsRescue, "a healthy file needs no rescue");

    // Duplicate keys collapse to one, keeping the copy that has steps.
    const std::string dup = appPath("selftest-dup.json");
    ok(writeFileAtomic(dup, R"({
        "keyboards": [{
          "alias": "Pad", "match": "VID_1&PID_2",
          "bindings": [
            {"key": "num1", "steps": []},
            {"key": "num1", "title": "real", "steps": [{"do":"type","text":"hi"}]},
            {"key": "notakey", "steps": [{"do":"type","text":"x"}]}
          ]
        }]
      })"),
       "writes a config with a duplicate key");
    cfg::Config d;
    d.load(dup);
    ok(d.loadError.empty(), "still loads", d.loadError);
    ok(d.keyboards.size() == 1 && d.keyboards[0].bindings.size() == 2,
       "collapses the duplicate to one binding");
    if (!d.keyboards.empty() && !d.keyboards[0].bindings.empty()) {
        const cfg::Binding* n1 = d.keyboards[0].findKey("num1");
        ok(n1 && n1->steps.size() == 1 && n1->title == "real",
           "keeps the copy that actually has steps");
    }
    bool warnedDup = false, warnedKey = false;
    for (const std::string& w : d.loadWarnings) {
        if (w.find("more than once") != std::string::npos) warnedDup = true;
        if (w.find("notakey") != std::string::npos) warnedKey = true;
    }
    ok(warnedDup, "warns about the duplicate");
    ok(warnedKey, "warns about a key name it does not know");

    // Confidence outside the range the settings dialog accepts is reset.
    const std::string conf = appPath("selftest-conf.json");
    ok(writeFileAtomic(conf, R"({"settings":{"matchConfidence":0.2}})"),
       "writes an out-of-range confidence");
    cfg::Config cc;
    cc.load(conf);
    ok(std::fabs(cc.settings.matchConfidence - cfg::kDefaultConfidence) < 1e-9,
       "resets confidence below the allowed range");
    ok(!cc.loadWarnings.empty(), "says so in a warning");

    // Round-tripping the start/stop shortcut.
    cc.path = conf;
    cc.settings.toggleHotkey = "ctrl+alt+p";
    ok(cc.save(&err), "saves the shortcut", err);
    cfg::Config hk;
    hk.load(conf);
    ok(hk.settings.toggleHotkey == "ctrl+alt+p", "reloads the shortcut",
       hk.settings.toggleHotkey);

    // The always-on-top palette: where it sat has to survive a restart, or it
    // reappears in a corner every morning and stops being worth opening.
    cfg::Config def;
    ok(!def.settings.miniDeckVisible, "the palette starts closed");
    ok(def.settings.miniDeckX == INT_MIN && def.settings.miniDeckY == INT_MIN,
       "with no remembered position");
    cc.settings.miniDeckVisible = true;
    cc.settings.miniDeckX = -1400;   // a monitor to the left of the primary
    cc.settings.miniDeckY = 220;
    cc.settings.miniDeckTile = 30;
    cc.settings.miniDeckCols = 8;
    ok(cc.save(&err), "saves the palette placement", err);
    cfg::Config md;
    md.load(conf);
    ok(md.settings.miniDeckVisible, "reopens if it was open");
    ok(md.settings.miniDeckX == -1400 && md.settings.miniDeckY == 220,
       "and comes back on a negative-coordinate monitor",
       std::to_string(md.settings.miniDeckX) + "," +
           std::to_string(md.settings.miniDeckY));
    ok(md.settings.miniDeckTile == 30 && md.settings.miniDeckCols == 8,
       "keeping its size and column count");

    // Hand-edited nonsense is clamped, not refused: the file still opens.
    const std::string silly = appPath("selftest-mini.json");
    ok(writeFileAtomic(
           silly,
           R"({"settings":{"miniDeckTile":900,"miniDeckCols":1}})"),
       "writes an impossible palette size");
    cfg::Config sc;
    sc.load(silly);
    ok(sc.settings.miniDeckTile <= 46 && sc.settings.miniDeckTile >= 30,
       "clamps a button size bigger than the screen",
       std::to_string(sc.settings.miniDeckTile));
    ok(sc.settings.miniDeckCols >= 2, "and a column count below two",
       std::to_string(sc.settings.miniDeckCols));
    deleteFile(silly);

    deleteFile(path);
    deleteFile(path + ".bak");
    deleteFile(dup);
    deleteFile(conf);
}

// ------------------------------------------------------- engine dispatch
// Only steps with no side effects are exercised here: a sleep, and failures
// that are rejected before anything is typed, clicked or launched. The point
// is the dispatch table and the error path, not synthetic input.
static void testEngine() {
    section("Engine");

    eng::Engine e;
    std::vector<std::string> logged;
    e.setLog([&](const std::string& m) { logged.push_back(m); });

    auto step = [](const std::string& action) {
        cfg::Step s;
        s.action = action;
        return s;
    };

    // a known action runs
    {
        cfg::Step s = step("sleep");
        s.params["seconds"] = 0.01;
        bool threw = false;
        try { e.runSequence({s}); } catch (const std::exception&) { threw = true; }
        ok(!threw, "runs a sleep step");
    }

    // an unknown action is reported, not ignored
    {
        bool threw = false;
        std::string msg;
        try { e.runSequence({step("no_such_action")}); }
        catch (const std::exception& ex) { threw = true; msg = ex.what(); }
        ok(threw && msg.find("unknown action") != std::string::npos,
           "rejects an unknown action", msg);
    }

    // a failing step names which step it was
    {
        bool threw = false;
        std::string msg;
        try { e.runSequence({step("sleep"), step("window_focus")}); }
        catch (const std::exception& ex) { threw = true; msg = ex.what(); }
        ok(threw && msg.find("step 2") != std::string::npos,
           "reports the failing step's number", msg);
    }

    // ...and stops the rest of the macro
    {
        cfg::Step marker = step("sleep");
        marker.params["seconds"] = 0.01;
        bool threw = false;
        try { e.runSequence({step("window_focus"), marker}); }
        catch (const std::exception&) { threw = true; }
        ok(threw, "a failed step ends the macro");
    }

    // unless the step says to keep going
    {
        cfg::Step skippable = step("window_focus");
        skippable.params[cfg::kContinueOnError] = true;
        cfg::Step after = step("sleep");
        after.params["seconds"] = 0.01;
        ok(skippable.continueOnError(), "continue-on-error reads back");
        logged.clear();
        bool threw = false;
        try { e.runSequence({skippable, after}); }
        catch (const std::exception&) { threw = true; }
        ok(!threw, "a step marked continue-on-error does not end the macro");
        bool noted = false;
        for (const std::string& m : logged)
            if (m.find("skipped") != std::string::npos) noted = true;
        ok(noted, "the skipped step is still logged");
    }

    // the flag survives a save/load round trip
    {
        const std::string p = appPath("selftest-continue.json");
        cfg::Config c;
        c.path = p;
        cfg::Keyboard kb;
        kb.alias = "Pad";
        kb.match = "VID_1&PID_2";
        cfg::Binding b;
        b.key = "num1";
        cfg::Step s = step("run");
        s.params["cmd"] = "whatever";
        s.params[cfg::kContinueOnError] = true;
        b.steps.push_back(s);
        kb.bindings.push_back(b);
        c.keyboards.push_back(kb);
        std::string err;
        ok(c.save(&err), "saves a continue-on-error step", err);
        cfg::Config back;
        back.load(p);
        const cfg::Binding* lb = back.keyboards.empty()
                                     ? nullptr
                                     : back.keyboards[0].findKey("num1");
        ok(lb && lb->steps.size() == 1 && lb->steps[0].continueOnError(),
           "continue-on-error survives save and load");
        deleteFile(p);
    }
}

// --------------------------------------------- recorded-click workflow
// The point of window_prepare / window_release: a recorded click is an
// absolute screen coordinate, so it only lands where it was recorded if the
// target window is put back to the size and position it had at record time.
// This drives that against a real window and checks it actually moves, and
// actually goes back.
namespace {

const wchar_t* kTestWndClass = L"MacroDeckSelfTestWindow";

void pump(int ms) {
    const DWORD until = GetTickCount() + (DWORD)ms;
    MSG m;
    while (GetTickCount() < until) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(5);
    }
}

HWND makeTestWindow() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kTestWndClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        if (!RegisterClassExW(&wc)) return nullptr;
        registered = true;
    }
    HWND h = CreateWindowExW(0, kTestWndClass, L"MacroDeck self-test window",
                             WS_OVERLAPPEDWINDOW, 100, 100, 400, 300, nullptr,
                             nullptr, GetModuleHandleW(nullptr), nullptr);
    if (h) {
        ShowWindow(h, SW_SHOWNOACTIVATE);
        pump(120);
    }
    return h;
}

RECT rectOf(HWND h) {
    RECT r{};
    GetWindowRect(h, &r);
    return r;
}

}  // namespace

// ------------------------------------------------------ built-in icons
// The drawing itself is checked by render_preview.exe, which has GDI+ up and
// can see whether an icon put any ink down. What matters here is the table
// and the "lib:" reference format that goes into macros.json.
// ------------------------------------------------ typing through the hook
// WH_KEYBOARD_LL is global and synchronous: every keystroke in the machine
// goes through it before the app being typed into sees it. The hook used to
// wait for the raw-input event on EVERY key so it could tell which physical
// keyboard the key came from. On a main keyboard carrying a handful of
// macros, that is a wait on very nearly every key typed all day.
static void testHookCost() {
    section("What the blocking hook costs a keystroke");

    // The wait, measured. Nothing is feeding this listener, so this is the
    // cost of a keystroke the hook cannot correlate - the bad case that made
    // typing feel laggy.
    dev::RawKeyboardListener idle;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string got = idle.findDevice('A', true, 30);
    const double stallMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0)
            .count();
    ok(got.empty(), "an uncorrelated keystroke finds no device");
    // The number is in the label, not the failure detail: a measurement is
    // worth seeing on a passing run too.
    ok(stallMs >= 20.0,
       "and only finds that out after stalling for " +
           std::to_string((int)stallMs) + "ms (timeout is 30ms)");

    // The fix: ask a question that needs no waiting at all first.
    cfg::Config c;
    cfg::Keyboard kb;
    kb.alias = "Main";
    kb.match = "VID_DEAD";
    kb.block = true;
    kb.blockAllKeys = false;      // "only macro keys blocked, the rest type"
    cfg::Binding macro;
    macro.key = "f13";
    macro.steps.push_back({"sleep", js::Value::object()});
    kb.bindings.push_back(macro);
    c.keyboards.push_back(kb);

    ok(c.couldBlockVk(keys::toVk("f13")),
       "a key carrying a macro still has to be looked at");
    ok(!c.couldBlockVk('A'), "but an ordinary letter is let straight through");
    ok(!c.couldBlockVk(VK_SPACE), "and so is the space bar");
    ok(!c.couldBlockVk(VK_RETURN), "and Enter");

    // A half-finished tile must not cost anything either: taking its key
    // would silence a key for no benefit.
    cfg::Binding empty;
    empty.key = "f14";
    c.keyboards[0].bindings.push_back(empty);
    ok(!c.couldBlockVk(keys::toVk("f14")),
       "a key with no steps on it is not worth stopping for");

    // Blocking everything on a device means every key must be correlated -
    // there is no shortcut available, and claiming one would break blocking.
    c.keyboards[0].blockAllKeys = true;
    ok(c.couldBlockVk('A'),
       "with every key on a device blocked, no key can be shortcut");

    // A keyboard that is not blocking costs nothing whatever it carries.
    c.keyboards[0].block = false;
    ok(!c.couldBlockVk('A') && !c.couldBlockVk(keys::toVk("f13")),
       "a keyboard that blocks nothing never reaches the hook at all");
}

// -------------------------------------------------- buttons with no keyboard
static void testSoftwareDeck() {
    section("Software buttons, with no keyboard behind them");

    cfg::Config c;
    cfg::Keyboard sw;
    sw.alias = "Buttons";
    sw.softwareOnly = true;
    sw.block = false;
    sw.match.clear();
    cfg::Binding b;
    b.key = "btn1";              // an id, not a key name
    b.title = "Print the day sheet";
    b.steps.push_back({"sleep", js::Value::object()});
    sw.bindings.push_back(b);
    c.keyboards.push_back(sw);

    // The whole promise: it cannot cost a keystroke, because it matches no
    // device and nothing it contains reaches the hook.
    ok(!c.blockedForDevice("\\?\HID#VID_046D&PID_C53D"),
       "a software deck claims no device");
    ok(!c.couldBlockVk('A') && !c.couldBlockVk(VK_RETURN),
       "and puts no key into the hook's shortcut table");
    ok(c.keyboardForDevice("\\?\HID#VID_046D&PID_C53D") == nullptr,
       "no device resolves to it");
    ok(!cfg::specMatches("", "\\?\HID#VID_046D&PID_C53D"),
       "an empty match string matches nothing at all");

    // Round trip, including the flag and the opaque key id.
    const std::string path = appPath("selftest-sw.json");
    std::string err;
    ok(c.saveAs(path, &err), "a software deck saves", err);
    cfg::Config back;
    back.load(path);
    ok(back.keyboards.size() == 1 && back.keyboards[0].softwareOnly,
       "and comes back marked software-only");
    ok(!back.keyboards[0].bindings.empty() &&
           back.keyboards[0].bindings[0].key == "btn1",
       "keeping the button id");
    ok(back.keyboards[0].bindings[0].title == "Print the day sheet",
       "and its label");
    // "btn1" is not a key name, and must not be complained about as one.
    bool moaned = false;
    for (const std::string& w : back.loadWarnings)
        if (w.find("btn1") != std::string::npos) moaned = true;
    ok(!moaned, "and no warning that btn1 is not a real key");
    deleteFile(path);

    // A file that somehow carries block on a software deck must not be able to
    // install the hook.
    const std::string bad = appPath("selftest-sw2.json");
    ok(writeFileAtomic(
           bad,
           R"({"keyboards":[{"alias":"B","softwareOnly":true,"block":true,)"
           R"("blockAllKeys":true,"match":"VID_9999","bindings":[]}]})"),
       "writes a software deck that claims to block");
    cfg::Config forced;
    forced.load(bad);
    ok(!forced.keyboards.empty() && !forced.keyboards[0].block,
       "loading it turns the blocking off");
    ok(forced.keyboards[0].match.empty(), "and drops the device match");
    ok(!forced.couldBlockVk('A'), "so it still cannot cost a keystroke");
    deleteFile(bad);
}

static void testIconLibrary() {
    section("Built-in icon library");
    const auto& all = ui::iconLibrary();
    ok(!all.empty(), "the library has icons in it", std::to_string(all.size()));

    bool named = true, dupes = false;
    std::vector<std::string> seen;
    for (const ui::LibIcon& i : all) {
        if (!i.id || !*i.id || !i.label || !*i.label || !i.category ||
            !*i.category)
            named = false;
        std::string id = i.id;
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) dupes = true;
        seen.push_back(id);
    }
    ok(named, "every icon has an id, a label and a category");
    ok(!dupes, "no id appears twice");
    ok(ui::iconCategories().size() >= 2, "icons are grouped into categories",
       std::to_string(ui::iconCategories().size()));

    // The reference format that ends up in the config file.
    const std::string ref = ui::libraryIconRef("printer");
    ok(ref == "lib:printer", "a library icon is written as lib:<id>", ref);
    ok(ui::isLibraryIcon(ref), "and recognised on the way back in");
    ok(ui::libraryIconId(ref) == "printer", "and the id comes back out",
       ui::libraryIconId(ref));
    ok(ui::libraryHasIcon("printer"), "a known id is known");
    ok(!ui::libraryHasIcon("no_such_icon"),
       "an id from some other build is not");

    // The whole point of the prefix: an icon field is still a file path when
    // it does not carry one, so decks built before the library keep working.
    ok(!ui::isLibraryIcon("icons/slack.png"), "a plain path is not a library icon");
    ok(!ui::isLibraryIcon(""), "and neither is an empty field");
    ok(ui::libraryIconId("icons/slack.png").empty(),
       "asking a path for its library id gets nothing");

    // Round trip through the config file, since this shares the `icon` field.
    cfg::Config c;
    c.keyboards.push_back({});
    c.keyboards[0].alias = "Pad";
    c.keyboards[0].match = "X";
    cfg::Binding b;
    b.key = "num1";
    b.icon = ref;
    b.steps.push_back({"sleep", js::Value::object()});
    c.keyboards[0].bindings.push_back(b);
    const std::string path = appPath("selftest-icons.json");
    std::string err;
    ok(c.saveAs(path, &err), "a deck using a library icon saves", err);
    cfg::Config back;
    back.load(path);
    ok(!back.keyboards.empty() && !back.keyboards[0].bindings.empty() &&
           back.keyboards[0].bindings[0].icon == ref,
       "and the icon reference survives the round trip");
    deleteFile(path);
}

static void testClickWorkflow() {
    section("Recorded-click workflow");

    // --- the actions exist and carry the fields the recorder writes
    const cfg::ActionDef* prep = cfg::findAction("window_prepare");
    const cfg::ActionDef* rel = cfg::findAction("window_release");
    ok(prep != nullptr, "window_prepare is a known action");
    ok(rel != nullptr, "window_release is a known action");
    if (prep) {
        auto hasField = [&](const char* k) {
            for (const auto& f : prep->fields)
                if (std::string(f.key) == k) return true;
            return false;
        };
        ok(hasField("class") && hasField("x") && hasField("y") &&
               hasField("w") && hasField("h"),
           "window_prepare takes a class and a rectangle");
        ok(hasField("block_input") && hasField("settle_ms") &&
               hasField("activate_timeout"),
           "window_prepare takes the timing and input-blocking options");
    }

    HWND h = makeTestWindow();
    if (!h) {
        std::printf("  [skip] could not create a test window\n");
        return;
    }
    const RECT before = rectOf(h);
    ok(before.right - before.left > 0, "test window is on screen");

    eng::Engine e;
    std::vector<std::string> logged;
    e.setLog([&](const std::string& m) { logged.push_back(m); });

    auto prepareStep = [&](int x, int y, int w, int ht) {
        cfg::Step s;
        s.action = "window_prepare";
        s.params = js::Value::object();
        s.params["class"] = narrow(kTestWndClass);
        s.params["x"] = x;
        s.params["y"] = y;
        s.params["w"] = w;
        s.params["h"] = ht;
        // Never block input from the test suite: if anything below threw
        // before the release, the machine's keyboard and mouse would be dead.
        s.params["block_input"] = false;
        s.params["settle_ms"] = 0;
        s.params["activate_timeout"] = 0.2;
        return s;
    };
    cfg::Step releaseStep;
    releaseStep.action = "window_release";
    releaseStep.params = js::Value::object();
    releaseStep.params["unblock_input"] = false;

    // --- prepare moves the window to the recorded rectangle
    const int wantX = before.left + 60, wantY = before.top + 40;
    const int wantW = 520, wantH = 360;
    bool threw = false;
    try { e.runSequence({prepareStep(wantX, wantY, wantW, wantH)}); }
    catch (const std::exception& ex) { threw = true; std::printf("  (%s)\n", ex.what()); }
    ok(!threw, "window_prepare runs against a real window");
    pump(80);

    RECT moved = rectOf(h);
    ok(moved.left == wantX && moved.top == wantY,
       "moves the window to the recorded position",
       std::to_string(moved.left) + "," + std::to_string(moved.top) +
           " wanted " + std::to_string(wantX) + "," + std::to_string(wantY));
    ok(moved.right - moved.left == wantW && moved.bottom - moved.top == wantH,
       "resizes the window to the recorded size",
       std::to_string(moved.right - moved.left) + "x" +
           std::to_string(moved.bottom - moved.top));

    // --- release puts it back exactly where it was
    try { e.runSequence({releaseStep}); }
    catch (const std::exception&) { threw = true; }
    pump(80);
    RECT after = rectOf(h);
    ok(after.left == before.left && after.top == before.top &&
           after.right == before.right && after.bottom == before.bottom,
       "window_release restores the original position and size",
       std::to_string(after.left) + "," + std::to_string(after.top) + " " +
           std::to_string(after.right - after.left) + "x" +
           std::to_string(after.bottom - after.top));

    // --- a maximised window is un-maximised to the recorded size, then
    //     put back to maximised afterwards
    ShowWindow(h, SW_MAXIMIZE);
    pump(120);
    ok(IsZoomed(h) != 0, "test window is maximised");
    try { e.runSequence({prepareStep(wantX, wantY, wantW, wantH)}); }
    catch (const std::exception&) {}
    pump(80);
    ok(IsZoomed(h) == 0, "un-maximises before moving");
    RECT m2 = rectOf(h);
    ok(m2.right - m2.left == wantW && m2.bottom - m2.top == wantH,
       "reaches the recorded size from maximised");
    try { e.runSequence({releaseStep}); }
    catch (const std::exception&) {}
    pump(120);
    ok(IsZoomed(h) != 0, "restores the maximised state afterwards");
    ShowWindow(h, SW_RESTORE);
    pump(60);

    // --- a window that is not there is a clear failure, not a silent no-op
    {
        cfg::Step missing = prepareStep(0, 0, 100, 100);
        missing.params["class"] = "NoSuchWindowClassZZZ";
        std::string msg;
        threw = false;
        try { e.runSequence({missing}); }
        catch (const std::exception& ex) { threw = true; msg = ex.what(); }
        ok(threw && msg.find("class") != std::string::npos,
           "a missing target window fails loudly", msg);
    }

    // --- the whole recorded shape survives a save/load round trip
    {
        const std::string p = appPath("selftest-clicks.json");
        cfg::Config c;
        c.path = p;
        cfg::Keyboard kb;
        kb.alias = "Pad";
        kb.match = "VID_1&PID_2";
        cfg::Binding b;
        b.key = "num1";
        b.steps.push_back(prepareStep(wantX, wantY, wantW, wantH));
        cfg::Step click;
        click.action = "click";
        click.params = js::Value::object();
        click.params["x"] = 640;
        click.params["y"] = 480;
        b.steps.push_back(click);
        cfg::Step wait;
        wait.action = "sleep";
        wait.params = js::Value::object();
        wait.params["seconds"] = 0.2;
        b.steps.push_back(wait);
        b.steps.push_back(releaseStep);
        kb.bindings.push_back(b);
        c.keyboards.push_back(kb);

        std::string err;
        ok(c.save(&err), "saves a recorded click sequence", err);
        cfg::Config back;
        back.load(p);
        const cfg::Binding* lb =
            back.keyboards.empty() ? nullptr : back.keyboards[0].findKey("num1");
        ok(lb && lb->steps.size() == 4, "reloads all four steps");
        if (lb && lb->steps.size() == 4) {
            ok(lb->steps[0].action == "window_prepare" &&
                   lb->steps[3].action == "window_release",
               "the clicks stay wrapped by prepare and release");
            ok(lb->steps[0].params.str("class") == narrow(kTestWndClass),
               "keeps the window class");
            ok(lb->steps[0].params.integer("w") == wantW &&
                   lb->steps[0].params.integer("h") == wantH,
               "keeps the recorded window size");
            ok(lb->steps[1].params.integer("x") == 640 &&
                   lb->steps[1].params.integer("y") == 480,
               "keeps the click position");
        }
        deleteFile(p);
    }

    // --- summaries stay readable in the step list
    {
        cfg::Step s = prepareStep(10, 20, 300, 200);
        const std::string sum = s.summary();
        ok(sum.find("Lock") == 0 && sum.find("300x200") != std::string::npos,
           "summarises a lock step", sum);
        ok(releaseStep.summary() == "Release the locked window",
           "summarises a release step", releaseStep.summary());
    }

    DestroyWindow(h);
    pump(40);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("MacroDeck self-test\n");
    std::printf("exe folder: %s\n", exeDir().c_str());

    std::string yaml = argc > 1 ? argv[1] : "";

    testUtil();
    testJson();
    testKeys();
    testPresets();
    testConfig();
    testSelectiveBlocking();
    testConfigRescue();
    testEngine();
    testClickWorkflow();
    testIconLibrary();
    testSoftwareDeck();
    testHookCost();
    if (!yaml.empty()) testYamlImport(yaml);
    testWindows();
    testVision();

    std::printf("\n=====================================\n");
    std::printf("  passed: %d    failed: %d\n", g_pass, g_fail);
    std::printf("=====================================\n");
    return g_fail == 0 ? 0 : 1;
}
