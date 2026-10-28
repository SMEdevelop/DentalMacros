// Dental Macros self-test.
//
// Exercises every non-UI subsystem against the real machine: JSON, the config
// model, the macrokb YAML importer, key tables, deck presets, window
// management and the image matcher.
//
// Build and run with run_tests.bat. It deliberately does NOT link the
// blocking hook, so it stays a plain console program.

#include "config.h"
#include "device.h"
#include "actions.h"
#include "engine.h"
#include "friendly.h"
#include "iconlib.h"
#include "json.h"
#include "keys.h"
#include "recorder.h"
#include "simple.h"
#include "discover.h"
#include "tokens.h"
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

// ------------------------------------------------- plain-English problems
// Every failure the app knows how to explain: the wording, the kind of
// problem each failure is labelled with where it happens, and the step number
// that travels with it to the status line.
static void testFriendlyProblems() {
    section("Plain-English problems");

    // --- every problem has words, and only fixable ones offer a fix
    const fe::Problem all[] = {
        fe::Problem::ConfigDamaged, fe::Problem::ConfigLocked,
        fe::Problem::SaveDenied,    fe::Problem::SaveDiskFull,
        fe::Problem::SaveLocked,    fe::Problem::SaveFailed,
        fe::Problem::InputBlocked,  fe::Problem::TargetElevated,
        fe::Problem::InputRefused,  fe::Problem::WindowMissing,
        fe::Problem::ProgramMissing, fe::Problem::ImageNotFound,
        fe::Problem::ImageFileMissing, fe::Problem::Unexpected};
    bool worded = true, plain = true;
    for (fe::Problem p : all) {
        const fe::Friendly f = fe::explain(p, "OpenDental", "raw detail 0x5");
        if (f.title.empty() || f.body.empty()) worded = false;
        // The technical text goes to "Copy details", never into the sentence.
        if (f.body.find("0x5") != std::string::npos ||
            f.technical != "raw detail 0x5")
            plain = false;
    }
    ok(worded, "every problem has a title and a sentence");
    ok(plain, "and keeps the technical text out of the sentence");

    {
        const fe::Friendly withCopy =
            fe::explain(fe::Problem::ConfigDamaged, "25 October 2026");
        const fe::Friendly without = fe::explain(fe::Problem::ConfigDamaged);
        ok(!withCopy.action.empty() &&
               withCopy.body.find("25 October 2026") != std::string::npos,
           "a damaged file offers the backup by its date");
        ok(without.action.empty(), "and offers nothing when there is no backup");
        ok(fe::explain(fe::Problem::ConfigLocked).action == "Try again",
           "a locked file offers to try again");
    }
    {
        const fe::Friendly unknown =
            fe::explain(fe::Problem::None, "", "could not frob the widget");
        ok(unknown.body == "could not frob the widget" && unknown.technical.empty(),
           "an unforeseen failure shows its own text, once");
    }

    // --- save errors
    ok(fe::saveProblem(5) == fe::Problem::SaveDenied &&
           fe::saveProblem(112) == fe::Problem::SaveDiskFull &&
           fe::saveProblem(32) == fe::Problem::SaveLocked &&
           fe::saveProblem(1234) == fe::Problem::SaveFailed,
       "Windows save errors map to denied, full, locked and other");

    // --- the status line
    ok(fe::stoppedLine("Insurance note", 3, fe::Problem::WindowMissing,
                       "OpenDental", "") ==
           "\"Insurance note\" stopped at step 3: OpenDental isn't open.",
       "says which button, which step and why",
       fe::stoppedLine("Insurance note", 3, fe::Problem::WindowMissing,
                       "OpenDental", ""));
    ok(fe::stoppedLine("X", 0, fe::Problem::None, "", "no command given") ==
           "\"X\" stopped: no command given",
       "falls back to the step's own words");

    ok(fe::programName("C:\\Program Files\\Open Dental\\opendental.exe") ==
           "Opendental",
       "names a program from its path");
    ok(fe::friendlyDate("2026-10-05") == "5 October 2026" &&
           fe::friendlyDate("not a date") == "not a date",
       "writes dates out in words");

    // --- failures are labelled where they happen, and keep their step
    eng::Engine e;
    {
        cfg::Step wait;
        wait.action = "sleep";
        wait.params["seconds"] = 0.0;
        cfg::Step focus;
        focus.action = "window_focus";
        focus.params["exe"] = "no-such-program-dm.exe";
        fe::Problem got = fe::Problem::None;
        int step = 0;
        std::string subject, what;
        try { e.runSequence({wait, focus}); }
        catch (const fe::Error& ex) {
            got = ex.problem; step = ex.step; subject = ex.subject; what = ex.what();
        }
        ok(got == fe::Problem::WindowMissing && step == 2,
           "a program that isn't open is labelled, at its step");
        ok(subject == "No-such-program-dm", "names the program", subject);
        ok(what.find("step 2") != std::string::npos,
           "and the log text reads as before", what);
    }
    {
        cfg::Step find;
        find.action = "click_image";
        find.params["image"] = "images\\no-such-picture-dm.png";
        fe::Problem got = fe::Problem::None;
        try { e.runSequence({find}); }
        catch (const fe::Error& ex) { got = ex.problem; }
        ok(got == fe::Problem::ImageFileMissing,
           "a missing picture file is told apart from one not on screen");
    }
    {
        cfg::Step bad;
        bad.action = "no_such_action";
        fe::Problem got = fe::Problem::Unexpected;
        int step = 0;
        try { e.runSequence({bad}); }
        catch (const fe::Error& ex) { got = ex.problem; step = ex.step; }
        ok(got == fe::Problem::None && step == 1,
           "an unlabelled failure still says which step");
    }

    // --- runs report how they ended
    {
        std::mutex mu;
        std::vector<eng::Outcome> seen;
        e.setOutcome([&](const eng::Outcome& o) {
            std::lock_guard<std::mutex> lk(mu);
            seen.push_back(o);
        });
        cfg::Step wait;
        wait.action = "sleep";
        wait.params["seconds"] = 0.0;
        cfg::Step focus;
        focus.action = "window_focus";
        focus.params["exe"] = "no-such-program-dm.exe";
        e.runSequenceAsync({wait}, "Test", "", "Thank you");
        e.runSequenceAsync({wait, focus}, "Test", "", "Insurance note");
        for (int i = 0; i < 200; ++i) {
            {
                std::lock_guard<std::mutex> lk(mu);
                if (seen.size() >= 2) break;
            }
            Sleep(10);
        }
        std::lock_guard<std::mutex> lk(mu);
        ok(seen.size() == 2, "each run reports once",
           std::to_string(seen.size()));
        if (seen.size() == 2) {
            ok(seen[0].ok && seen[0].button == "Thank you",
               "a run that worked says so, by the button's name");
            ok(!seen[1].ok && seen[1].step == 2 &&
                   seen[1].problem == fe::Problem::WindowMissing,
               "a run that stopped says where and why");
        }
    }
    e.setOutcome(nullptr);

    // --- input that Windows would drop
    ok(act::blockedProgram(nullptr).empty() &&
           act::blockedProgram(GetDesktopWindow()).empty(),
       "nothing in the way of input to an ordinary window");

    // --- saving: the Windows error comes back, after the quiet retries
    {
        const std::string p = appPath("selftest-readonly.json");
        cfg::Config c;
        c.path = p;
        std::string err;
        ok(c.save(&err), "saves normally", err);
        SetFileAttributesW(widen(p).c_str(), FILE_ATTRIBUTE_READONLY);
        const DWORD t0 = GetTickCount();
        const bool saved = c.save(&err);
        const DWORD took = GetTickCount() - t0;
        ok(!saved && fe::saveProblem(c.lastSaveError) == fe::Problem::SaveDenied,
           "a file that refuses writes is reported as denied",
           std::to_string(c.lastSaveError) + " " + err);
        ok(took >= 300, "after a few quiet retries", std::to_string(took) + " ms");
        SetFileAttributesW(widen(p).c_str(), FILE_ATTRIBUTE_NORMAL);
        ok(c.save(&err) && c.lastSaveError == 0, "and saves again once it can");
        deleteFile(p);
    }

    // --- loading: a file held open is told apart from a damaged one
    {
        const std::string p = appPath("selftest-locked.json");
        writeFileAtomic(p, "{}");
        HANDLE h = CreateFileW(widen(p).c_str(), GENERIC_READ, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        cfg::Config c;
        c.load(p);
        ok(c.loadLocked && !c.loadError.empty(), "a file held open is locked");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        writeFileAtomic(p, "{ not json");
        c.load(p);
        ok(!c.loadLocked && !c.loadError.empty(),
           "a damaged file is not mistaken for a locked one");
        deleteFile(p);
    }
}

// ------------------------------------------------- where settings live
// Beside the program when that folder takes changes, in the Windows profile
// when it doesn't. The decision is tested against a pretend file system; the
// copying and the checks it relies on against the real one.
namespace dataprobe {
bool exeWritable = true;
bool profileWritable = true;
bool exeHasSettings = false;
bool settingsReadOnly = false;
bool canCreate(const std::string& dir) {
    return iequals(dir, "C:\\exe") ? exeWritable : profileWritable;
}
bool exists(const std::string& f) {
    return exeHasSettings && iequals(f, "C:\\exe\\macros.json");
}
bool denies(const std::string&) { return settingsReadOnly; }
}  // namespace dataprobe

static void testDataFolder() {
    section("Where settings live");

    {
        using namespace dataprobe;
        const DataDirProbe probe{&canCreate, &exists, &denies};
        const std::string exe = "C:\\exe";
        const std::string prof = "C:\\profile\\DentalMacros";

        exeWritable = true; profileWritable = true;
        exeHasSettings = false; settingsReadOnly = false;
        ok(chooseDataDir(exe, prof, probe) == exe,
           "beside the program when that folder takes changes");
        exeWritable = false;
        ok(chooseDataDir(exe, prof, probe) == prof,
           "in the Windows profile when it doesn't");
        exeWritable = true; exeHasSettings = true; settingsReadOnly = true;
        ok(chooseDataDir(exe, prof, probe) == prof,
           "or when the settings file itself refuses changes");
        settingsReadOnly = false;
        ok(chooseDataDir(exe, prof, probe) == exe,
           "a settings file that takes changes stays where it is");
        exeWritable = false; profileWritable = false;
        ok(chooseDataDir(exe, prof, probe) == exe,
           "stays put when the profile can't be written either");
        ok(chooseDataDir(exe, "", probe) == exe,
           "and when there is no profile folder at all");
    }

    // --- moving copies, and never overwrites
    const std::string root = appPath("selftest-datadir");
    const std::string from = joinPath(root, "from");
    const std::string to = joinPath(root, "to");
    ensureDir(joinPath(from, "images"));
    ensureDir(joinPath(from, "layouts"));
    writeFileAtomic(joinPath(from, "macros.json"), "{}");
    writeFileAtomic(joinPath(from, "images\\stamp.png"), "png");
    writeFileAtomic(joinPath(from, "layouts\\main.json"), "{}");

    ok(migrateData(from, to) && fileExists(joinPath(to, "macros.json")) &&
           fileExists(joinPath(to, "images\\stamp.png")) &&
           fileExists(joinPath(to, "layouts\\main.json")),
       "moving copies the settings, pictures and layouts");
    writeFileAtomic(joinPath(from, "macros.json"), "{\"newer\": 1}");
    std::string kept;
    readFile(joinPath(to, "macros.json"), kept);
    ok(!migrateData(from, to) && kept == "{}",
       "and never copies over settings already there");
    ok(fileExists(joinPath(from, "macros.json")),
       "the originals stay where they were");

    // --- the checks the decision relies on
    ok(canCreateIn(to), "a normal folder takes new files");
    ok(!fileDeniesWrites(joinPath(to, "macros.json")),
       "a normal file takes changes");
    const std::wstring ro = widen(joinPath(to, "macros.json"));
    SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_READONLY);
    ok(fileDeniesWrites(joinPath(to, "macros.json")),
       "a read-only file refuses them");
    SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_NORMAL);
    {
        // Held open by someone else is not a refusal - sync software does it
        // for a moment all the time.
        HANDLE h = CreateFileW(ro.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        ok(!fileDeniesWrites(joinPath(to, "macros.json")),
           "a file held open by another program is not refusing");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    ok(!fileDeniesWrites(joinPath(root, "nothing-here.json")),
       "a file that isn't there refuses nothing");

    // --- finding stored pictures
    ok(!dataDirIsFallback() && iequals(dataDir(), exeDir()),
       "the test's own folder takes changes, so it is the data folder");
    ok(resolveDataPath("C:\\elsewhere\\x.png") == "C:\\elsewhere\\x.png",
       "an absolute path is left alone");
    writeFileAtomic(appPath("selftest-resolve.png"), "x");
    ok(iequals(resolveDataPath("selftest-resolve.png"),
               appPath("selftest-resolve.png")),
       "a stored relative path is found");
    ok(relativeToApp(dataPath("images\\stamp.png")) == "images\\stamp.png",
       "and stored relative to the data folder");

    deleteFile(appPath("selftest-resolve.png"));
    for (const std::string& dir : {from, to}) {
        deleteFile(joinPath(dir, "macros.json"));
        deleteFile(joinPath(dir, "images\\stamp.png"));
        deleteFile(joinPath(dir, "layouts\\main.json"));
        RemoveDirectoryW(widen(joinPath(dir, "images")).c_str());
        RemoveDirectoryW(widen(joinPath(dir, "layouts")).c_str());
        RemoveDirectoryW(widen(dir).c_str());
    }
    RemoveDirectoryW(widen(root).c_str());
}

// ------------------------------------------------------------ simple mode
// Simple mode's rows: every step in exactly one row, recorded clicks as one
// row, anything Simple mode doesn't make gathered up and kept.
static cfg::Step simpleStep(const std::string& action) {
    cfg::Step s;
    s.action = action;
    s.params = js::Value::object();
    return s;
}

static void testSimpleMode() {
    section("Simple mode");

    cfg::Step type = simpleStep("type");
    type.params["text"] = "Thank you for visiting!\nSee you soon.";
    cfg::Step keys = simpleStep("keys");
    keys.params["combo"] = "ctrl+s";
    cfg::Step wait = simpleStep("sleep");
    wait.params["seconds"] = 0.5;
    cfg::Step prep = simpleStep("window_prepare");
    prep.params["class"] = "WindowsForms10";
    prep.params["exe"] = "opendental.exe";
    cfg::Step click = simpleStep("click");
    click.params["x"] = 10;
    click.params["y"] = 20;
    cfg::Step pause = simpleStep("sleep");
    pause.params["seconds"] = 0.2;
    cfg::Step release = simpleStep("window_release");
    cfg::Step image = simpleStep("click_image");
    cfg::Step page = simpleStep("page_goto");

    {
        const std::vector<cfg::Step> steps = {type, keys, wait};
        const auto rows = smp::rowsFor(steps);
        ok(rows.size() == 3 && rows[0].kind == smp::Kind::Type &&
               rows[1].kind == smp::Kind::Keys &&
               rows[2].kind == smp::Kind::Wait,
           "type, keys and wait are a row each");
        if (rows.size() == 3) {
            ok(rows[0].text == "Type \"Thank you for visiting! See you\xE2\x80\xA6\"",
               "typed text is shown on one line, shortened", rows[0].text);
            ok(rows[1].text == "Press " + keys::prettyCombo("ctrl+s"),
               "keys read as they are pressed", rows[1].text);
            ok(rows[2].text == "Wait half a second", "waits read in words",
               rows[2].text);
        }
    }
    {
        // lock, click, pause, click, release, then a wait of its own
        const std::vector<cfg::Step> steps = {prep, click, pause, click,
                                              release, wait};
        const auto rows = smp::rowsFor(steps);
        ok(rows.size() == 2 && rows[0].kind == smp::Kind::Clicks &&
               rows[0].first == 0 && rows[0].count == 5,
           "a recorded sequence is one row, lock to release");
        if (!rows.empty())
            ok(rows[0].text == "Click in Opendental (2 clicks)",
               "named by the program it clicks in", rows[0].text);
        ok(rows.size() == 2 && rows[1].kind == smp::Kind::Wait,
           "and a wait after it stays a wait");
    }
    {
        // clicks with no lock: the pause between them is theirs, the one
        // after the last is not
        const std::vector<cfg::Step> steps = {click, pause, click, wait};
        const auto rows = smp::rowsFor(steps);
        ok(rows.size() == 2 && rows[0].kind == smp::Kind::Clicks &&
               rows[0].count == 3 && rows[1].kind == smp::Kind::Wait,
           "clicks without a lock group with the pauses between them");
        if (!rows.empty())
            ok(rows[0].text == "Click on the screen (2 clicks)",
               "and say where they go", rows[0].text);
    }
    {
        const std::vector<cfg::Step> steps = {type, image, page, wait, image};
        const auto rows = smp::rowsFor(steps);
        ok(rows.size() == 4 && rows[1].kind == smp::Kind::Advanced &&
               rows[1].first == 1 && rows[1].count == 2 &&
               rows[3].kind == smp::Kind::Advanced && rows[3].count == 1,
           "steps Simple mode doesn't make are gathered, run by run");
        if (rows.size() == 4)
            ok(rows[1].text.find("+ 2 advanced steps") == 0,
               "and counted", rows[1].text);
    }
    {
        // a lock with no release is not a recording
        const std::vector<cfg::Step> steps = {prep, click};
        const auto rows = smp::rowsFor(steps);
        ok(rows.size() == 2 && rows[0].kind == smp::Kind::Advanced &&
               rows[1].kind == smp::Kind::Clicks,
           "a lock without its release is left as an advanced step");
    }
    {
        const std::vector<cfg::Step> steps = {type, prep, click, release, image,
                                              keys, click, wait};
        const auto rows = smp::rowsFor(steps);
        size_t covered = 0, next = 0;
        bool inOrder = true;
        for (const auto& r : rows) {
            if (r.first != next) inOrder = false;
            next = r.first + r.count;
            covered += r.count;
        }
        ok(inOrder && covered == steps.size(),
           "every step is in exactly one row, in order");
        ok(smp::rowOfStep(rows, 2) == 1 && smp::rowOfStep(rows, 99) == -1,
           "finds the row that holds a step");
    }
    ok(smp::rowsFor({}).empty(), "no steps, no rows");

    ok(smp::waitText(1.0) == "Wait 1 second" &&
           smp::waitText(2.0) == "Wait 2 seconds" &&
           smp::waitText(1.5) == "Wait 1.5 seconds",
       "waits of any length read naturally");

    // --- the setting
    ok(cfg::Settings().simpleMode, "a brand-new setup starts in Simple mode");
    {
        const std::string p = appPath("selftest-uimode.json");
        writeFileAtomic(p, "{ \"settings\": { \"matchConfidence\": 0.9 }, "
                           "\"keyboards\": [] }");
        cfg::Config c;
        c.load(p);
        ok(!c.settings.simpleMode,
           "a settings file from before the option opens in Advanced");
        c.settings.simpleMode = true;
        std::string err;
        ok(c.save(&err), "saves the choice", err);
        cfg::Config back;
        back.load(p);
        ok(back.settings.simpleMode, "and reads it back");
        writeFileAtomic(p, "{ broken");
        back.load(p);
        ok(!back.settings.simpleMode,
           "a damaged file means an existing user, so Advanced");
        deleteFile(p);
    }
}

// ------------------------------------------------------- first-run setup
// When setup is offered, and which dental programs it recognises.
static void testDesignSystem() {
    section("SMEdevelop design system");

    // --- the values, exactly as tokens.json has them
    const tk::Colors& c = tk::colors();
    struct Want { const char* name; const tk::Rgb& got; const char* hex; };
    const Want want[] = {
        {"surface", c.surface, "#f7f3ec"},
        {"surface-raised", c.surfaceRaised, "#fffdf9"},
        {"surface-sunken", c.surfaceSunken, "#efe8dc"},
        {"surface-hover", c.surfaceHover, "#e6dccb"},
        {"line", c.line, "#e3d9c8"},
        {"line-strong", c.lineStrong, "#9a8a74"},
        {"ink", c.ink, "#3a3027"},
        {"ink-muted", c.inkMuted, "#6e5f4e"},
        {"sky-100", c.sky100, "#e3eef6"},
        {"sand-100", c.sand100, "#f1e6d8"},
        {"primary", c.primary, "#3d6b8e"},
        {"primary-hover", c.primaryHover, "#335c7b"},
        {"primary-tint", c.primaryTint, "#e3eef6"},
        {"primary-ink", c.primaryInk, "#2f5878"},
        {"focus-ring", c.focusRing, "#3d6b8e"},
        {"success", c.success, "#4f7a52"},
        {"warning", c.warning, "#c98a2b"},
        {"warning-tint", c.warningTint, "#faf0d9"},
        {"warning-ink", c.warningInk, "#8a5a12"},
        {"danger", c.danger, "#b4483a"},
        {"danger-tint", c.dangerTint, "#f8e5e0"},
        {"danger-ink", c.dangerInk, "#a03f32"},
    };
    bool allMatch = true;
    std::string wrong;
    for (const Want& w : want)
        if (tk::toHex(w.got) != w.hex) {
            allMatch = false;
            wrong += std::string(w.name) + "=" + tk::toHex(w.got) + " ";
        }
    ok(allMatch, "every colour token has its tokens.json value", wrong);
    ok(c.overlay.a == 102 && tk::toHex(c.overlay) == "#3a3027",
       "the overlay is warm brown at 40%");

    // --- the contrast the design system promises
    auto ratio = [](const tk::Rgb& a, const tk::Rgb& b) {
        return tk::contrast(a, b);
    };
    std::string low;
    bool inkOk = true;
    for (const tk::Rgb* g : {&c.surface, &c.surfaceRaised, &c.surfaceSunken,
                             &c.sky100, &c.sand100})
        if (ratio(c.ink, *g) < 10.0) {
            inkOk = false;
            low += tk::toHex(*g) + " ";
        }
    ok(inkOk, "ink is 10:1 or better on every ground and wash", low);
    bool mutedOk = true;
    low.clear();
    for (const tk::Rgb* g : {&c.surface, &c.surfaceRaised, &c.surfaceSunken}) {
        // The design system rounds to 5:1; 4.9 leaves room for that rounding
        // while still being well past the 4.5 legibility floor.
        if (ratio(c.inkMuted, *g) < 4.9) {
            mutedOk = false;
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%s %.2f ", tk::toHex(*g).c_str(),
                          ratio(c.inkMuted, *g));
            low += buf;
        }
    }
    ok(mutedOk, "ink-muted is about 5:1 on surface, raised and sunken", low);
    ok(ratio(c.onPrimary, c.primary) >= 5.5, "white on the primary is 5.7:1");
    ok(ratio(c.primaryInk, c.primaryTint) >= 6.0,
       "primary-ink is readable on its tint");
    ok(ratio(c.warningInk, c.warningTint) >= 5.0, "warning-ink on its tint");
    ok(ratio(c.dangerInk, c.dangerTint) >= 5.0, "danger-ink on its tint");
    ok(ratio(c.focusRing, c.surfaceRaised) >= 4.5,
       "the focus ring shows on every panel");
    bool keysOk = true;
    low.clear();
    const tk::Rgb white{255, 255, 255, 255};
    for (const tk::KeyColor& k : tk::keyColors())
        if (ratio(white, k.rgb) < 4.6) {
            keysOk = false;
            low += std::string(k.id) + " ";
        }
    ok(keysOk, "white text is 4.7:1 or better on all seven key colours", low);

    // --- key colours: only the seven, and older files map onto them
    ok(tk::keyColors().size() == 7, "there are exactly seven key colours");
    ok(std::string(tk::defaultKeyColor().id) == "walnut",
       "a new key is walnut");
    bool selfMapped = true;
    for (const tk::KeyColor& k : tk::keyColors())
        if (&tk::nearestKeyColor(k.hex) != &k) selfMapped = false;
    ok(selfMapped, "each of the seven maps to itself");
    auto nearest = [](const char* hex) {
        return std::string(tk::nearestKeyColor(hex).id);
    };
    ok(nearest("") == "walnut" && nearest("not a colour") == "walnut",
       "no colour, or a broken one, is walnut");
    ok(nearest("#808080") == "walnut" && nearest("#202020") == "walnut",
       "greys go to walnut, the neutral one");
    ok(nearest("#ff0000") == "red" && nearest("#e74c3c") == "red",
       "a bright red from before becomes terracotta");
    ok(nearest("#22c55e") == "green", "a green becomes sage");
    ok(nearest("#4f8cff") == "blue", "the old blue accent becomes denim");
    ok(nearest("#f59e0b") == "ochre", "an amber becomes ochre");
    ok(nearest("#8b5cf6") == "plum", "a violet becomes plum");
    ok(nearest("#14b8a6") == "spruce", "a teal becomes spruce");
    ok(nearest("  #B4483A ") == "red", "hex is read in either case, with spaces");

    // --- type
    ok(tk::spec(tk::Text::Body).size == 17,
       "17px is the reading size");
    ok(tk::spec(tk::Text::Button).size == 18 &&
           tk::spec(tk::Text::Button).weight == 600,
       "buttons are 18px semibold");
    ok(tk::spec(tk::Text::Title).size == 26 && tk::spec(tk::Text::Title).weight == 700,
       "headings are 26px bold");
    bool noHeavy = true;
    for (int i = 0; i <= (int)tk::Text::CompactBold; ++i)
        if (tk::spec((tk::Text)i).weight > 800) noHeavy = false;
    ok(noHeavy, "nothing is heavier than 800");
    ok(tk::kTargetMin == 48 && tk::kTargetBig == 64,
       "tap targets are 48px, the main action 64px");
}

