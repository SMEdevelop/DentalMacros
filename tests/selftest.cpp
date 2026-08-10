// MacroDeck self-test.
//
// Exercises every non-UI subsystem against the real machine: JSON, the config
// model, the macrokb YAML importer, key tables, deck presets, window
// management and the image matcher.
//
// Build and run with run_tests.bat. It deliberately does NOT link the
// blocking hook, so it stays a plain console program.

#include "config.h"
#include "engine.h"
#include "json.h"
#include "keys.h"
#include "util.h"
#include "vision.h"
#include "winman.h"

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

    int withExe = 0, withTitle = 0;
    for (const auto& w : wins) {
        if (!w.exe.empty()) ++withExe;
        if (!w.title.empty()) ++withTitle;
    }
    ok(withExe > 0, "resolves process names (" + std::to_string(withExe) + ")");
    ok(withTitle > 0, "reads window titles");

    if (!wins.empty()) {
        std::printf("         (e.g. %s - \"%s\" %dx%d)\n", wins[0].exe.c_str(),
                    wins[0].title.substr(0, 40).c_str(), wins[0].w, wins[0].h);
        win::WindowInfo found;
        bool byExe = win::findWindow("", wins[0].exe, found);
        ok(byExe, "finds a window by program name");
        std::string part = wins[0].title.substr(0, std::min<size_t>(6, wins[0].title.size()));
        if (!part.empty())
            ok(win::findWindow(part, "", found), "finds a window by title");
    }
    ok(!win::findWindow("no such window anywhere zzz", "", *(new win::WindowInfo())),
       "reports when nothing matches");

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
    testConfigRescue();
    testEngine();
    if (!yaml.empty()) testYamlImport(yaml);
    testWindows();
    testVision();

    std::printf("\n=====================================\n");
    std::printf("  passed: %d    failed: %d\n", g_pass, g_fail);
    std::printf("=====================================\n");
    return g_fail == 0 ? 0 : 1;
}