static void testFirstRun() {
    section("First-run setup");

    ok(!cfg::Settings().setupDone, "a brand-new setup hasn't been set up");
    {
        const std::string p = appPath("selftest-setupdone.json");
        writeFileAtomic(p, "{ \"settings\": {}, \"keyboards\": [] }");
        cfg::Config c;
        c.load(p);
        ok(c.settings.setupDone,
           "a settings file from before the option was set up long ago");
        c.settings.setupDone = false;
        std::string err;
        ok(c.save(&err), "saves the flag", err);
        cfg::Config back;
        back.load(p);
        ok(!back.settings.setupDone, "and reads it back");
        writeFileAtomic(p, "{ broken");
        back.load(p);
        ok(back.settings.setupDone,
           "a damaged file never brings setup back over someone's buttons");
        deleteFile(p);
    }

    ok(discover::matchProduct("Open Dental Software 24.1") == "Open Dental",
       "recognises a product from its uninstall entry");
    ok(discover::matchProduct("OpenDental.exe") == "Open Dental",
       "and from its program file, spaces or not");
    ok(discover::matchProduct("Dentrix Office Manager - Front Desk") == "Dentrix",
       "and from a window title");
    ok(discover::matchProduct("PRACTICE-WEB 21") == "Practice-Web",
       "whatever the case and punctuation");
    ok(discover::matchProduct("Notepad").empty() &&
           discover::matchProduct("").empty(),
       "and nothing that isn't one");
    {
        // Runs against this machine; only checks that it answers sanely.
        const auto found = discover::findDentalPrograms();
        bool named = true, openFirst = true, seenClosed = false;
        for (const auto& p : found) {
            if (p.name.empty()) named = false;
            if (!p.running) seenClosed = true;
            else if (seenClosed) openFirst = false;
        }
        ok(named && openFirst, "lists what it finds, open programs first",
           std::to_string(found.size()) + " found");
    }
}

// ------------------------------------------------- click recorder steps
// What a recording turns into. The recorder window itself is not tested here
// (it needs a person to right-click), but every step it writes comes from
// rec::buildSteps, and every double-click from rec::makesDouble.
static void testClickRecorder() {
    section("Click recorder");

    rec::Target win;
    win.has = true;
    win.cls = "WindowsForms10.Window.8.app.0.1";
    win.exe = "opendental.exe";
    win.x = 10; win.y = 20; win.w = 1200; win.h = 800;

    auto click = [&](int x, int y, bool dbl = false, bool inWin = true) {
        rec::Click c;
        c.x = x;
        c.y = y;
        c.isDouble = dbl;
        if (inWin) c.window = win;
        return c;
    };

    ok(rec::buildSteps({}, rec::Speed::Normal).empty(),
       "nothing recorded writes nothing");

    {
        auto steps = rec::buildSteps({click(100, 200)}, rec::Speed::Normal);
        ok(steps.size() == 3, "one click: lock, click, release",
           std::to_string(steps.size()));
        if (steps.size() == 3) {
            ok(steps[0].action == "window_prepare" &&
                   steps[0].params.str("class") == win.cls &&
                   steps[0].params.str("exe") == win.exe,
               "locks the window the first click landed in");
            ok(steps[0].params.integer("x") == 10 &&
                   steps[0].params.integer("w") == 1200 &&
                   steps[0].params.integer("h") == 800,
               "at the size and position it had while recording");
            ok(steps[1].action == "click" && steps[1].params.integer("x") == 100 &&
                   steps[1].params.integer("y") == 200,
               "keeps the click position");
            ok(!steps[1].params.boolean("double", false),
               "a single click is not a double-click");
            ok(steps[2].action == "window_release", "ends by releasing the window");
        }
    }

    {
        auto steps = rec::buildSteps(
            {click(1, 1), click(2, 2, true), click(3, 3)}, rec::Speed::Slow);
        // prepare, click, wait, click, wait, click, release
        ok(steps.size() == 7, "three clicks: a pause between each, none after",
           std::to_string(steps.size()));
        if (steps.size() == 7) {
            ok(steps[2].action == "sleep" && steps[4].action == "sleep" &&
                   steps[5].action == "click",
               "pauses sit between clicks only");
            ok(std::fabs(steps[2].params.num("seconds", 0) - 1.5) < 1e-9,
               "Slow pauses 1.5 seconds");
            ok(steps[3].params.boolean("double", false),
               "a double-click is written as one");
        }
    }

    ok(rec::pauseSeconds(rec::Speed::Fast) == 0.2 &&
           rec::pauseSeconds(rec::Speed::Normal) == 0.5,
       "Fast and Normal pause 0.2 and 0.5 seconds");

    {
        auto steps = rec::buildSteps({click(5, 6, false, false)}, rec::Speed::Normal);
        ok(steps.size() == 1 && steps[0].action == "click",
           "no window under the click: no lock and no release");
    }

    // The recorded shape still loads back after a save.
    {
        const std::string p = appPath("selftest-recorder.json");
        cfg::Config c;
        c.path = p;
        cfg::Keyboard kb;
        kb.alias = "Pad";
        kb.match = "VID_1&PID_2";
        cfg::Binding b;
        b.key = "num1";
        b.steps = rec::buildSteps({click(7, 8), click(9, 10, true)},
                                  rec::Speed::Normal);
        kb.active().bindings.push_back(b);
        c.keyboards.push_back(kb);
        std::string err;
        ok(c.save(&err), "saves a recording", err);
        cfg::Config back;
        back.load(p);
        const cfg::Binding* lb =
            back.keyboards.empty() ? nullptr : back.keyboards[0].findKey("num1");
        ok(lb && lb->steps.size() == 5 &&
               lb->steps[3].params.boolean("double", false),
           "and it loads back with the double-click intact");
        deleteFile(p);
    }

    // --- double-click detection
    ok(rec::makesDouble(100, 100, 1000, 102, 99, 1300, 500, 4, 4),
       "a second right-click close by and soon after makes a double-click");
    ok(!rec::makesDouble(100, 100, 1000, 100, 100, 1600, 500, 4, 4),
       "too slow is two separate clicks");
    ok(!rec::makesDouble(100, 100, 1000, 140, 100, 1100, 500, 4, 4),
       "too far apart is two separate clicks");
    ok(rec::makesDouble(5, 5, 0xFFFFFF00ul, 5, 5, 0x00000010ul, 500, 4, 4),
       "copes with the tick count wrapping round");

    // --- what the user reads
    ok(rec::describe(click(1, 1)) == "Click in Opendental",
       "names the program from its executable", rec::describe(click(1, 1)));
    ok(rec::describe(click(1, 1, true)) == "Double-click in Opendental",
       "says when it is a double-click");
    ok(rec::describe(click(1, 1, false, false)).find("not inside a program") !=
           std::string::npos,
       "says when a click is not inside a program");
    rec::Target other = win;
    other.exe = "notepad.exe";
    other.cls = "Notepad";
    ok(win.sameWindowAs(win) && !win.sameWindowAs(other),
       "tells one program's window from another's");
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
    c.settings.startEngineOnLaunch = false;

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
    kb.active().bindings.push_back(b);
    c.keyboards.push_back(kb);

    std::string err;
    ok(c.save(&err), "saves", err);

    cfg::Config loaded;
    loaded.load(path);
    ok(loaded.loadError.empty(), "reloads without error", loaded.loadError);
    ok(loaded.keyboards.size() == 1, "keeps the keyboard");
    ok(std::fabs(loaded.settings.matchConfidence - 0.77) < 1e-9,
       "keeps settings");
    ok(!loaded.settings.startEngineOnLaunch,
       "keeps boolean settings, including listening turned off");

    // Listening is on by default, and files saved before that switch on once.
    ok(cfg::Settings().startEngineOnLaunch, "a new setup starts listening");
    {
        const std::string p = appPath("selftest-listen.json");
        writeFileAtomic(p, "{ \"settings\": { \"startEngineOnLaunch\": false }, "
                           "\"keyboards\": [] }");
        cfg::Config old;
        old.load(p);
        ok(old.settings.startEngineOnLaunch,
           "an older file's 'off' turns into listening");
        DeleteFileW(widen(p).c_str());
    }

    if (!loaded.keyboards.empty()) {
        const cfg::Keyboard& k = loaded.keyboards[0];
        ok(k.alias == "Test pad" && k.deck == "numpad", "keeps keyboard fields");
        ok(k.active().bindings.size() == 1, "keeps the binding");
        if (!k.active().bindings.empty()) {
            const cfg::Binding& lb = k.active().bindings[0];
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
    kb.active().bindings.push_back(bound);

    cfg::Binding empty;          // num1 is a tile with no steps yet
    empty.key = "num1";
    kb.active().bindings.push_back(empty);

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
    ok(!kb.active().bindings.empty(),
       "imported bindings: " + std::to_string(kb.active().bindings.size()));

    int totalSteps = 0, badActions = 0;
    bool foundType = false, foundMultiStep = false;
    for (const auto& b : kb.active().bindings) {
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

    for (const auto& b : kb.active().bindings)
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
    ok(d.keyboards.size() == 1 && d.keyboards[0].active().bindings.size() == 2,
       "collapses the duplicate to one binding");
    if (!d.keyboards.empty() && !d.keyboards[0].active().bindings.empty()) {
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
        kb.active().bindings.push_back(b);
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

const wchar_t* kTestWndClass = L"DentalMacrosSelfTestWindow";

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
    HWND h = CreateWindowExW(0, kTestWndClass, L"Dental Macros self-test window",
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
    dev::RawInputListener idle;
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
    kb.active().bindings.push_back(macro);
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
    c.keyboards[0].active().bindings.push_back(empty);
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


// ------------------------------------------------------------------- pages
static void testPages() {
    section("Pages and per-application decks");

    // ---- what an application rule matches, and what it must not ----------
    cfg::AppRule none;
    ok(none.empty(), "a rule with nothing in it is empty");
    ok(!none.matches("chrome.exe", "Chrome_WidgetWin_1", "Docs"),
       "and matches nothing at all - an empty rule is a default page, "
       "not a wildcard");

    cfg::AppRule byExe;
    byExe.exe = "photoshop.exe";
    ok(byExe.matches("Photoshop.exe", "Photoshop", "untitled-1"),
       "the executable matches without regard to case");
    ok(!byExe.matches("photoshopped.exe", "Photoshop", ""),
       "but not as a prefix of a different program");

    cfg::AppRule byTitle;
    byTitle.exe = "chrome.exe";
    byTitle.title = "GitHub";
    ok(byTitle.matches("chrome.exe", "Chrome_WidgetWin_1", "PR #4 - GitHub"),
       "a title rule matches on a substring");
    ok(!byTitle.matches("chrome.exe", "Chrome_WidgetWin_1", "BBC News"),
       "and fails when the substring is absent");
    ok(!byTitle.matches("firefox.exe", "MozillaWindowClass", "GitHub"),
       "every filled-in field has to agree, not just one");
    ok(byTitle.specificity() > byExe.specificity(),
       "a rule naming two things is more specific than one naming one");

    // ---- which page an application gets ----------------------------------
    cfg::Keyboard kb;
    kb.alias = "Pad";
    kb.pages.clear();
    cfg::Page base;
    base.name = "Base";
    cfg::Page chrome;
    chrome.name = "Chrome";
    chrome.rule.exe = "chrome.exe";
    cfg::Page github;
    github.name = "GitHub";
    github.rule.exe = "chrome.exe";
    github.rule.title = "GitHub";
    kb.pages.push_back(base);
    kb.pages.push_back(chrome);
    kb.pages.push_back(github);
    kb.normalizePages();

    ok(kb.defaultPage() == 0, "the page with no rule is the default");
    ok(kb.pageForApp("notepad.exe", "Notepad", "readme") == 0,
       "an application nothing claims lands on the default page");
    ok(kb.pageForApp("chrome.exe", "Chrome_WidgetWin_1", "BBC News") == 1,
       "a program with a page of its own gets it");
    ok(kb.pageForApp("chrome.exe", "Chrome_WidgetWin_1", "PR - GitHub") == 2,
       "and the more specific of two matching pages wins");
    ok(kb.pageByName("chrome") == 1, "pages are found by name, case aside");
    ok(kb.pageByName("nothing") == -1, "and a name nobody has is not found");
    ok(kb.uniquePageName("Base") == "Base 2",
       "a name already taken comes back with a number on it");

    // A deck with every page ruled still has to answer with something.
    cfg::Keyboard allRuled;
    allRuled.pages.clear();
    allRuled.pages.push_back(chrome);
    allRuled.pages.push_back(github);
    allRuled.normalizePages();
    ok(allRuled.defaultPage() == 0,
       "with no unruled page, the first one stands in as the default");
    ok(allRuled.pageForApp("notepad.exe", "Notepad", "") == 0,
       "so an unclaimed application still has somewhere to go");

    // ---- the same key on two pages ---------------------------------------
    cfg::Binding one;
    one.key = "num1";
    one.title = "Cut";
    one.steps.push_back({"sleep", js::Value::object()});
    cfg::Binding two;
    two.key = "num1";
    two.title = "Crop";
    two.steps.push_back({"sleep", js::Value::object()});
    kb.pages[0].bindings.push_back(one);
    kb.pages[1].bindings.push_back(two);

    kb.activePage = 0;
    ok(kb.findKey("num1") && kb.findKey("num1")->title == "Cut",
       "a key resolves against the page that is in front");
    kb.activePage = 1;
    ok(kb.findKey("num1") && kb.findKey("num1")->title == "Crop",
       "and means something else on the next page - the whole point");
    ok(kb.anyPageHasMacro("num1"), "the key carries a macro somewhere");
    ok(!kb.anyPageHasMacro("num9"), "and this one carries none anywhere");

    // ---- blocking spans every page ---------------------------------------
    // A key that types into your document on one page and fires a macro on
    // another would be worse than either answer on its own.
    cfg::Config c;
    cfg::Keyboard blocking;
    blocking.alias = "Pad";
    blocking.match = "VID_046D&PID_C53D";
    blocking.block = true;
    blocking.blockAllKeys = false;
    blocking.pages.clear();
    cfg::Page p1, p2;
    p1.name = "One";
    p2.name = "Two";
    cfg::Binding f13;
    f13.key = "f13";
    f13.steps.push_back({"sleep", js::Value::object()});
    p2.bindings.push_back(f13);      // the macro lives on the SECOND page
    blocking.pages.push_back(p1);
    blocking.pages.push_back(p2);
    blocking.normalizePages();
    blocking.activePage = 0;         // ...while the FIRST one is in front
    c.keyboards.push_back(blocking);

    ok(c.couldBlockVk(keys::toVk("f13")),
       "a key with a macro on any page is still worth looking at");
    ok(c.blockedForKey("\\\\?\\HID#VID_046D&PID_C53D", keys::toVk("f13")),
       "and is taken from other programs even while another page is up");
    ok(!c.couldBlockVk(keys::toVk("f14")),
       "a key with no macro on any page is left alone");

    // ---- the file --------------------------------------------------------
    cfg::Config multi;
    multi.keyboards.push_back(kb);
    const std::string path = appPath("selftest-pages.json");
    std::string err;
    ok(multi.saveAs(path, &err), "a deck with pages saves", err);

    std::string written;
    readFile(path, written);
    ok(written.find("\"pages\"") != std::string::npos,
       "and is written in the pages shape");

    cfg::Config back;
    back.load(path);
    ok(back.keyboards.size() == 1 && back.keyboards[0].pages.size() == 3,
       "all three pages come back");
    if (!back.keyboards.empty() && back.keyboards[0].pages.size() == 3) {
        const cfg::Keyboard& r = back.keyboards[0];
        ok(r.pages[1].name == "Chrome", "with their names");
        ok(r.pages[1].rule.exe == "chrome.exe", "and their application rules");
        ok(r.pages[2].rule.title == "GitHub", "including the title part");
        ok(r.pages[0].bindings.size() == 1 && r.pages[1].bindings.size() == 1,
           "and a key on each page rather than one of them dropped");
        ok(r.pages[0].bindings[0].title == "Cut" &&
               r.pages[1].bindings[0].title == "Crop",
           "the same key kept a different macro on each page");
        ok(r.activePage == r.defaultPage(),
           "and it opens on the page its rules ask for, not the one that "
           "happened to be up when it was saved");
    }
    // Two bindings for one key on ONE page is still wrong, and still refused.
    bool moaned = false;
    for (const std::string& w : back.loadWarnings)
        if (w.find("more than once") != std::string::npos) moaned = true;
    ok(!moaned,
       "the same key on two different pages draws no duplicate warning");
    deleteFile(path);

    // ---- a file from before pages existed --------------------------------
    const std::string oldPath = appPath("selftest-oldshape.json");
    writeFileAtomic(oldPath,
                    "{\"keyboards\":[{\"alias\":\"Pad\","
                    "\"match\":\"VID_046D\",\"deck\":\"numpad\","
                    "\"bindings\":[{\"key\":\"num1\",\"title\":\"Slack\","
                    "\"steps\":[{\"do\":\"sleep\",\"seconds\":1}]}]}]}");
    cfg::Config old;
    old.load(oldPath);
    ok(old.loadError.empty(), "a file written before pages existed still loads",
       old.loadError);
    ok(old.keyboards.size() == 1 && old.keyboards[0].pages.size() == 1,
       "as a deck with exactly one page");
    ok(!old.keyboards.empty() && old.keyboards[0].findKey("num1") &&
           old.keyboards[0].findKey("num1")->title == "Slack",
       "with its keys where they always were");

    // ...and it goes back out in the shape it came in, so an older build can
    // still read a config this one has touched.
    const std::string rePath = appPath("selftest-oldshape2.json");
    ok(old.saveAs(rePath, &err), "and saves again", err);
    std::string reWritten;
    readFile(rePath, reWritten);
    ok(reWritten.find("\"pages\"") == std::string::npos,
       "still without a pages block - one plain page is written the old way");
    ok(reWritten.find("\"bindings\"") != std::string::npos,
       "as plain bindings");

    // ...but only while there is genuinely nothing the old shape cannot hold.
    // A name is such a thing, and writing it away would lose it silently.
    cfg::Config named;
    cfg::Keyboard nk;
    nk.alias = "Pad";
    nk.pages[0].name = "General";
    nk.pages[0].bindings.push_back(one);
    named.keyboards.push_back(nk);
    const std::string namedPath = appPath("selftest-named.json");
    ok(named.saveAs(namedPath, &err), "a deck with one named page saves", err);
    cfg::Config namedBack;
    namedBack.load(namedPath);
    ok(!namedBack.keyboards.empty() &&
           namedBack.keyboards[0].pages.size() == 1 &&
           namedBack.keyboards[0].pages[0].name == "General",
       "and keeps the name it was given");
    deleteFile(namedPath);
    deleteFile(oldPath);
    deleteFile(rePath);
}


// ------------------------------------------------------- mice and trackpads
static void testMouse() {
    section("Mice, trackpads and the wheel");

    // ---- buttons are keys ------------------------------------------------
    ok(keys::toVk("mouseleft") == VK_LBUTTON,
       "a mouse button uses the virtual key Windows already reserves for it");
    ok(keys::toVk("mouse4") == VK_XBUTTON1 &&
           keys::toVk("mouse5") == VK_XBUTTON2,
       "and the thumb buttons use theirs");
    ok(keys::toVk("mouseback") == keys::toVk("mouse4"),
       "\"back\" is accepted for the button that does it");
    ok(keys::toVk("wheelup") == dev::kVkWheelUp &&
           keys::toVk("wheeldown") == dev::kVkWheelDown,
       "the wheel gets codes out of the unassigned block");
    ok(keys::vkName(dev::kVkWheelUp) == "wheelup",
       "and names round-trip back out of them");
    ok(keys::displayName("mouseleft") == "Left click",
       "buttons read as what they are, not as a code");

    // No mouse code may collide with a key someone might actually press:
    // that would make one binding fire for two different things.
    ok(dev::isMouseVk(VK_LBUTTON) && dev::isMouseVk(dev::kVkWheelDown),
       "pointer keys are recognised as such");
    ok(!dev::isMouseVk(keys::toVk("a")) && !dev::isMouseVk(keys::toVk("f13")) &&
           !dev::isMouseVk(VK_SPACE),
       "and ordinary keys are not");
    for (int vk = dev::kVkWheelUp; vk <= dev::kVkWheelRight; ++vk) {
        const std::string n = keys::vkName(vk);
        ok(n.rfind("wheel", 0) == 0,
           "wheel code 0x" + std::to_string(vk) + " is not shadowing a key");
    }

    // ---- left click can never carry a macro ------------------------------
    // Not a situational rule like blocking's: a binding is written to a file
    // and read back somewhere else, so "sometimes allowed" would mean a config
    // that is harmless here and unusable there.
    std::string why;
    ok(!cfg::keyBindable("mouseleft", &why),
       "left click cannot be bound at all");
    ok(!why.empty(), "and says why, rather than just refusing");
    ok(cfg::keyBindable("mouseright") && cfg::keyBindable("mousemiddle") &&
           cfg::keyBindable("mouse4") && cfg::keyBindable("wheelup"),
       "every other button and the wheel still can");
    ok(cfg::keyBindable("a") && cfg::keyBindable("num1"),
       "and no keyboard key is affected");
    ok(!cfg::keyBindable("MouseLeft") && !cfg::keyBindable("  mouseleft  "),
       "the refusal is not defeated by case or padding");

    // A file that names it anyway - hand-edited, or written by an older
    // build - has that binding dropped rather than honoured.
    const std::string sneaky = appPath("selftest-leftclick.json");
    writeFileAtomic(sneaky,
                    "{\"keyboards\":[{\"alias\":\"M\",\"match\":\"VID_1\","
                    "\"device\":\"mouse\",\"bindings\":["
                    "{\"key\":\"mouseleft\",\"steps\":[{\"do\":\"sleep\"}]},"
                    "{\"key\":\"mouse4\",\"steps\":[{\"do\":\"sleep\"}]}]}]}");
    cfg::Config sneaked;
    sneaked.load(sneaky);
    ok(!sneaked.keyboards.empty() &&
           sneaked.keyboards[0].findKey("mouseleft") == nullptr,
       "a config naming left click has that binding ignored on load");
    ok(!sneaked.keyboards.empty() &&
           sneaked.keyboards[0].findKey("mouse4") != nullptr,
       "while the rest of that deck loads normally");
    bool warned = false;
    for (const std::string& w : sneaked.loadWarnings)
        if (w.find("mouseleft") != std::string::npos) warned = true;
    ok(warned, "and it is reported rather than dropped silently");
    deleteFile(sneaky);

    // ---- the extra buttons a multi-button mouse has ----------------------
    ok(keys::toVk("mouse6") == dev::kVkMouse6,
       "button 6 has a key code of its own");
    ok(keys::toVk("mouse20") == dev::kVkMouseExtraLast,
       "and the range runs to button 20");
    ok(dev::isMouseVk(keys::toVk("mouse12")),
       "an extra button is recognised as a pointer key");
    ok(dev::vkForHidButton(1) == VK_LBUTTON && dev::vkForHidButton(5) ==
                                                   VK_XBUTTON2,
       "HID button numbers 1-5 map onto the codes Windows already has");
    ok(dev::vkForHidButton(6) == keys::toVk("mouse6") &&
           dev::vkForHidButton(9) == keys::toVk("mouse9"),
       "and 6 upwards onto the extra ones");
    ok(dev::vkForHidButton(0) == 0 && dev::vkForHidButton(99) == 0,
       "a button number nothing can represent maps to nothing");
    // The extra codes must not shadow a key someone could actually press,
    // or one binding would fire for two different things.
    for (int vk = dev::kVkMouse6; vk <= dev::kVkMouseExtraLast; ++vk) {
        const std::string n = keys::vkName(vk);
        ok(n.rfind("mouse", 0) == 0,
           "extra-button code 0x" + std::to_string(vk) +
               " is not shadowing a key");
    }

    // ---- a mouse survives the file ---------------------------------------
    cfg::Config c;
    cfg::Keyboard m;
    m.alias = "Spare mouse";
    m.match = "VID_046D&PID_C08B";
    m.isMouse = true;
    m.block = true;
    m.blockAllKeys = false;
    m.deck = "mouse";
    cfg::Binding side;
    side.key = "mouse4";
    side.title = "Undo";
    side.steps.push_back({"sleep", js::Value::object()});
    // The same tile as a dial: press does one thing, the wheel does two more.
    side.scrollUp.push_back({"sleep", js::Value::object()});
    side.scrollDown.push_back({"sleep", js::Value::object()});
    m.active().bindings.push_back(side);
    c.keyboards.push_back(m);

    ok(c.keyboards[0].active().bindings[0].isDial(),
       "a binding with wheel steps is a dial");
    ok(c.anyMouseBlocks(), "a blocking mouse asks for the pointer hook");

    const std::string path = appPath("selftest-mouse.json");
    std::string err;
    ok(c.saveAs(path, &err), "a mouse deck saves", err);
    cfg::Config back;
    back.load(path);
    ok(back.keyboards.size() == 1 && back.keyboards[0].isMouse,
       "and comes back marked as a pointer");
    ok(!back.keyboards.empty() && back.keyboards[0].deck == "mouse",
       "on the deck shaped like one");
    const cfg::Binding* rb =
        back.keyboards.empty() ? nullptr
                               : back.keyboards[0].findKey("mouse4");
    ok(rb && rb->scrollUp.size() == 1 && rb->scrollDown.size() == 1,
       "with both wheel directions intact");
    ok(rb && rb->steps.size() == 1,
       "and the press still separate from them");
    // "mouse4" is a real key here and must not be complained about.
    bool moaned = false;
    for (const std::string& w : back.loadWarnings)
        if (w.find("mouse4") != std::string::npos) moaned = true;
    ok(!moaned, "and no warning that mouse4 is not a real key");
    deleteFile(path);

    // A tile that is not a dial must not grow empty arrays in the file.
    cfg::Config plain;
    cfg::Keyboard pk;
    pk.alias = "Pad";
    cfg::Binding pb;
    pb.key = "num1";
    pb.steps.push_back({"sleep", js::Value::object()});
    pk.active().bindings.push_back(pb);
    plain.keyboards.push_back(pk);
    ok(plain.toJson().find("scrollUp") == std::string::npos,
       "an ordinary key is written without any wheel fields");

    // ---- the primary button is not takeable on a one-pointer machine -----
    // The escape hatch for a keyboard is that you can still type
    // Ctrl+Alt+Shift+M. There is none for a pointer, so this rule is what
    // stands between a bad configuration and a machine you cannot click on.
    const std::string devPath = "\\\\?\\HID#VID_046D&PID_C08B";
    cfg::Config one;
    cfg::Keyboard greedy;
    greedy.alias = "Mouse";
    greedy.match = "VID_046D&PID_C08B";
    greedy.isMouse = true;
    greedy.block = true;
    greedy.blockAllKeys = true;      // asked for everything...
    cfg::Binding lb;
    lb.key = "mouseleft";
    lb.steps.push_back({"sleep", js::Value::object()});
    greedy.active().bindings.push_back(lb);
    one.keyboards.push_back(greedy);
    one.pointerCount = 1;

    const int primary =
        GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    ok(dev::isPrimaryButtonVk(primary),
       "the primary button is whichever one Windows currently calls primary");
    ok(!one.blockedForKey(devPath, primary),
       "...and it is still not taken when it is the only pointer");
    ok(!one.couldBlockVk(primary),
       "the cheap pre-filter refuses it too, so it never even correlates");
    ok(one.blockedForKey(devPath, VK_MBUTTON),
       "every other button on that mouse is still blocked as asked");

    // With a second pointer attached there is a way out again, so a genuinely
    // spare mouse may give up all of its buttons.
    one.pointerCount = 2;
    ok(one.blockedForKey(devPath, primary),
       "with a second pointer attached the primary button can be taken");

    // A mouse that blocks nothing must not drag the pointer hook in with it.
    cfg::Config passive;
    cfg::Keyboard pm;
    pm.alias = "Mouse";
    pm.isMouse = true;
    pm.block = false;
    passive.keyboards.push_back(pm);
    ok(!passive.anyMouseBlocks(),
       "a mouse that blocks nothing installs no pointer hook");

    // ---- the deck shaped like a mouse ------------------------------------
    const cfg::DeckPreset* mp = cfg::findPreset("mouse");
    ok(mp != nullptr, "there is a mouse deck preset");
    if (mp) {
        ok(mp->fixedKeys, "which already knows its own button names");
        bool sawLeft = false, sawWheel = false, sawExtra = false;
        for (const cfg::DeckSlot& sl : mp->slots) {
            if (sl.key == "mouseleft") sawLeft = true;
            if (sl.key == "wheelup") sawWheel = true;
            if (sl.key == "mouse6") sawExtra = true;
            ok(sl.key.empty() || keys::toVk(sl.key) != 0,
               "slot \"" + sl.key + "\" names a key this build knows");
            ok(cfg::keyBindable(sl.key),
               "slot \"" + sl.key + "\" offers a key that can be bound");
        }
        ok(sawWheel, "including the wheel");
        ok(!sawLeft,
           "and deliberately no left click - it has to keep working");
        ok(sawExtra,
           "with the space it would have taken given to an extra button");
    }
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
    sw.active().bindings.push_back(b);
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
    ok(!back.keyboards[0].active().bindings.empty() &&
           back.keyboards[0].active().bindings[0].key == "btn1",
       "keeping the button id");
    ok(back.keyboards[0].active().bindings[0].title == "Print the day sheet",
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
    c.keyboards[0].active().bindings.push_back(b);
    const std::string path = appPath("selftest-icons.json");
    std::string err;
    ok(c.saveAs(path, &err), "a deck using a library icon saves", err);
    cfg::Config back;
    back.load(path);
    ok(!back.keyboards.empty() && !back.keyboards[0].active().bindings.empty() &&
           back.keyboards[0].active().bindings[0].icon == ref,
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
        kb.active().bindings.push_back(b);
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

// ------------------------------------------------------ Bluetooth keypads
// What a Bluetooth LE keypad looks like to Raw Input, in the shape real ones
// have (the addresses here are made up): one keypad, several inputs, each
// with a path of its own, and nothing shared between those paths but the ids
// and the address. Its extra buttons mostly arrive as media keys.
static void testBluetoothKeypads() {
    section("Bluetooth keypads and media keys");

    const std::string hid = "\\\\?\\HID#{00001812-0000-1000-8000-00805f9b34fb}";
    const std::string pad = hid + "_Dev_VID&0203f0_PID&854a_REV&044c_a1b2c3d4e5f6";
    const std::string keysIn =
        pad + "&Col02#b&365c9656&0&0001#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}";
    const std::string mediaIn =
        pad + "&Col03#b&365c9656&0&0002#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    const std::string mouseIn =
        pad + "&Col01#b&365c9656&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}";
    const std::string otherPad =
        hid + "_Dev_VID&02256c_PID&8251_REV&0001_0f1e2d3c4b5a";
    const std::string otherKeys =
        otherPad + "&Col01#b&2780b1fd&0&0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}";
    // A dial can be a HID device of its own, with no collection number.
    const std::string otherDial =
        otherPad + "#b&3b50ed85&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    const std::string usbKeys =
        "\\\\?\\HID#VID_046D&PID_C539&MI_00#9&36ad48bf&0&0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}";
    const std::string usbMedia =
        "\\\\?\\HID#VID_046D&PID_C539&MI_01&Col02#9&237d2c81&0&0001#{4d1e55b2-f16f-11cf-88cb-001111000030}";

    // ---- reading a path --------------------------------------------------
    dev::InputDevice d;
    dev::parseDevicePath(keysIn, d);
    ok(d.vid == "03F0" && d.pid == "854A",
       "reads a Bluetooth keypad's vendor and product id", d.vid + " " + d.pid);
    ok(d.idMatch == "VID&0203f0_PID&854a",
       "and keeps them spelled the way its path spells them", d.idMatch);
    ok(d.btAddress == "a1b2c3d4e5f6", "finds the Bluetooth address",
       d.btAddress);
    dev::InputDevice u;
    dev::parseDevicePath(usbKeys, u);
    ok(u.vid == "046D" && u.idMatch == "VID_046D&PID_C539" && u.btAddress.empty(),
       "a USB path reads as it always did");
    ok(dev::bluetoothAddress(otherDial) == "0f1e2d3c4b5a",
       "an input with no collection number has the address too");
    ok(dev::bluetoothAddress("_a1b2c3d4e5f6").empty() &&
           dev::bluetoothAddress(usbKeys).empty(),
       "nothing that is not a Bluetooth LE path yields an address");

    ok(dev::sameDevice(keysIn, mediaIn) && dev::sameDevice(mouseIn, keysIn),
       "a keypad's inputs are one device");
    ok(!dev::sameDevice(keysIn, otherKeys), "a different keypad is not");
    ok(dev::sameDevice(usbKeys, usbKeys) && !dev::sameDevice(usbKeys, usbMedia),
       "a USB input is only ever itself");

    // ---- matching a deck to a keypad -------------------------------------
    bool stable = false;
    const std::string m = cfg::chooseMatch(d, {d}, &stable);
    ok(m == "_a1b2c3d4e5f6" && stable, "a keypad is matched by its address", m);
    ok(cfg::specMatches(m, keysIn) && cfg::specMatches(m, mediaIn) &&
           cfg::specMatches(m, mouseIn),
       "which selects every input it has, media keys included");
    ok(!cfg::specMatches(m, otherKeys), "and nothing of any other keypad's");
    ok(cfg::chooseMatch(u, {u}, &stable) == "VID_046D&PID_C539" && stable,
       "a USB keyboard is still matched by its ids");
    ok(cfg::specMatches(keysIn, mediaIn),
       "a deck saved with one input's full path, as older builds did, "
       "reaches that keypad's media keys");
    ok(!cfg::specMatches(keysIn, otherKeys),
       "without reaching another keypad");
    ok(!cfg::specMatches(usbKeys, usbMedia),
       "a full USB path is not widened - it names one port");

    // ---- two decks on one keypad -----------------------------------------
    cfg::Config c;
    cfg::Keyboard dialDeck;
    dialDeck.alias = "Dial";
    dialDeck.match = m;
    dialDeck.isMouse = true;
    cfg::Keyboard keyDeck;
    keyDeck.alias = "Keys";
    keyDeck.match = m;
    // The mouse deck first, so the order of the list cannot be what decides.
    c.keyboards.push_back(dialDeck);
    c.keyboards.push_back(keyDeck);
    const cfg::Keyboard* forKey = c.keyboardForDevice(keysIn, 'K');
    const cfg::Keyboard* forWheel = c.keyboardForDevice(mouseIn, dev::kVkWheelUp);
    ok(forKey && forKey->alias == "Keys",
       "a key goes to the keypad's keyboard deck");
    ok(forWheel && forWheel->alias == "Dial", "and the dial to its mouse deck");
    c.keyboards.erase(c.keyboards.begin());
    forWheel = c.keyboardForDevice(mouseIn, dev::kVkWheelUp);
    ok(forWheel && forWheel->alias == "Keys",
       "with one deck on the keypad, that deck hears all of it");

    // ---- media keys ------------------------------------------------------
    ok(dev::vkForConsumerUsage(0xEA) == VK_VOLUME_DOWN &&
           dev::vkForConsumerUsage(0x224) == VK_BROWSER_BACK,
       "a media key Windows has a key code for keeps that code");
    ok(keys::vkName(dev::vkForConsumerUsage(0x192)) == "calculator",
       "the calculator key has a name",
       keys::vkName(dev::vkForConsumerUsage(0x192)));
    ok(keys::vkName(dev::vkForConsumerUsage(0xCD)) == "mediaplay",
       "a dial's play/pause is the same key as a keyboard's");
    const int zoom = dev::vkForConsumerUsage(0x22D);
    ok(zoom > 0xFF && keys::vkName(zoom) == "zoomin",
       "one Windows has no code for is numbered out of its way",
       keys::vkName(zoom));
    ok(keys::toVk("zoomin") == zoom, "and its name reads back");
    const int unnamed = dev::vkForConsumerUsage(0x21A);
    ok(keys::vkName(unnamed) == "consumer_0x21A" &&
           keys::toVk("consumer_0x21A") == unnamed,
       "an unnamed one still round-trips through its name",
       keys::vkName(unnamed));
    ok(keys::displayName("consumer_0x21A") == "Media 21A",
       "and reads as a media key on its tile",
       keys::displayName("consumer_0x21A"));
    ok(dev::vkForConsumerUsage(0) == 0, "usage 0 is nothing pressed");
    ok(!dev::isMouseVk(zoom) && !dev::isMouseVk(VK_VOLUME_DOWN),
       "no media key is mistaken for a mouse button");
    std::string err;
    ok(keys::parseCombo("zoomin", &err).empty() && !err.empty(),
       "a macro cannot type a key that has no key code", err);
    ok(!keys::parseCombo("volumeup", &err).empty(),
       "but can type a media key that has one");
    unsigned mods = 0;
    int hotVk = 0;
    ok(!keys::parseHotkey("ctrl+zoomin", mods, hotVk, &err),
       "and no shortcut can be made of one", err);

    // ---- blocking what a keypad sends ------------------------------------
    ok(dev::rawInputVk(VK_LCONTROL) == VK_CONTROL &&
           dev::rawInputVk(VK_RCONTROL) == VK_CONTROL,
       "the hook's left and right Ctrl are Raw Input's Ctrl");
    ok(dev::rawInputVk(VK_LSHIFT) == VK_SHIFT &&
           dev::rawInputVk(VK_RMENU) == VK_MENU,
       "and the same goes for Shift and Alt");
    ok(dev::rawInputVk(VK_LWIN) == VK_LWIN && dev::rawInputVk('A') == 'A',
       "every other key is left as it is");
    cfg::Config blocking;
    cfg::Keyboard selective;
    selective.alias = "Pad";
    selective.match = m;
    selective.block = true;
    selective.blockAllKeys = false;
    for (const char* key : {"ctrl", "calculator"}) {
        cfg::Binding b;
        b.key = key;
        b.steps.push_back({"sleep", js::Value::object()});
        selective.active().bindings.push_back(b);
    }
    blocking.keyboards.push_back(selective);
    const int hookCtrl = dev::rawInputVk(VK_LCONTROL);
    ok(blocking.couldBlockVk(hookCtrl) &&
           blocking.blockedForKey(keysIn, hookCtrl),
       "so a macro on a keypad's Ctrl key can be blocked");
    ok(blocking.couldBlockVk(VK_LAUNCH_APP2) &&
           blocking.blockedForKey(mediaIn, VK_LAUNCH_APP2),
       "and so can a macro on its calculator key");

    // ---- a keypad that sends a press twice -------------------------------
    eng::PressFilter presses;
    auto key = [&](int vk, bool down, double t) {
        dev::KeyEvent e;
        e.vk = vk;
        e.down = down;
        e.t = t;
        e.devicePath = keysIn;
        return e;
    };
    ok(!presses.ignore(key(VK_SPACE, true, 10.00)), "a press runs");
    presses.ignore(key(VK_SPACE, false, 10.02));
    ok(presses.ignore(key(VK_SPACE, true, 10.03)),
       "a second press 10ms after its release is the same press");
    presses.ignore(key(VK_SPACE, false, 10.04));
    ok(!presses.ignore(key(VK_SPACE, true, 10.20)),
       "a press 160ms after the release is a new one");
    ok(presses.ignore(key(VK_SPACE, true, 10.25)),
       "a key still held down is repeating, not pressed again");
    presses.ignore(key(VK_SPACE, false, 10.30));
    ok(!presses.ignore(key('K', true, 10.31)),
       "a different key straight after is a press of its own");
    bool everyNotch = true;
    for (int i = 0; i < 5; ++i) {
        const double t = 20.0 + i * 0.008;
        if (presses.ignore(key(dev::kVkWheelUp, true, t))) everyNotch = false;
        presses.ignore(key(dev::kVkWheelUp, false, t));
    }
    ok(everyNotch, "five wheel notches 8ms apart are five notches");
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("Dental Macros self-test\n");
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
    testClickRecorder();
    testFriendlyProblems();
    testDataFolder();
    testSimpleMode();
    testFirstRun();
    testDesignSystem();
    testIconLibrary();
    testSoftwareDeck();
    testPages();
    testMouse();
    testBluetoothKeypads();
    testHookCost();
    if (!yaml.empty()) testYamlImport(yaml);
    testWindows();
    testVision();

    std::printf("\n=====================================\n");
    std::printf("  passed: %d    failed: %d\n", g_pass, g_fail);
    std::printf("=====================================\n");
    return g_fail == 0 ? 0 : 1;
}
