#include "app.h"

#include "config.h"
#include "device.h"
#include "dialogs.h"
#include "engine.h"
#include "gdiplus_inc.h"
#include "iconlib.h"
#include "keys.h"
#include "minideck.h"
#include "simple.h"
#include "theme.h"
#include "ui_common.h"
#include "util.h"
#include "vision.h"
#include "winman.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <dbt.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace Gdiplus;

namespace ui {
namespace {

constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_KEYEVENT = WM_APP + 2;
constexpr UINT WM_APP_TRAY = WM_APP + 3;
// A macro asked for a different page. Posted rather than called straight
// through: the request arrives on the input thread, and everything it moves
// belongs to the UI one.
constexpr UINT WM_APP_PAGE = WM_APP + 4;
// The window in front changed, so the app-linked pages get another look.
constexpr UINT WM_APP_FOREGROUND = WM_APP + 5;
// A macro run finished, well or not, so the status line has news.
constexpr UINT WM_APP_OUTCOME = WM_APP + 6;
// The settings file could not be loaded: explain it once the window is up.
constexpr UINT WM_APP_LOADPROBLEM = WM_APP + 7;
// A brand-new setup: open first-run setup once the window is on screen.
constexpr UINT WM_APP_SETUP = WM_APP + 8;

enum {
    ID_TITLE_EDIT = 200,
    ID_STEP_LIST,
    ID_LOG_EDIT,
    ID_BTN_ADD_STEP,
    ID_BTN_EDIT_STEP,
    ID_BTN_DEL_STEP,
    ID_BTN_UP,
    ID_BTN_DOWN,
    ID_BTN_TEST,
    ID_BTN_RECORD,
    ID_BTN_ICON,
    ID_BTN_COLOR,
    ID_BTN_ENGINE,
    ID_BTN_FLOATING,
    ID_BTN_SETTINGS,
    ID_BTN_SIMPLE,
    ID_BTN_ADVANCED,
    // Simple mode's three ways to add a step (recording clicks is the fourth,
    // shared with Advanced).
    ID_BTN_S_TYPE,
    ID_BTN_S_KEYS,
    ID_BTN_S_WAIT,
    ID_DECK_COMBO,
    // The three gestures a tile can carry. One list is shown at a time and
    // every step button below acts on whichever is selected.
    ID_GESTURE_PRESS,
    ID_GESTURE_UP,
    ID_GESTURE_DOWN,

    // The seven key colours, ID_SWATCH_FIRST .. ID_SWATCH_LAST.
    ID_SWATCH_FIRST = 300,
    ID_SWATCH_LAST = ID_SWATCH_FIRST + 6,

    IDM_KB_RENAME = 430,
    IDM_KB_BLOCK,
    IDM_KB_BLOCK_ALL,
    IDM_KB_REMATCH,
    IDM_KB_REMOVE,

    IDM_TILE_RENAME = 450,
    IDM_TILE_ICON,
    IDM_TILE_ICON_CLEAR,
    IDM_TILE_COLOR,
    IDM_TILE_COLOR_CLEAR,
    IDM_TILE_TEST,
    IDM_TILE_CLEAR,
    IDM_TILE_REKEY,
    IDM_TILE_DUPLICATE,
    IDM_TILE_COPY_STEPS,
    IDM_TILE_PASTE_STEPS,
    IDM_TILE_REMOVE,

    IDM_PAGE_RENAME = 470,
    IDM_PAGE_COLOR,
    IDM_PAGE_COLOR_CLEAR,
    IDM_PAGE_APP,
    IDM_PAGE_APP_CLEAR,
    IDM_PAGE_DUPLICATE,
    IDM_PAGE_LEFT,
    IDM_PAGE_RIGHT,
    IDM_PAGE_REMOVE,

    IDM_TRAY_SHOW = 480,
    IDM_TRAY_TOGGLE,
    IDM_TRAY_EXIT,
    IDM_TRAY_MINI,
};

// The system-wide start/stop shortcut. Any id unique within this window will
// do; RegisterHotKey ids are per-window.
constexpr int kToggleHotkeyId = 1;

// Windows says devices changed in bursts - one plug-in is several messages -
// so the keypads are looked for once things have been quiet this long.
constexpr UINT_PTR kDeviceCheckTimer = 7;
constexpr UINT kDeviceCheckDelayMs = 700;

// Design-pixel metrics, from the SMEdevelop design system (tokens.h).
constexpr int kToolbarH = 80;
constexpr int kSidebarW = 296;
// Room for seven 48px colour swatches side by side.
constexpr int kInspectorW = 412;
// The activity log, Advanced only.
constexpr int kLogH = 132;
// A banner, with room inside it for a 48px button.
constexpr int kBannerH = 64;
constexpr int kPad = tk::kSpace5;   // side padding, 24
// How long a toast stays up.
constexpr DWORD kToastMs = 4000;
constexpr UINT_PTR kToastTimer = 8;

// Every key on every page. What "all its keys" means when a keyboard is
// about to be removed, and what an import is really bringing in - the active
// page alone would undercount both.
int totalBindings(const cfg::Keyboard& kb) {
    int n = 0;
    for (const cfg::Page& p : kb.pages) n += (int)p.bindings.size();
    return n;
}

struct DeckTile {
    RECT rect{};
    int slot = -1;          // preset slot index (-1 in flow mode)
    std::string key;        // key this tile represents ("" = not yet assigned)
    int binding = -1;       // index into the active page's bindings, -1 if empty
    bool addTile = false;   // the trailing "+" tile in flow mode
};

// One tab in the strip above the deck. The "+" at the end is a tab too, so
// that hit-testing has one list to walk rather than a list and a special case.
struct PageTab {
    RECT rect{};
    int index = -1;         // -1 = the trailing "add a page" tab
};

struct PaletteRow {
    RECT rect{};
    std::string actionId;
};

struct KbRow {
    RECT rect{};
    int index = -1;         // -1 = the "add keyboard" row
};

std::wstring W(const std::string& s) { return widen(s); }

RectF toRectF(const RECT& r) {
    return RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left),
                 (REAL)(r.bottom - r.top));
}

bool ptIn(const RECT& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

// Width in device pixels of `text` in a control's own font, so a button can be
// sized to whatever label it is currently wearing.
int textWidthOf(HWND ctl, const std::string& text) {
    HDC dc = GetDC(ctl);
    if (!dc) return 0;
    HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(ctl, WM_GETFONT, 0, 0));
    std::wstring w = widen(text);
    SIZE sz{};
    GetTextExtentPoint32W(dc, w.c_str(), (int)w.size(), &sz);
    SelectObject(dc, old);
    ReleaseDC(ctl, dc);
    return sz.cx;
}

// -------------------------------------------------------------- icon cache
class IconCache {
public:
    Image* get(const std::string& relPath) {
        if (trim(relPath).empty()) return nullptr;
        auto it = cache_.find(relPath);
        if (it != cache_.end()) return it->second;
        const std::string full = resolveDataPath(relPath);
        Image* img = nullptr;
        if (fileExists(full)) {
            img = Image::FromFile(W(full).c_str());
            if (img && img->GetLastStatus() != Ok) {
                delete img;
                img = nullptr;
            }
        }
        cache_[relPath] = img;
        return img;
    }
    void clear() {
        for (auto& kv : cache_) delete kv.second;
        cache_.clear();
    }
    ~IconCache() { clear(); }

private:
    std::map<std::string, Image*> cache_;
};

}  // namespace

// ------------------------------------------------------------- main window
namespace {

class MainWindow {
public:
    bool create(HINSTANCE inst, int showCmd);
    HWND hwnd() const { return hwnd_; }
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);

private:
    // ---- lifecycle
    void onCreate();
    void onDestroy();
    bool confirmExit();
    void onDpiChanged(UINT dpi, const RECT* suggested);
    void reportLoadProblems();

    // ---- layout & painting
    void layout();
    void layoutDeck();
    void layoutPages();
    // The listen button's label changes width, so it is sized and placed
    // together rather than pinned to a fixed box.
    std::string engineButtonLabel() const;
    void layoutEngineButton();
    std::string listeningSummary() const;
    void paint(HDC hdc);
    void paintToolbar(Graphics& g);
    void paintSidebar(Graphics& g);
    void paintDeck(Graphics& g);
    void paintPages(Graphics& g);
    void paintTile(Graphics& g, const DeckTile& tile, cfg::Keyboard* kb);
    void paintWelcome(Graphics& g);
    void paintDragGhost(Graphics& g);
    void paintInspector(Graphics& g);
    void paintStatusBanner(Graphics& g);
    void paintToast(Graphics& g);
    // A problem or a notice heads the middle column until it clears or is
    // dismissed; a success is a toast instead.
    bool bannerShown() const;
    BannerKind bannerKind() const;
    void showToast(const std::string& text, bool good);
    // Which of the panel's controls are shown: those of the current mode,
    // and only while a key is picked.
    void showInspectorControls();
    void setKeyColor(int index);

    // ---- model helpers
    cfg::Keyboard* currentKb();
    cfg::Binding* selectedBinding();
    // The step list the inspector is editing: a tile's press, or one of the
    // two wheel directions that make it a dial.
    std::vector<cfg::Step>* editingSteps();
    void setGesture(int gesture);
    void refreshGestureButtons();
    const cfg::DeckPreset* currentPreset();
    void selectKeyboard(const std::string& alias);
    void selectTile(int tileIndex);
    // True if the tile under this point carries a wheel macro in that
    // direction - and, if so, runs it.
    bool tileUnderCursorIsDial(POINT p, bool up);
    void refreshInspector();
    // Enable/disable the step buttons for whatever is selected right now,
    // without rebuilding the list underneath the user's cursor.
    void updateStepButtons();
    void refreshDeckCombo();
    void modelChanged(bool relayoutDeck = true);
    void saveConfig();

    // ---- commands
    void addKeyboard();
    void addMouse();
    // Refresh config_.pointerCount from the machine.
    void countPointers();
    void addSoftwareDeck();
    void keyboardContextMenu(int index, POINT screenPt);
    void tileContextMenu(int tileIndex, POINT screenPt);
    void activateTile(int tileIndex);
    void assignKeyToTile(int tileIndex);
    void addStep(const std::string& presetAction = "");
    void editStep();
    void removeStep();
    void moveStep(int delta);
    // Records clicks onto the selected key. With a range, the new clicks
    // replace those steps instead of being added at the end.
    void recordClicks(int replaceFirst = -1, int replaceCount = 0);
    void testRun();
    void clearTile();
    void deleteBinding();
    void chooseTileIcon();
    void chooseTileColor();
    void toggleEngine();
    void openSettings();
    void importYaml();
    void reloadConfig();
    void exportConfig();
    void loadCopy();
    void duplicateBinding(int tileIndex);
    void copySteps();
    void pasteSteps();

    // ---- pages
    void pageContextMenu(int pageIndex, POINT screenPt);
    void addPage();
    void movePage(int pageIndex, int delta);
    void removePage(int pageIndex);
    void editPageAppRule(int pageIndex);
    // The one door every page change goes through, whether it came from a
    // click, a macro or the application in front.
    void setActivePage(const std::string& alias, int index);
    void onForegroundChanged(HWND fg);
    void drainPageRequests();
    void startForegroundHook();
    void stopForegroundHook();

    // ---- engine plumbing
    void pushLog(const std::string& line);
    void drainLogs();
    void drainKeyEvents();

    // ---- global shortcut
    void applyToggleHotkey();
    void releaseToggleHotkey();

    // Fire a tile's wheel-up or wheel-down macro. Returns false if that
    // tile is not a dial, so the caller can fall back to scrolling.
    bool runDial(const std::string& key, bool up);

    // ---- mini deck
    void toggleMiniDeck();
    void updateFloatingButton();
    void refreshMiniDeck();
    // Fire a binding by key name, for a click on the mini deck.
    void runBindingByKey(const std::string& key);
    void saveMiniPlacement();

    // ---- tray
    bool addTray();          // false if Windows refused the icon
    void removeTray();
    void trayMenu();
    void showWindowAgain();

    // ---- status line and explained problems
    // Plain: an info banner. Good: a toast. Warn, Danger: a banner of that
    // kind.
    enum class StatusTone { Plain, Good, Warn, Danger };
    void setStatus(const std::string& text,
                   StatusTone tone = StatusTone::Plain);
    void drainOutcomes();
    void noticeWhileAway(const std::string& title, const std::string& text);
    bool findRestorableBackup(std::string& path, std::string& date);
    void explainLoadProblem();
    void restoreBackup(const std::string& backup, const std::string& date);

    // ---- first-run setup
    void runSetup();

    // ---- simple mode
    bool simple() const { return config_.settings.simpleMode; }
    void setSimpleMode(bool on);
    void refreshModeButtons();
    void selectStep(size_t index);
    int selectedRow() const;
    void addSimpleStep(int which);
    std::string askCombo();
    void editSimpleRow(int row);
    void removeSimpleRow(int row);
    int deckHeaderH() const;
    int pageStripY() const;
    int pageStripH() const;

    // ---- whether each keypad is there
    void checkDevices(bool announce);

    HWND hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HICON icon_ = nullptr, iconSm_ = nullptr;

    cfg::Config config_;
    eng::Engine engine_;
    IconCache icons_;
    MiniDeck mini_;

    std::string currentAlias_;
    // Which binding the step list was last built for ("alias|key"), so that
    // keeping the selection across a rebuild does not carry step 3 of one key
    // over to step 3 of the next one.
    std::string stepListFor_;
    int selectedTile_ = -1;
    // 0 = press, 1 = wheel up, 2 = wheel down. Reset to press whenever the
    // selection moves, so editing a dial never leaves the next tile you click
    // quietly showing its scroll list instead of its steps.
    int gesture_ = 0;
    int hoverTile_ = -1;
    int hoverPalette_ = -1;
    int hoverKb_ = -1;

    // regions
    RECT rcToolbar_{}, rcSidebar_{}, rcDeck_{}, rcInspector_{}, rcLog_{};
    RECT rcWelcomeBtn_{};
    RECT rcBanner_{}, rcBannerDismiss_{};
    bool hoverDismiss_ = false;
    bool bannerDismissed_ = false;
    // Where the deck's own header starts: below the banner, when there is one.
    int deckTop_ = 0;
    int toolbarPillsLeft_ = 0;
    // Heights in the panel on the right that the painted labels line up with.
    int insHeadY_ = 0, insNameY_ = 0, insColourY_ = 0, captionY_ = 0;
    std::string toastText_;
    bool toastGood_ = false;
    // The banner says a button stopped, so the next one that runs clears it.
    bool statusFromRun_ = false;
    std::vector<DeckTile> tiles_;
    std::vector<PageTab> pageTabs_;
    RECT rcPagePin_{};
    int hoverPageTab_ = -1;      // index into pageTabs_, not into pages
    std::vector<PaletteRow> palette_;
    std::vector<KbRow> kbRows_;
    int paletteScroll_ = 0;
    int paletteContentH_ = 0;
    // The auto deck keeps tiles a comfortable size and scrolls instead of
    // shrinking them away to nothing once there are a lot of keys.
    int deckScroll_ = 0;
    int deckContentH_ = 0;
    int deckViewH_ = 0;

    // drag & drop from the palette
    bool maybeDrag_ = false;
    bool dragging_ = false;
    std::string dragAction_;
    POINT dragStart_{}, dragPos_{};

    // controls
    HWND titleEdit_ = nullptr, stepList_ = nullptr, logEdit_ = nullptr;
    HWND deckCombo_ = nullptr;
    HWND btnGesturePress_ = nullptr, btnGestureUp_ = nullptr,
         btnGestureDown_ = nullptr;
    HWND btnAddStep_ = nullptr, btnEditStep_ = nullptr, btnDelStep_ = nullptr;
    HWND btnUp_ = nullptr, btnDown_ = nullptr, btnTest_ = nullptr;
    HWND btnRecord_ = nullptr;
    HWND btnIcon_ = nullptr, btnColor_ = nullptr;
    HWND btnEngine_ = nullptr, btnFloating_ = nullptr, btnSettings_ = nullptr;
    HWND btnSimple_ = nullptr, btnAdvanced_ = nullptr;
    HWND btnSType_ = nullptr, btnSKeys_ = nullptr, btnSWait_ = nullptr;
    std::vector<HWND> swatches_;   // the seven key colours, in tk order
    // What the step list shows in Simple mode, one entry per list row.
    std::vector<smp::Row> simpleRows_;
    // The pause the click recorder last used, offered again next time.
    rec::Speed recordSpeed_ = rec::Speed::Normal;
    std::vector<HWND> inspectorLabels_;

    // async plumbing
    std::mutex logMu_;
    std::deque<std::string> logQueue_;
    std::mutex pageMu_;
    std::deque<std::pair<std::string, int>> pageQueue_;
    std::mutex keyMu_;
    std::deque<eng::MonitorEvent> keyQueue_;
    std::string lastKeyText_;
    std::map<std::string, DWORD> lastFire_;   // "alias|key" -> tick
    std::mutex outcomeMu_;
    std::deque<eng::Outcome> outcomes_;

    std::string statusText_;
    StatusTone statusTone_ = StatusTone::Plain;
    ULONGLONG lastAwayNotice_ = 0;
    // The save problem already explained this run, so a save that keeps
    // failing is one dialog and then the status line, not a dialog per edit.
    fe::Problem saveProblemShown_ = fe::Problem::None;
    bool destroying_ = false;
    // The start/stop shortcut actually registered: the saved one, or a
    // stand-in while another program owns it. Empty when there is none.
    std::string activeHotkey_;
    // Which keypads have a device attached right now, by deck name. Software
    // buttons are never in it; a deck not in it has not been looked for yet
    // and is drawn as present.
    std::map<std::string, bool> present_;

    // The application the pages were last matched against, as "exe|class|
    // title". Compared before doing anything, so that clicking around inside
    // one program is not a stream of page changes - and so a page picked by
    // hand survives until the front window is really a different one.
    std::string lastForeground_;
    HWINEVENTHOOK fgHook_ = nullptr;

    bool trayAdded_ = false;
    bool quitting_ = false;
    bool hotkeyRegistered_ = false;
    bool dailyBackupDone_ = false;
    std::vector<cfg::Step> stepClipboard_;
    std::vector<std::string> deckIds_;
};

MainWindow* g_main = nullptr;

// Windows tells us the foreground window changed. Hooked OUTOFCONTEXT, so it
// already arrives on this thread - but it is posted rather than acted on here,
// because a page change repaints the deck and this can fire in the middle of
// anything.
void CALLBACK foregroundEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                  LONG idObject, LONG idChild, DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    // Only the window itself. A menu opening or a control taking focus inside
    // the same program is not a change of application.
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (!hwnd || !g_main || !g_main->hwnd()) return;
    PostMessageW(g_main->hwnd(), WM_APP_FOREGROUND, (WPARAM)hwnd, 0);
}

// ------------------------------------------------------------------ create
bool MainWindow::create(HINSTANCE inst, int showCmd) {
    inst_ = inst;
    g_main = this;

    icon_ = makeAppIcon(256);
    iconSm_ = makeAppIcon(32);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MainWindow::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"DentalMacrosMain";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = icon_;
    wc.hIconSm = iconSm_;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    // Centre on the primary monitor's work area, and never larger than it -
    // the default size is bigger than a small laptop screen at 150% scaling.
    // The window opens at its full size even though what's inside is drawn
    // smaller (kUiSize), so the keypad gets the room.
    RECT work = workAreaNear(nullptr);
    int w = 0, h = 0;
    {
        const FullSize fullSize;
        w = std::min<int>(S(1280), work.right - work.left);
        h = std::min<int>(S(880), work.bottom - work.top);
    }
    int sx = work.left + ((work.right - work.left) - w) / 2;
    int sy = work.top + ((work.bottom - work.top) - h) / 2;

    // WS_EX_CONTROLPARENT so IsDialogMessage in the main loop will walk into
    // the child controls when the user presses Tab.
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, L"DentalMacrosMain", L"Dental Macros",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, sx, sy, w, h,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    applyFrame(hwnd_);

    // Start hidden only if there is actually a tray icon to come back from.
    bool startHidden = config_.settings.startMinimized;
    if (startHidden && !addTray()) {
        startHidden = false;
        pushLog("Windows would not provide a notification-area icon, so "
                "Dental Macros opened its window instead of starting minimised.");
    }
    ShowWindow(hwnd_, startHidden ? SW_HIDE : showCmd);
    UpdateWindow(hwnd_);
    return true;
}

void MainWindow::onCreate() {
    // DPI: everything downstream scales off this.
    UINT dpi = GetDpiForWindow(hwnd_);
    if (dpi >= 72 && dpi <= 480) setDpiScale(dpi / 96.0f);

    config_.load(dataPath("macros.json"));
    countPointers();
    if (dataDirIsFallback()) {
        pushLog("Dental Macros' own folder (" + exeDir() +
                ") can't be changed, so your settings are kept in " +
                dataDir() + ".");
        setStatus("Your settings are kept in your Windows profile, because "
                  "Dental Macros' own folder can't be changed.");
    }

    engine_.setLog([this](const std::string& m) { pushLog(m); });
    engine_.setMonitor([this](const eng::MonitorEvent& e) {
        {
            std::lock_guard<std::mutex> lk(keyMu_);
            if (keyQueue_.size() > 200) keyQueue_.pop_front();
            keyQueue_.push_back(e);
        }
        PostMessageW(hwnd_, WM_APP_KEYEVENT, 0, 0);
    });
    engine_.setPageChanger([this](const std::string& alias, int page) {
        {
            std::lock_guard<std::mutex> lk(pageMu_);
            pageQueue_.push_back({alias, page});
        }
        PostMessageW(hwnd_, WM_APP_PAGE, 0, 0);
    });
    engine_.setOutcome([this](const eng::Outcome& o) {
        {
            std::lock_guard<std::mutex> lk(outcomeMu_);
            if (outcomes_.size() > 50) outcomes_.pop_front();
            outcomes_.push_back(o);
        }
        PostMessageW(hwnd_, WM_APP_OUTCOME, 0, 0);
    });
    engine_.updateConfig(config_);

    // ---- toolbar controls
    // The listen button, then three pills - the most the top bar may carry.
    btnEngine_ = makeButton(hwnd_, "Start listening", 0, 0, 160, 48,
                            ID_BTN_ENGINE, true);
    btnFloating_ = makeButton(hwnd_, "Floating buttons", 0, 0, 176, 48,
                              ID_BTN_FLOATING);
    makePill(btnFloating_);
    btnSettings_ = makeButton(hwnd_, "Settings", 0, 0, 92, 48, ID_BTN_SETTINGS);
    makePill(btnSettings_);
    btnSimple_ = makeButton(hwnd_, "Simple", 0, 0, 76, 48, ID_BTN_SIMPLE);
    makePill(btnSimple_, PillPart::First);
    btnAdvanced_ = makeButton(hwnd_, "Advanced", 0, 0, 96, 48, ID_BTN_ADVANCED);
    makePill(btnAdvanced_, PillPart::Last);

    // ---- deck controls
    std::vector<std::string> deckLabels;
    for (const auto& p : cfg::deckPresets()) {
        deckIds_.push_back(p.id);
        deckLabels.push_back(p.label);
    }
    deckCombo_ = makeCombo(hwnd_, 0, 0, 260, 40, ID_DECK_COMBO, deckLabels, 0);
    setComboHeight(deckCombo_, 40);
    setTextStyle(deckCombo_, tk::Text::Compact);
    // It sits on the page itself, not on a panel.
    setSurface(deckCombo_, toColorRef(tok().surface));

    // ---- inspector controls
    titleEdit_ = makeEdit(hwnd_, "", 0, 0, 100, 48, ID_TITLE_EDIT);
    btnIcon_ = makeButton(hwnd_, "Choose an icon", 0, 0, 90, 40, ID_BTN_ICON);
    btnColor_ = makeButton(hwnd_, "Colour...", 0, 0, 90, 40, ID_BTN_COLOR);
    for (size_t i = 0; i < tk::keyColors().size(); ++i) {
        const tk::KeyColor& kc = tk::keyColors()[i];
        swatches_.push_back(makeSwatch(hwnd_, 0, 0, 48,
                                       ID_SWATCH_FIRST + (int)i, kc.hex, kc.name));
    }
    btnGesturePress_ = makeButton(hwnd_, "Press", 0, 0, 76, 40,
                                  ID_GESTURE_PRESS);
    makePill(btnGesturePress_, PillPart::First);
    btnGestureUp_ = makeButton(hwnd_, "Turn up", 0, 0, 76, 40, ID_GESTURE_UP);
    makePill(btnGestureUp_, PillPart::Middle);
    btnGestureDown_ = makeButton(hwnd_, "Turn down", 0, 0, 76, 40,
                                 ID_GESTURE_DOWN);
    makePill(btnGestureDown_, PillPart::Last);
    stepList_ = makeList(hwnd_, 0, 0, 100, 100, ID_STEP_LIST);
    btnAddStep_ = makeButton(hwnd_, "Add step", 0, 0, 90, 40, ID_BTN_ADD_STEP);
    setTone(btnAddStep_, Tone::Secondary);
    setButtonIcon(btnAddStep_, "plus");
    btnEditStep_ = makeButton(hwnd_, "Change", 0, 0, 68, 40, ID_BTN_EDIT_STEP);
    btnDelStep_ = makeButton(hwnd_, "Remove step", 0, 0, 78, 40, ID_BTN_DEL_STEP);
    setTone(btnDelStep_, Tone::Danger);
    btnUp_ = makeButton(hwnd_, "Move up", 0, 0, 52, 40, ID_BTN_UP);
    btnDown_ = makeButton(hwnd_, "Move down", 0, 0, 62, 40, ID_BTN_DOWN);
    btnTest_ = makeButton(hwnd_, "Try it", 0, 0, 100, 40, ID_BTN_TEST);
    btnRecord_ = makeButton(hwnd_, "Record clicks", 0, 0, 100, 48,
                            ID_BTN_RECORD);
    btnSType_ = makeButton(hwnd_, "Type text", 0, 0, 100, 48, ID_BTN_S_TYPE);
    btnSKeys_ = makeButton(hwnd_, "Press keys", 0, 0, 100, 48, ID_BTN_S_KEYS);
    btnSWait_ = makeButton(hwnd_, "Wait", 0, 0, 100, 48, ID_BTN_S_WAIT);
    for (HWND h : {btnRecord_, btnSType_, btnSKeys_, btnSWait_})
        setTone(h, Tone::Secondary);
    for (HWND h : {btnSType_, btnSKeys_, btnSWait_}) setButtonIcon(h, "plus");

    // ---- log
    logEdit_ = makeEdit(hwnd_, "", 0, 0, 100, 100, ID_LOG_EDIT, true, true);

    if (!config_.keyboards.empty()) selectKeyboard(config_.keyboards[0].alias);

    reportLoadProblems();
    applyToggleHotkey();
    checkDevices(true);
    startForegroundHook();
    // Start on the page the application already in front asks for, rather
    // than waiting for the user to alt-tab once before the rules mean
    // anything.
    onForegroundChanged(GetForegroundWindow());

    layout();
    // The keys only have places once the window has a size, so the first
    // key can only be picked now - and an empty panel on the right is a
    // poor first sight.
    if (selectedTile_ < 0)
        for (size_t i = 0; i < tiles_.size(); ++i)
            if (tiles_[i].binding >= 0) {
                selectedTile_ = (int)i;
                break;
            }
    refreshInspector();
    refreshModeButtons();

    // ---- the always-on-top palette
    mini_.setRunner([this](const std::string& key) { runBindingByKey(key); });
    mini_.setOpener([this] { showWindowAgain(); });
    mini_.setDialer([this](const std::string& key, bool up) {
        return runDial(key, up);
    });
    mini_.setPageStepper([this](int delta) {
        cfg::Keyboard* kb = currentKb();
        if (!kb || kb->pages.size() < 2) return;
        const int n = (int)kb->pages.size();
        // Wrapping, because on a strip this narrow the alternative is an
        // arrow that does nothing at each end.
        setActivePage(kb->alias, (kb->activePage + delta + n) % n);
    });
    // Written out as soon as it settles, not only at exit: a palette dragged
    // somewhere better should still be there after a power cut.
    mini_.setOnPlacementChanged([this] {
        saveMiniPlacement();
        if (config_.dirty) saveConfig();
        // Shown or hidden from anywhere - the tray, its own menu - the
        // toolbar button says what pressing it will do now.
        updateFloatingButton();
    });
    {
        MiniPlacement p;
        p.visible = config_.settings.miniDeckVisible;
        p.x = config_.settings.miniDeckX;
        p.y = config_.settings.miniDeckY;
        p.tile = config_.settings.miniDeckTile;
        p.cols = config_.settings.miniDeckCols;
        mini_.setPlacement(p);
    }
    refreshMiniDeck();
    if (config_.settings.miniDeckVisible) mini_.show(inst_);
    updateFloatingButton();

    if (config_.settings.startEngineOnLaunch && !config_.keyboards.empty())
        toggleEngine();

    // Nothing set up and never offered: offer it, once the window is up.
    if (config_.loadError.empty() && config_.keyboards.empty() &&
        !config_.settings.setupDone)
        PostMessageW(hwnd_, WM_APP_SETUP, 0, 0);
}

void MainWindow::reportLoadProblems() {
    for (const std::string& w : config_.loadWarnings)
        pushLog("macros.json: " + w);

    if (config_.loadError.empty()) {
        pushLog(config_.keyboards.empty() ? "Dental Macros ready."
                                          : "Loaded macros.json.");
        return;
    }

    pushLog("Could not read macros.json: " + config_.loadError);
    pushLog("  The file has not been touched. If anything is saved before it "
            "is fixed, it is copied to macros.json.bak first.");
    setStatus("Your buttons couldn't be loaded. The file is kept as it was.", StatusTone::Danger);
    // Explained once the window is up: the fixes on offer reload the
    // settings, which can't happen halfway through creating the window.
    PostMessageW(hwnd_, WM_APP_LOADPROBLEM, 0, 0);
}

void MainWindow::onDestroy() {
    destroying_ = true;
    releaseToggleHotkey();
    stopForegroundHook();
    // Before the config is out of reach: the palette knows where it ended up.
    saveMiniPlacement();
    if (config_.dirty) saveConfig();
    mini_.destroy();
    engine_.stop();
    removeTray();
    icons_.clear();
    releaseFonts();
    if (icon_) DestroyIcon(icon_);
    if (iconSm_) DestroyIcon(iconSm_);
}

// The manifest asks for PerMonitorV2 awareness, which means Windows scales
// nothing for us: every font, control and metric here is ours to redo when the
// window lands on a monitor with different scaling.
void MainWindow::onDpiChanged(UINT dpi, const RECT* suggested) {
    if (dpi < 72 || dpi > 480) return;
    setDpiScale(dpi / 96.0f);
    rescaleControlFonts(hwnd_);
    if (suggested)
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    layout();   // repositions everything at the new scale
    InvalidateRect(hwnd_, nullptr, TRUE);
}

// ------------------------------------------------------------------ layout
namespace {

// Width of `text` in the given font, in device pixels.
int textPx(HWND ref, const std::string& text, int px, int weight) {
    HDC dc = GetDC(ref);
    if (!dc) return 0;
    HGDIOBJ old = SelectObject(dc, uiFontWeight(px, weight));
    std::wstring w = widen(text);
    SIZE sz{};
    GetTextExtentPoint32W(dc, w.c_str(), (int)w.size(), &sz);
    SelectObject(dc, old);
    ReleaseDC(ref, dc);
    return sz.cx;
}

// A pill sized to its label: the pills' own 16px bold, 18px either side and
// the edges.
int pillW(HWND ref, const std::string& text) {
    return textPx(ref, text, 16, 700) + S(18) * 2 + S(24);
}

const Color kWhite(255, 255, 255, 255);

}  // namespace

bool MainWindow::bannerShown() const {
    return !statusText_.empty() && !bannerDismissed_ &&
           statusTone_ != StatusTone::Good;
}

BannerKind MainWindow::bannerKind() const {
    return statusTone_ == StatusTone::Danger ? BannerKind::Danger
           : statusTone_ == StatusTone::Warn ? BannerKind::Warning
                                             : BannerKind::Info;
}

void MainWindow::layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int W_ = rc.right, H_ = rc.bottom;

    const bool sm = simple();
    auto show = [](HWND h, bool on) { ShowWindow(h, on ? SW_SHOW : SW_HIDE); };
    // Advanced mode's own controls are compact (docs/REDESIGN.md section 6);
    // everything Simple mode shows is the full 48px.
    const int ctl = sm ? tk::kTargetMin : tk::kTargetCompact;
    const int ctlH = S(ctl);
    const tk::Text btnText = sm ? tk::Text::Button : tk::Text::CompactBold;

    rcToolbar_ = {0, 0, W_, S(kToolbarH)};
    // Simple mode keeps no log: what it would say is in the banner and the
    // toasts.
    rcLog_ = sm ? RECT{0, H_, W_, H_} : RECT{0, H_ - S(kLogH), W_, H_};
    rcSidebar_ = {0, S(kToolbarH), S(kSidebarW), rcLog_.top};
    rcInspector_ = {W_ - S(kInspectorW), S(kToolbarH), W_, rcLog_.top};
    rcDeck_ = {rcSidebar_.right, S(kToolbarH), rcInspector_.left, rcLog_.top};

    // The banner, while there is one, heads the middle column: above the keys
    // it is about, without squeezing the panel beside them.
    rcBanner_ = RECT{};
    rcBannerDismiss_ = RECT{};
    deckTop_ = rcDeck_.top;
    if (bannerShown()) {
        const int left = rcDeck_.left + S(kPad), right = rcDeck_.right - S(kPad);
        const int dw = textPx(hwnd_, "Dismiss", 18, 600) + S(40);
        // Tall enough for the whole sentence, in the banner's 17px bold,
        // between its icon and the Dismiss button.
        const int textW = (right - left) - S(16 + 22 + 12) - S(16) - (dw + S(8));
        int textH = 0;
        if (HDC dc = GetDC(hwnd_)) {
            HGDIOBJ old = SelectObject(dc, uiFontWeight(17, 700));
            RECT tr{0, 0, std::max(S(80), textW), 0};
            std::wstring t = W(statusText_);
            DrawTextW(dc, t.c_str(), -1, &tr,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            textH = tr.bottom - tr.top;
            SelectObject(dc, old);
            ReleaseDC(hwnd_, dc);
        }
        const int h = std::max(S(kBannerH), textH + S(24));
        rcBanner_ = {left, rcDeck_.top + S(16), right, rcDeck_.top + S(16) + h};
        const int dh = S(tk::kTargetMin);
        const int dy = rcBanner_.top + (h - dh) / 2;
        rcBannerDismiss_ = {rcBanner_.right - S(8) - dw, dy, rcBanner_.right - S(8),
                            dy + dh};
        deckTop_ = rcBanner_.bottom;
    }

    // ---- top bar: three pills at the right, the listen button before them
    const int bh = S(tk::kTargetMin);
    const int by = (S(kToolbarH) - bh) / 2;
    int x = W_ - S(kPad);
    const int wSettings = pillW(hwnd_, "Settings");
    x -= wSettings;
    MoveWindow(btnSettings_, x, by, wSettings, bh, TRUE);
    const int wAdv = pillW(hwnd_, "Advanced"), wSim = pillW(hwnd_, "Simple");
    x -= S(12) + wAdv;
    MoveWindow(btnAdvanced_, x, by, wAdv, bh, TRUE);
    x -= wSim;
    MoveWindow(btnSimple_, x, by, wSim, bh, TRUE);
    const int wFloat = pillW(hwnd_, "Floating buttons");
    x -= S(12) + wFloat;
    MoveWindow(btnFloating_, x, by, wFloat, bh, TRUE);
    toolbarPillsLeft_ = x;
    layoutEngineButton();

    // ---- deck header. A combo box's window height controls how far its
    // list drops down; the closed box is set by setComboHeight.
    MoveWindow(deckCombo_, rcDeck_.left + S(kPad), deckTop_ + S(16), S(260),
               S(40) * 9, TRUE);
    show(deckCombo_, !sm);

    // ---- the selected key, on the right
    const int ix = rcInspector_.left + S(kPad);
    const int iw = S(kInspectorW) - S(kPad) * 2;
    int y = rcInspector_.top + S(24);
    insHeadY_ = y;                         // heading, 32
    y += S(32) + S(16);
    insNameY_ = y;                         // "Name on the button", 20
    y += S(20) + S(6);
    MoveWindow(titleEdit_, ix, y, iw, ctlH, TRUE);
    setTextStyle(titleEdit_, sm ? tk::Text::Body : tk::Text::Compact);
    y += ctlH + S(16);
    insColourY_ = y;                       // "Colour - Walnut", 20
    y += S(20) + S(6);
    const int sgap = (iw - ctlH * 7) / 6;
    for (size_t i = 0; i < swatches_.size(); ++i)
        MoveWindow(swatches_[i], ix + (int)i * (ctlH + sgap), y, ctlH, ctlH,
                   TRUE);
    y += ctlH + S(20);
    show(btnColor_, false);

    const int hw = (iw - S(12)) / 2;
    const int x2 = ix + hw + S(12);
    if (sm) {
        show(btnIcon_, false);
        MoveWindow(btnRecord_, ix, y, hw, ctlH, TRUE);
        MoveWindow(btnSKeys_, x2, y, hw, ctlH, TRUE);
        MoveWindow(btnSType_, ix, y + ctlH + S(12), hw, ctlH, TRUE);
        MoveWindow(btnSWait_, x2, y + ctlH + S(12), hw, ctlH, TRUE);
        y += ctlH * 2 + S(12) + S(24);
    } else {
        // Advanced's panel holds more, so the two picture-and-recording
        // buttons share a row.
        show(btnIcon_, true);
        MoveWindow(btnIcon_, ix, y, hw, ctlH, TRUE);
        MoveWindow(btnRecord_, x2, y, hw, ctlH, TRUE);
        y += ctlH + S(12);
        const int gw = iw / 3;
        MoveWindow(btnGesturePress_, ix, y, gw, ctlH, TRUE);
        MoveWindow(btnGestureUp_, ix + gw, y, gw, ctlH, TRUE);
        MoveWindow(btnGestureDown_, ix + gw * 2, y, iw - gw * 2, ctlH, TRUE);
        y += ctlH + S(20);
    }
    for (HWND h : {btnGesturePress_, btnGestureUp_, btnGestureDown_})
        show(h, !sm);

    captionY_ = y;                         // "WHAT IT DOES", 18
    y += S(18) + S(8);
    const int rows = sm ? 1 : 2;
    const int listBottom = rcInspector_.bottom - S(kPad) - ctlH * rows -
                           S(8) * (rows - 1) - S(16);
    MoveWindow(stepList_, ix, y, iw, std::max(S(80), listBottom - y), TRUE);
    setListRowHeight(stepList_, ctl);
    setTextStyle(stepList_, sm ? tk::Text::Body : tk::Text::Compact);

    const int by2 = listBottom + S(16);
    if (sm) {
        MoveWindow(btnTest_, ix, by2, hw, ctlH, TRUE);
        MoveWindow(btnDelStep_, x2, by2, hw, ctlH, TRUE);
    } else {
        const int tw = (iw - S(8) * 2) / 3;
        auto col = [&](int c) { return ix + c * (tw + S(8)); };
        MoveWindow(btnAddStep_, col(0), by2, tw, ctlH, TRUE);
        MoveWindow(btnEditStep_, col(1), by2, tw, ctlH, TRUE);
        MoveWindow(btnDelStep_, col(2), by2, tw, ctlH, TRUE);
        const int by3 = by2 + ctlH + S(8);
        MoveWindow(btnUp_, col(0), by3, tw, ctlH, TRUE);
        MoveWindow(btnDown_, col(1), by3, tw, ctlH, TRUE);
        MoveWindow(btnTest_, col(2), by3, tw, ctlH, TRUE);
    }
    for (HWND h : {btnSType_, btnSKeys_, btnSWait_}) show(h, sm);
    for (HWND h : {btnAddStep_, btnEditStep_, btnUp_, btnDown_}) show(h, !sm);
    for (HWND h : {btnSType_, btnSKeys_, btnSWait_, btnRecord_, btnTest_,
                   btnDelStep_, btnAddStep_, btnEditStep_, btnUp_, btnDown_,
                   btnIcon_})
        setTextStyle(h, btnText);
    for (HWND h : {btnGesturePress_, btnGestureUp_, btnGestureDown_})
        setTextStyle(h, tk::Text::CompactBold);
    // Simple's four ways to add a step all start with "+"; Advanced records
    // clicks from a button of its own.
    setButtonIcon(btnRecord_, sm ? "plus" : "click");

    // ---- the activity log, Advanced only
    MoveWindow(logEdit_, S(kPad), rcLog_.top + S(44), W_ - S(kPad) * 2,
               S(kLogH) - S(44) - S(16), TRUE);
    setTextStyle(logEdit_, tk::Text::Compact);
    show(logEdit_, !sm);

    layoutDeck();
    showInspectorControls();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::showInspectorControls() {
    const bool sm = simple();
    const bool has = selectedBinding() != nullptr;
    auto show = [](HWND h, bool on) {
        if (h) ShowWindow(h, on ? SW_SHOW : SW_HIDE);
    };
    show(titleEdit_, has);
    for (HWND h : swatches_) show(h, has);
    show(btnColor_, false);
    show(stepList_, has);
    show(btnTest_, has);
    show(btnDelStep_, has);
    show(btnRecord_, has);
    for (HWND h : {btnSType_, btnSKeys_, btnSWait_}) show(h, has && sm);
    for (HWND h : {btnIcon_, btnGesturePress_, btnGestureUp_, btnGestureDown_,
                   btnAddStep_, btnEditStep_, btnUp_, btnDown_})
        show(h, has && !sm);

    // Listening is for keypads. With none, there is nothing to start - and
    // the welcome card's "Get started" is this window's one main action.
    bool keypads = false;
    for (const auto& kb : config_.keyboards)
        if (!kb.softwareOnly) keypads = true;
    show(btnEngine_, keypads || engine_.running());
}

std::string MainWindow::engineButtonLabel() const {
    if (!engine_.running()) return "Start listening";
    // Show the way out only while there is something to escape from, and only
    // when the shortcut really is ours - if another program claimed it,
    // printing it here would be a lie.
    if (hotkeyRegistered_ && !activeHotkey_.empty())
        return "Stop listening (" + keys::prettyCombo(activeHotkey_) + ")";
    return "Stop listening";
}

void MainWindow::layoutEngineButton() {
    if (!btnEngine_ || !hwnd_) return;
    const std::string label = engineButtonLabel();
    setText(btnEngine_, label);
    // The one main action while nothing is listening; once it is, stopping
    // is a quiet choice, not the thing this window is for.
    setTone(btnEngine_, engine_.running() ? Tone::Quiet : Tone::Primary);
    setTextStyle(btnEngine_, tk::Text::Button);

    const int bh = S(tk::kTargetMin);
    const int by = (S(kToolbarH) - bh) / 2;
    const int w = std::max(S(160), textPx(hwnd_, label, 18, 600) + S(48));
    MoveWindow(btnEngine_, toolbarPillsLeft_ - S(16) - w, by, w, bh, TRUE);
}

std::string MainWindow::listeningSummary() const {
    std::vector<const cfg::Keyboard*> pads;
    for (const auto& k : config_.keyboards)
        if (!k.softwareOnly) pads.push_back(&k);
    if (pads.empty()) return "Listening";
    if (pads.size() == 1) return "Listening to " + pads[0]->alias;
    return "Listening to " + std::to_string(pads.size()) + " keypads";
}

// How far below deckTop_ the tiles begin, and where the page pills sit, in
// design px. Advanced has the layout picker in the first band and the page
// pills in the second; Simple has a line of help, then the pages only when
// a keypad already has several.
int MainWindow::deckHeaderH() const {
    if (!simple()) return 16 + 40 + 12 + 40 + 16;
    bool pages = false;
    for (const cfg::Keyboard& kb : config_.keyboards)
        if (kb.alias == currentAlias_) pages = kb.pages.size() > 1;
    return pages ? 16 + 26 + 12 + 48 + 16 : 16 + 26 + 16;
}

int MainWindow::pageStripY() const { return simple() ? 16 + 26 + 12 : 16 + 40 + 12; }
int MainWindow::pageStripH() const {
    return simple() ? tk::kTargetMin : tk::kTargetCompact;
}

void MainWindow::layoutPages() {
    pageTabs_.clear();
    rcPagePin_ = RECT{};

    cfg::Keyboard* kb = currentKb();
    if (!kb) return;
    kb->normalizePages();

    // Simple mode makes no pages, but a deck that already has several still
    // shows them, so a page changing by itself is never a mystery. No "Add
    // page" and no follow switch there: those are for building pages.
    if (simple() && kb->pages.size() < 2) return;

    const int y = deckTop_ + S(pageStripY());
    const int h = S(pageStripH());
    // The follow switch is pinned to the right end, and the page pills stop
    // short of it rather than sliding underneath.
    if (!simple()) {
        const int pinW = pillW(hwnd_, "Follow the program in front") + S(26);
        rcPagePin_ = {rcDeck_.right - S(kPad) - pinW, y, rcDeck_.right - S(kPad),
                      y + h};
    }
    const int limit =
        simple() ? rcDeck_.right - S(kPad) : rcPagePin_.left - S(12);

    int x = rcDeck_.left + S(kPad);
    auto place = [&](int index, int w) {
        if (x + w > limit && !pageTabs_.empty()) return false;
        PageTab t;
        t.index = index;
        t.rect = {x, y, x + w, y + h};
        pageTabs_.push_back(t);
        x += w + S(8);
        return true;
    };
    for (size_t i = 0; i < kb->pages.size(); ++i) {
        const cfg::Page& p = kb->pages[i];
        // Room for the page's colour dot and for the mark of a page that a
        // program brings up by itself.
        const int w = pillW(hwnd_, p.displayName()) +
                      (trim(p.color).empty() ? 0 : S(18)) +
                      (p.rule.empty() ? 0 : S(24));
        if (!place((int)i, w)) break;
    }
    if (!simple()) place(-1, pillW(hwnd_, "Add page") + S(26));
}

void MainWindow::layoutDeck() {
    tiles_.clear();
    rcWelcomeBtn_ = RECT{};
    deckContentH_ = 0;
    deckViewH_ = 0;

    layoutPages();

    cfg::Keyboard* kb = currentKb();
    if (!kb) {
        deckScroll_ = 0;
        return;
    }

    const cfg::DeckPreset* preset = currentPreset();
    RECT area = rcDeck_;
    area.left += S(kPad);
    area.right -= S(kPad);
    area.top = deckTop_ + S(deckHeaderH());
    area.bottom -= S(kPad);
    const int availW = area.right - area.left;
    const int availH = area.bottom - area.top;
    if (availW < S(80) || availH < S(80)) return;

    const int gap = S(tk::kSpace4);
    deckViewH_ = availH;

    if (preset->flow) {
        // Flow layout: every binding in order, plus a trailing "add" tile.
        // Columns at least 150px, so a label in the key's 22px type fits.
        int count = (int)kb->active().bindings.size() + 1;
        int cols = std::max(1, std::min(count, (availW + gap) / (S(150) + gap)));
        int rows = (count + cols - 1) / cols;
        // Size tiles from the width alone and scroll if the rows overflow.
        // Squeezing them to fit the height instead is what made a deck with
        // many keys unusable - past about thirty they either vanished or ran
        // off the bottom into the log.
        int byWidth = (availW - gap * (cols - 1)) / cols;
        int side = std::max(S(112), std::min(byWidth, S(180)));
        const int tileW = side, tileH = side;

        deckContentH_ = rows * tileH + gap * std::max(0, rows - 1);
        deckScroll_ = std::max(0, std::min(deckScroll_,
                                           deckContentH_ - availH));

        for (int i = 0; i < count; ++i) {
            int r = i / cols, c = i % cols;
            const int x0 = area.left + c * (tileW + gap);
            const int y0 = area.top + r * (tileH + gap) - deckScroll_;
            DeckTile t;
            t.rect = {x0, y0, x0 + tileW, y0 + tileH};
            if (i < (int)kb->active().bindings.size()) {
                t.binding = i;
                t.key = kb->active().bindings[(size_t)i].key;
            } else {
                t.addTile = true;
            }
            tiles_.push_back(t);
        }
        return;
    }

    // Fixed grid: size tiles so the whole preset fits the available area -
    // but never smaller than a key whose label can still be read. Below
    // that, the keys keep their size and the deck scrolls.
    int cols = preset->cols, rows = preset->rows;
    int tileW = (availW - gap * (cols - 1)) / cols;
    int tileH = (availH - gap * (rows - 1)) / rows;
    // Always as many columns as fit across, though: a key cut off at the
    // side can't be scrolled to.
    int side = std::max(std::min(S(92), tileW), std::min({tileW, tileH, S(160)}));
    int gridW = side * cols + gap * (cols - 1);
    const int gridH = side * rows + gap * (rows - 1);
    deckContentH_ = gridH;
    deckScroll_ = std::max(0, std::min(deckScroll_, gridH - availH));
    int ox = area.left + std::max(0, (availW - gridW) / 2);
    int oy = area.top - deckScroll_;

    for (size_t i = 0; i < preset->slots.size(); ++i) {
        const cfg::DeckSlot& s = preset->slots[i];
        DeckTile t;
        t.slot = (int)i;
        t.key = s.key;
        t.rect = {ox + s.col * (side + gap), oy + s.row * (side + gap),
                  ox + s.col * (side + gap) + side * s.colSpan +
                      gap * (s.colSpan - 1),
                  oy + s.row * (side + gap) + side * s.rowSpan +
                      gap * (s.rowSpan - 1)};

        // Presets with real key names bind by key; blank grids bind by slot.
        for (size_t b = 0; b < kb->active().bindings.size(); ++b) {
            const cfg::Binding& bind = kb->active().bindings[b];
            if (preset->fixedKeys ? (bind.key == s.key)
                                  : (bind.slot == (int)i)) {
                t.binding = (int)b;
                t.key = bind.key;
                break;
            }
        }
        tiles_.push_back(t);
    }
}

// ---------------------------------------------------------------- painting
void MainWindow::paint(HDC hdc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int W_ = rc.right, H_ = rc.bottom;
    if (W_ <= 0 || H_ <= 0) return;

    // Double-buffer: one memory bitmap for the whole client area.
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W_, H_);
    HGDIOBJ old = SelectObject(mem, bmp);

    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        const Tok& k = tok();

        SolidBrush bg(k.surface);
        g.FillRectangle(&bg, 0, 0, W_, H_);

        paintToolbar(g);
        paintSidebar(g);
        if (config_.keyboards.empty())
            paintWelcome(g);
        else
            paintDeck(g);
        if (bannerShown()) paintStatusBanner(g);
        paintInspector(g);

        if (!simple()) {
            SolidBrush raised(k.surfaceRaised);
            g.FillRectangle(&raised, toRectF(rcLog_));
            Pen line(k.line, 1.0f);
            g.DrawLine(&line, 0.0f, (REAL)rcLog_.top, (REAL)W_, (REAL)rcLog_.top);
            drawStyled(g, L"Activity",
                       RectF((REAL)S(kPad), (REAL)(rcLog_.top + S(16)), (REAL)S(300),
                             (REAL)S(18)),
                       tk::Text::Overline, k.inkMuted);
        }

        paintToast(g);
        if (dragging_) paintDragGhost(g);
    }

    BitBlt(hdc, 0, 0, W_, H_, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void MainWindow::paintInspector(Graphics& g) {
    const Tok& k = tok();
    SolidBrush raised(k.surfaceRaised);
    g.FillRectangle(&raised, toRectF(rcInspector_));
    Pen line(k.line, 1.0f);
    g.DrawLine(&line, (REAL)rcInspector_.left, (REAL)rcInspector_.top,
               (REAL)rcInspector_.left, (REAL)rcInspector_.bottom);

    const REAL ix = (REAL)(rcInspector_.left + S(kPad));
    const REAL iw = (REAL)(S(kInspectorW) - S(kPad) * 2);
    cfg::Binding* b = selectedBinding();
    const cfg::Keyboard* kb = currentKb();

    std::wstring head = L"Nothing selected";
    if (b && kb && kb->softwareOnly) head = L"On-screen button";
    else if (b) head = W(keys::displayName(b->key));
    drawStyled(g, head, RectF(ix, (REAL)insHeadY_, iw, (REAL)S(32)),
               tk::Text::Title, b ? k.ink : k.inkMuted);

    if (!b) {
        drawStyled(g,
                   simple() ? L"Pick a button to change it."
                            : L"Pick a key to change it.",
                   RectF(ix, (REAL)insNameY_, iw, (REAL)S(26)), tk::Text::Body,
                   k.inkMuted);
        return;
    }
    drawStyled(g, simple() ? L"Name on the button" : L"Label on the key",
               RectF(ix, (REAL)insNameY_, iw, (REAL)S(20)), tk::Text::Label,
               k.ink);
    const tk::KeyColor& kc = tk::nearestKeyColor(b->color);
    drawStyled(g, L"Colour: " + W(kc.name),
               RectF(ix, (REAL)insColourY_, iw, (REAL)S(20)), tk::Text::Label,
               k.ink);
    const wchar_t* caption = L"What it does";
    if (!simple() && gesture_ == 1) caption = L"When turned up";
    if (!simple() && gesture_ == 2) caption = L"When turned down";
    drawStyled(g, caption, RectF(ix, (REAL)captionY_, iw, (REAL)S(18)),
               tk::Text::Overline, k.inkMuted);
}

void MainWindow::paintToolbar(Graphics& g) {
    const Tok& k = tok();
    SolidBrush raised(k.surfaceRaised);
    g.FillRectangle(&raised, toRectF(rcToolbar_));
    Pen line(k.line, 1.0f);
    g.DrawLine(&line, 0.0f, (REAL)rcToolbar_.bottom - 0.5f, (REAL)rcToolbar_.right,
               (REAL)rcToolbar_.bottom - 0.5f);

    // No logo yet: the name, set in the title style.
    drawStyled(g, L"Dental Macros",
               RectF((REAL)S(kPad), (REAL)S(10), (REAL)S(320), (REAL)S(32)),
               tk::Text::Title, k.ink);

    // Stop the status line before the buttons rather than at a fixed width -
    // the listen button changes size with its label.
    int statusRight = rcToolbar_.right - S(kPad);
    if (btnEngine_) {
        RECT eb{};
        GetWindowRect(btnEngine_, &eb);
        MapWindowPoints(nullptr, hwnd_, (POINT*)&eb, 2);
        statusRight = eb.left - S(16);
    }

    bool keypads = false;
    for (const auto& kb : config_.keyboards)
        if (!kb.softwareOnly) keypads = true;

    // A word and a shape for every state: a filled dot while listening, an
    // empty ring while not.
    std::wstring text;
    bool on = false;
    if (config_.keyboards.empty()) {
        text = L"No buttons set up yet";
    } else if (!keypads) {
        text = L"Your on-screen buttons are ready";
        on = true;
    } else if (engine_.running()) {
        text = W(listeningSummary());
        if (!lastKeyText_.empty()) text += L"   \x00B7   " + W(lastKeyText_);
        on = true;
    } else {
        text = L"Not listening: your keypad types normally";
    }
    const REAL d = (REAL)S(12);
    const REAL sy = (REAL)S(44);
    const REAL dotY = sy + ((REAL)S(26) - d) / 2;
    if (on) {
        SolidBrush dot(k.success);
        g.FillEllipse(&dot, (REAL)S(kPad), dotY, d, d);
    } else {
        Pen ring(k.lineStrong, Sf(2));
        g.DrawEllipse(&ring, (REAL)S(kPad) + Sf(1), dotY + Sf(1), d - Sf(2),
                      d - Sf(2));
    }
    const REAL tx = (REAL)(S(kPad) + S(20));
    drawStyled(g, text,
               RectF(tx, sy, (REAL)std::max(S(120), statusRight - (int)tx),
                     (REAL)S(26)),
               tk::Text::Body, on ? k.ink : k.inkMuted);
}

void MainWindow::paintSidebar(Graphics& g) {
    const Tok& k = tok();
    SolidBrush raised(k.surfaceRaised);
    g.FillRectangle(&raised, toRectF(rcSidebar_));
    Pen line(k.line, 1.0f);
    g.DrawLine(&line, (REAL)rcSidebar_.right - 0.5f, (REAL)rcSidebar_.top,
               (REAL)rcSidebar_.right - 0.5f, (REAL)rcSidebar_.bottom);

    kbRows_.clear();
    palette_.clear();

    const bool sm = simple();
    const int x = rcSidebar_.left + S(kPad);
    const int w = S(kSidebarW) - S(kPad) * 2;
    int y = rcSidebar_.top + S(24);

    drawStyled(g, sm ? L"Your buttons" : L"Your keypads",
               RectF((REAL)x, (REAL)y, (REAL)w, (REAL)S(18)), tk::Text::Overline,
               k.inkMuted);
    y += S(30);

    // Each keypad is a small choice tile: its name, and a line saying how it
    // is - the words, not a coloured dot, carry it.
    const int rowH = S(sm ? 64 : 56);
    for (size_t i = 0; i < config_.keyboards.size(); ++i) {
        const cfg::Keyboard& kb = config_.keyboards[i];
        KbRow row;
        row.index = (int)i;
        row.rect = {x, y, x + w, y + rowH};
        kbRows_.push_back(row);

        const bool sel = kb.alias == currentAlias_;
        const bool hot = hoverKb_ == (int)i;
        bool missing = false;
        if (!kb.softwareOnly) {
            auto it = present_.find(kb.alias);
            missing = it != present_.end() && !it->second;
        }
        const RectF r = toRectF(row.rect);
        const float radius = Sf((float)tk::kRadiusMd);
        fillRounded(g, r, radius, sel ? k.primaryTint : k.surfaceRaised);
        if (missing && !sel && !hot)
            strokeRoundedDashed(g, r, radius, k.lineStrong, Sf(2));
        else
            strokeRounded(g, r, radius, sel || hot ? k.primary : k.line, Sf(2));

        // The round check says which one is shown.
        const REAL cd = (REAL)S(22);
        RectF check(r.X + Sf(14), r.Y + (r.Height - cd) / 2, cd, cd);
        if (sel) {
            SolidBrush f(k.primary);
            g.FillEllipse(&f, check);
            drawUiIcon(g, "check",
                       RectF(check.X + Sf(3), check.Y + Sf(3), cd - Sf(6), cd - Sf(6)),
                       k.onPrimary);
        } else {
            Pen p(k.lineStrong, Sf(2));
            g.DrawEllipse(&p, RectF(check.X + Sf(1), check.Y + Sf(1), cd - Sf(2),
                                    cd - Sf(2)));
        }

        std::string sub = kb.softwareOnly ? "On screen"
                          : missing       ? "Not connected"
                                          : "Connected";
        if (!sm && !kb.softwareOnly && !missing) {
            if (kb.isMouse && !kb.block) sub += ", a mouse";
            else if (!kb.block) sub += ", also types";
            else if (!kb.blockAllKeys) sub += ", button keys only";
        }
        const REAL tx = check.GetRight() + Sf(12);
        const REAL tw = r.GetRight() - Sf(12) - tx;
        const REAL nameH = (REAL)S(sm ? 24 : 20);
        const REAL top = r.Y + (r.Height - nameH - Sf(20)) / 2;
        drawStyled(g, W(kb.alias), RectF(tx, top, tw, nameH),
                   sm ? tk::Text::Button : tk::Text::CompactBold,
                   missing ? k.inkMuted : k.ink);
        if (missing)
            drawText(g, W(sub), RectF(tx, top + nameH, tw, Sf(20)), 15,
                     k.warningInk, Align::Left, true);
        else
            drawStyled(g, W(sub), RectF(tx, top + nameH, tw, Sf(20)),
                       tk::Text::Label, k.inkMuted);
        y += rowH + S(8);
    }
    y += S(8);

    // Ways to add more: Secondary buttons, painted here and hit-tested by
    // the rows they leave in kbRows_ (-1 keypad, -2 on-screen, -3 mouse).
    const int addH = S(sm ? tk::kTargetMin : tk::kTargetCompact);
    auto addButton = [&](int index, int hoverId, const wchar_t* label) {
        KbRow row;
        row.index = index;
        row.rect = {x, y, x + w, y + addH};
        kbRows_.push_back(row);
        ButtonState st;
        st.hover = hoverKb_ == hoverId;
        paintButton(g, toRectF(row.rect), label, ButtonKind::Secondary, st,
                    k.surfaceRaised,
                    tk::spec(sm ? tk::Text::Button : tk::Text::CompactBold),
                    "plus");
        y += addH + S(8);
    };
    addButton(-1, -2, L"Add a keypad");
    // A spare mouse or a trackpad. Its own button rather than a checkbox in
    // the keyboard scan, because you are looking for a different piece of
    // hardware and the list you need is a different list.
    if (!sm) addButton(-3, -4, L"Add a mouse or trackpad");
    // Buttons with no keyboard behind them. Worth its own button rather than
    // an option inside the keyboard scan, because the whole point is that
    // there is no device to go looking for.
    addButton(-2, -3, L"Add on-screen buttons");

    // ---- action palette - Advanced only: Simple mode adds steps with the
    // inspector's four buttons instead of dragging.
    if (sm) {
        paletteContentH_ = 0;
        return;
    }
    y += S(16);
    drawStyled(g, L"Drag onto a key",
               RectF((REAL)x, (REAL)y, (REAL)w, (REAL)S(18)), tk::Text::Overline,
               k.inkMuted);
    y += S(26);

    const int paletteTop = y;
    const int paletteBottom = rcSidebar_.bottom - S(8);
    int py = y - paletteScroll_;

    Region oldClip;
    g.GetClip(&oldClip);
    g.SetClip(Rect(x, paletteTop, w, std::max(0, paletteBottom - paletteTop)));

    const int prow = S(tk::kTargetCompact);
    std::string lastCat;
    for (const auto& a : cfg::actionDefs()) {
        if (a.category != lastCat) {
            lastCat = a.category;
            if (py + S(18) > paletteTop - S(20) && py < paletteBottom)
                drawStyled(g, W(lastCat),
                           RectF((REAL)x, (REAL)py + S(4), (REAL)w, (REAL)S(18)),
                           tk::Text::Overline, k.inkMuted);
            py += S(26);
        }
        PaletteRow row;
        row.actionId = a.id;
        row.rect = {x, py, x + w, py + prow};
        palette_.push_back(row);

        if (py + prow > paletteTop && py < paletteBottom) {
            const bool hot = hoverPalette_ == (int)palette_.size() - 1;
            if (hot)
                fillRounded(g, toRectF(row.rect), Sf((float)tk::kRadiusSm),
                            k.surfaceSunken);
            const REAL isz = (REAL)S(22);
            RectF ic((REAL)(x + S(8)), (REAL)py + (prow - isz) / 2, isz, isz);
            drawActionIcon(g, a.id, ic, hot ? k.primaryInk : k.ink);
            RectF tr((REAL)(x + S(40)), (REAL)py, (REAL)(w - S(44)), (REAL)prow);
            drawStyled(g, W(a.label), tr, tk::Text::Compact, k.ink);
        }
        py += prow + S(2);
    }
    paletteContentH_ = py + paletteScroll_ - paletteTop;
    g.SetClip(&oldClip);
}

void MainWindow::paintWelcome(Graphics& g) {
    const Tok& k = tok();
    RECT a = rcDeck_;
    a.top = deckTop_;
    const int cx = (a.left + a.right) / 2;
    const int cy = (a.top + a.bottom) / 2;

    // A sheet on the page: one heading, one line, one big main action.
    const int sw = std::min(S(520), (int)(a.right - a.left) - S(kPad) * 2);
    RectF box((REAL)(cx - sw / 2), (REAL)(cy - S(150)), (REAL)sw, (REAL)S(300));
    paintShadow(g, box, Sf((float)tk::kRadiusLg), Shadow::Soft);
    fillRounded(g, box, Sf((float)tk::kRadiusLg), k.surfaceRaised);
    strokeRounded(g, box, Sf((float)tk::kRadiusLg), k.line, 1.0f);

    const REAL pad = (REAL)S(tk::kSpace6);
    drawStyled(g, L"Set up your first button",
               RectF(box.X + pad, box.Y + pad, box.Width - pad * 2, (REAL)S(32)),
               tk::Text::Title, k.ink);
    drawStyled(g,
               L"Make buttons that type and click for you: on your screen, or "
               L"on a spare keypad while you keep typing normally on your main "
               L"keyboard.",
               RectF(box.X + pad, box.Y + pad + S(44), box.Width - pad * 2,
                     (REAL)S(84)),
               tk::Text::Body, k.inkMuted, Align::Left, false, true);

    rcWelcomeBtn_ = {(int)(box.X + pad), (int)(box.GetBottom() - pad) - S(64),
                     (int)(box.GetRight() - pad), (int)(box.GetBottom() - pad)};
    ButtonState st;
    st.hover = ptIn(rcWelcomeBtn_, dragPos_);
    paintButton(g, toRectF(rcWelcomeBtn_), L"Get started", ButtonKind::Primary,
                st, k.surfaceRaised, tk::spec(tk::Text::ButtonBig));
}

void MainWindow::paintStatusBanner(Graphics& g) {
    const BannerKind kind = bannerKind();
    const RectF r = toRectF(rcBanner_);
    const RectF d = toRectF(rcBannerDismiss_);
    std::string text = statusText_;
    // The banner has its own icon; the warning sign the text started with
    // for the old status line is not needed twice.
    const std::string warnSign = "\xE2\x9A\xA0 ";
    if (text.rfind(warnSign, 0) == 0) text = text.substr(warnSign.size());
    paintBanner(g, r, kind, W(text), r.GetRight() - d.X);
    ButtonState st;
    st.hover = hoverDismiss_;
    paintButton(g, d, L"Dismiss", ButtonKind::Quiet, st, bannerFill(kind));
}

void MainWindow::paintToast(Graphics& g) {
    if (toastText_.empty()) return;
    const std::wstring text = W(toastText_);
    const REAL h = (REAL)S(48);
    RECT area = rcDeck_;
    const REAL w = std::min(toastWidth(g, text), (REAL)(area.right - area.left - S(48)));
    RectF r((REAL)(area.left + area.right) / 2 - w / 2,
            (REAL)area.bottom - S(kPad) - h, w, h);
    ui::paintToast(g, r, text, toastGood_);
}

void MainWindow::paintDeck(Graphics& g) {
    cfg::Keyboard* kb = currentKb();
    if (!kb) return;
    const Tok& k = tok();

    if (!simple()) {
        const int hx = rcDeck_.left + S(kPad) + S(260) + S(16);
        RectF hint((REAL)hx, (REAL)(deckTop_ + S(16)),
                   (REAL)std::max(0, (int)rcDeck_.right - S(kPad) - hx),
                   (REAL)S(40));
        drawStyled(g, L"Click a key to change it, or drag an action onto it.",
                   hint, tk::Text::Compact, k.inkMuted, Align::Left, true, true);
    } else {
        RectF hint((REAL)(rcDeck_.left + S(kPad)), (REAL)(deckTop_ + S(16)),
                   (REAL)std::max(0, (int)(rcDeck_.right - rcDeck_.left) -
                                         S(kPad * 2)),
                   (REAL)S(26));
        drawStyled(g, L"Click a button to change it. Right-click it to rename "
                      L"or delete it.",
                   hint, tk::Text::Body, k.inkMuted);
    }

    // Tiles can be scrolled, so keep them inside the deck area rather than
    // letting them paint over the header, the sidebar or the log. The clip
    // leaves room for the selection ring outside a key.
    RECT clip = rcDeck_;
    clip.top = deckTop_ + S(deckHeaderH()) - S(8);
    Region oldClip;
    g.GetClip(&oldClip);
    g.SetClip(Rect(clip.left, clip.top, clip.right - clip.left,
                   std::max(0, (int)(clip.bottom - clip.top))));
    for (const DeckTile& t : tiles_) paintTile(g, t, kb);
    g.SetClip(&oldClip);

    paintPages(g);

    // A quiet hint that there is more below, only while it is true.
    if (deckContentH_ > deckViewH_) {
        const int hidden = deckContentH_ - deckViewH_ - deckScroll_;
        if (hidden > 0) {
            // On a chip of its own, so it reads over the keys it sits on.
            const std::wstring t = L"Scroll for more keys \x2193";
            const REAL w = measureText(g, t, 15, true).Width + Sf(32);
            const REAL h = (REAL)S(32);
            RectF chip((REAL)(rcDeck_.left + rcDeck_.right) / 2 - w / 2,
                       (REAL)rcDeck_.bottom - S(12) - h, w, h);
            paintShadow(g, chip, h / 2, Shadow::Soft);
            fillRounded(g, chip, h / 2, k.surfaceRaised);
            strokeRounded(g, chip, h / 2, k.line, Sf(1));
            drawStyled(g, t, chip, tk::Text::Label, k.inkMuted, Align::Center);
        }
    }
}

void MainWindow::paintPages(Graphics& g) {
    cfg::Keyboard* kb = currentKb();
    if (!kb || pageTabs_.empty()) return;
    const Tok& k = tok();

    for (size_t i = 0; i < pageTabs_.size(); ++i) {
        const PageTab& t = pageTabs_[i];
        const RectF r = toRectF(t.rect);
        const bool hover = hoverPageTab_ == (int)i;
        if (t.index < 0) {
            paintPill(g, r, L"Add page", false, hover, false, k.surface, "plus");
            continue;
        }
        const cfg::Page& page = kb->pages[(size_t)t.index];
        const bool current = t.index == kb->activePage;
        // The current page is the active pill. Its own colour, if it has
        // one, is a dot beside the name - never the only sign of which page
        // it is.
        paintPill(g, r, L"", current, hover, false, k.surface);
        const Color ink = current || hover ? k.primaryInk : k.ink;
        REAL x = r.X + Sf(18);
        if (!trim(page.color).empty()) {
            const REAL d = Sf(10);
            SolidBrush dot(readableTileColor(page.color, k.primary));
            g.FillEllipse(&dot, x, r.Y + (r.Height - d) / 2, d, d);
            x += Sf(18);
        }
        const std::wstring name = W(page.displayName());
        const REAL tw = measureText(g, name, 16, true).Width + Sf(4);
        drawText(g, name, RectF(x, r.Y, tw + Sf(4), r.Height), 16, ink,
                 Align::Left, true);
        // A page that a program brings up by itself carries that program's
        // window mark.
        if (!page.rule.empty()) {
            const REAL isz = Sf(16);
            drawActionIcon(g, "window_focus",
                           RectF(x + tw + Sf(6), r.Y + (r.Height - isz) / 2, isz, isz),
                           ink);
        }
    }

    // The follow switch: on, the deck follows the program in front.
    if (IsRectEmpty(&rcPagePin_)) return;
    const bool follow = config_.settings.switchPageWithApp;
    paintPill(g, toRectF(rcPagePin_), L"Follow the program in front", follow,
              hoverPageTab_ == -2, false, k.surface, follow ? "check" : "");
}

void MainWindow::paintTile(Graphics& g, const DeckTile& tile,
                           cfg::Keyboard* kb) {
    const Tok& k = tok();
    RectF r = toRectF(tile.rect);
    const float radius = Sf((float)tk::kRadiusMd);
    const int index = (int)(&tile - tiles_.data());
    const bool hover = hoverTile_ == index;
    const bool selected = selectedTile_ == index;

    const cfg::Binding* b =
        tile.binding >= 0 && tile.binding < (int)kb->active().bindings.size()
            ? &kb->active().bindings[(size_t)tile.binding]
            : nullptr;

    // A key that just ran does the press: a quick shrink to 96%, the only
    // motion the design system allows a macro key.
    if (b) {
        auto it = lastFire_.find(currentAlias_ + "|" + b->key);
        if (it != lastFire_.end() && GetTickCount() - it->second < 160) {
            const float dw = r.Width * 0.04f, dh = r.Height * 0.04f;
            r = RectF(r.X + dw / 2, r.Y + dh / 2, r.Width - dw, r.Height - dh);
        }
    }

    // The selected key: the focus ring, outside the key with a 2px gap.
    if (selected) {
        const float out = Sf((float)(tk::kFocusGap + tk::kFocusRing));
        RectF ring(r.X - out, r.Y - out, r.Width + out * 2, r.Height + out * 2);
        strokeRounded(g, ring, radius + out, k.focusRing,
                      Sf((float)tk::kFocusRing));
    }

    if (!b) {
        // An empty slot: no fill, a dashed edge, a plus and the key's name.
        if (hover) fillRounded(g, r, radius, k.surfaceRaised);
        strokeRoundedDashed(g, r, radius, hover ? k.primary : k.lineStrong, Sf(2));
        const bool named = !tile.key.empty() && !kb->softwareOnly;
        const std::wstring label =
            named ? W(keys::displayName(tile.key)) : std::wstring(L"Add a button");
        const REAL isz = Sf(22);
        const REAL cy = r.Y + r.Height / 2 - Sf(12);
        drawUiIcon(g, "plus", RectF(r.X + (r.Width - isz) / 2, cy - isz / 2, isz, isz),
                   hover ? k.primaryInk : k.inkMuted);
        drawStyled(g, label,
                   RectF(r.X + Sf(6), cy + isz / 2 + Sf(6), r.Width - Sf(12), Sf(20)),
                   tk::Text::Label, hover ? k.primaryInk : k.inkMuted,
                   Align::Center);
        return;
    }

    Color base = readableTileColor(b->color, theme().tile);
    // Hover lifts a key one shade, as bundle.css's .sme-msg:hover does.
    if (hover) base = mix(base, kWhite, 0.08f);
    paintShadow(g, r, radius, hover || selected ? Shadow::Hover : Shadow::Soft);
    fillRounded(g, r, radius, base);

    // The key badge, top left - meaningless on a button with no key behind
    // it. bundle.css .sme-key: white at 22% behind white text.
    const REAL tagH = Sf(22);
    REAL badgeRight = r.X;
    if (!kb->softwareOnly) {
        const std::wstring keyLabel = W(keys::displayName(b->key));
        const REAL bw = std::min(measureText(g, keyLabel, 13, true).Width + Sf(16),
                                 r.Width - Sf(16));
        RectF badge(r.X + Sf(8), r.Y + Sf(8), bw, tagH);
        fillRounded(g, badge, Sf((float)tk::kRadiusSm), withAlpha(kWhite, 56));
        drawText(g, keyLabel, badge, 13, kWhite, Align::Center, true);
        badgeRight = badge.GetRight();
    }
    // A key you can also turn says so, in words, top right.
    if (b->isDial()) {
        const REAL tw = measureText(g, L"Turn", 12, true).Width + Sf(16);
        if (r.GetRight() - Sf(8) - tw > badgeRight + Sf(4)) {
            RectF tag(r.GetRight() - Sf(8) - tw, r.Y + Sf(8), tw, tagH);
            fillRounded(g, tag, tagH / 2, withAlpha(kWhite, 56));
            drawText(g, L"Turn", tag, 12, kWhite, Align::Center, true);
        }
    }

    // Top to bottom: the badge row, the icon (about a third of the key, as
    // before the redesign), then two lines of label. On a small key the icon
    // gives way first, so the name - what the key is for - always fits.
    const float labelPx = r.Width >= Sf(150) ? 17.0f : r.Width >= Sf(110) ? 15.0f
                                                                       : 14.0f;
    const REAL lineH = Sf(labelPx * 1.4f);  // Segoe UI needs about 1.33
    const REAL top = r.Y + (kb->softwareOnly ? Sf(12) : Sf(8) + tagH + Sf(4));
    // Room for two lines of label only when the name needs them. drawText
    // pads each side by about a sixth of the text size; measureText doesn't.
    const std::wstring title = W(b->displayTitle());
    const int lines = measureText(g, title, labelPx, true).Width + Sf(labelPx) * 0.4f <=
                              r.Width - Sf(12)
                          ? 1
                          : 2;
    const REAL room = r.GetBottom() - Sf(6) - lineH * lines - Sf(4) - top;
    const REAL isz =
        std::max(0.0f, std::min(std::min(r.Width, r.Height) * 0.34f, room));
    RectF iconBox(r.X + (r.Width - isz) / 2, top, isz, isz);
    if (isz < Sf(18)) iconBox = RectF(r.X + r.Width / 2, top, 0, 0);
    const bool unfinished = b->steps.empty() && !b->isDial();
    if (unfinished) {
        // In the URGENT tag's style, so a half-made key can't be missed.
        const std::wstring t = L"No steps yet";
        const REAL tw = std::min(measureText(g, t, 12, true).Width + Sf(16),
                                 r.Width - Sf(12));
        RectF tag(r.X + (r.Width - tw) / 2, top + std::max(0.0f, (isz - tagH) / 2),
                  tw, tagH);
        fillRounded(g, tag, tagH / 2, kWhite);
        drawText(g, t, tag, 12, k.dangerInk, Align::Center, true);
        if (iconBox.Height < tagH) iconBox.Height = tagH;
    } else if (iconBox.Width <= 0) {
        // No room for an icon on a key this small.
    } else if (isLibraryIcon(b->icon) &&
               drawLibraryIcon(g, libraryIconId(b->icon), iconBox, kWhite)) {
        // A "lib:" icon is drawn from code and never touches the image cache.
    } else if (Image* img = b->icon.empty() || isLibraryIcon(b->icon)
                                ? nullptr
                                : icons_.get(b->icon)) {
        // preserve aspect ratio inside the box
        const float iw = (float)img->GetWidth(), ih = (float)img->GetHeight();
        if (iw > 0 && ih > 0) {
            const float scale = std::min(iconBox.Width / iw, iconBox.Height / ih);
            RectF dst(iconBox.X + (iconBox.Width - iw * scale) / 2,
                      iconBox.Y + (iconBox.Height - ih * scale) / 2, iw * scale,
                      ih * scale);
            g.DrawImage(img, dst);
        }
    } else if (!b->steps.empty()) {
        drawActionIcon(g, b->steps[0].action, iconBox, kWhite);
    } else {
        drawActionIcon(g, "scroll", iconBox, kWhite);
    }

    // Key labels are bold; a narrow key gets a smaller size so two words
    // still fit across it.
    // The "No steps yet" tag can be taller than the room left for an icon;
    // the name still gets all of its lines.
    const REAL labelTop =
        std::max(top, std::min(iconBox.Height > 0 ? iconBox.GetBottom() + Sf(4) : top,
                               r.GetBottom() - Sf(4) - lineH * lines));
    RectF label(r.X + Sf(6), labelTop, r.Width - Sf(12),
                r.GetBottom() - Sf(4) - labelTop);
    drawText(g, title, label, labelPx, kWhite, Align::Center, true, true, true);
}

void MainWindow::paintDragGhost(Graphics& g) {
    const cfg::ActionDef* def = cfg::findAction(dragAction_);
    if (!def) return;
    const std::wstring label = W(def->label);
    const REAL h = (REAL)S(40);
    const REAL w = measureText(g, label, 15, true).Width + Sf(22 + 8 + 32);
    RectF chip((REAL)(dragPos_.x + S(12)), (REAL)(dragPos_.y + S(10)), w, h);
    paintShadow(g, chip, h / 2, Shadow::Sheet);
    fillRounded(g, chip, h / 2, tok().primary);
    const REAL isz = Sf(22);
    drawActionIcon(g, dragAction_,
                   RectF(chip.X + Sf(14), chip.Y + (h - isz) / 2, isz, isz),
                   tok().onPrimary);
    drawText(g, label, RectF(chip.X + Sf(14) + isz + Sf(8), chip.Y,
                             w - isz - Sf(36), h),
             15, tok().onPrimary, Align::Left, true);
}

// ------------------------------------------------------------ model access
cfg::Keyboard* MainWindow::currentKb() {
    return currentAlias_.empty() ? nullptr : config_.keyboard(currentAlias_);
}

const cfg::DeckPreset* MainWindow::currentPreset() {
    cfg::Keyboard* kb = currentKb();
    return cfg::findPreset(kb ? kb->deck : "auto");
}

cfg::Binding* MainWindow::selectedBinding() {
    cfg::Keyboard* kb = currentKb();
    if (!kb) return nullptr;
    if (selectedTile_ < 0 || selectedTile_ >= (int)tiles_.size()) return nullptr;
    int bi = tiles_[(size_t)selectedTile_].binding;
    if (bi < 0 || bi >= (int)kb->active().bindings.size()) return nullptr;
    return &kb->active().bindings[(size_t)bi];
}

void MainWindow::selectKeyboard(const std::string& alias) {
    currentAlias_ = alias;
    selectedTile_ = -1;
    deckScroll_ = 0;
    refreshDeckCombo();
    layoutDeck();
    // select the first configured key so the inspector isn't empty
    for (size_t i = 0; i < tiles_.size(); ++i)
        if (tiles_[i].binding >= 0) { selectedTile_ = (int)i; break; }
    refreshInspector();
    refreshMiniDeck();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::refreshDeckCombo() {
    cfg::Keyboard* kb = currentKb();
    std::string want = kb ? kb->deck : "auto";
    for (size_t i = 0; i < deckIds_.size(); ++i)
        if (deckIds_[i] == want) {
            SendMessageW(deckCombo_, CB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    SendMessageW(deckCombo_, CB_SETCURSEL, 0, 0);
}

void MainWindow::refreshInspector() {
    cfg::Binding* b = selectedBinding();
    bool has = b != nullptr;

    setText(titleEdit_, has ? b->title : "");
    EnableWindow(titleEdit_, has);
    // An empty, greyed name box reads as something to fill in. With nothing
    // picked there is nothing to name, so the panel shows only its heading.
    showInspectorControls();
    EnableWindow(btnIcon_, has);
    EnableWindow(btnColor_, has);
    // The tick goes on the colour the key is drawn in: the nearest of the
    // seven to whatever it has saved.
    const tk::KeyColor* chosen = has ? &tk::nearestKeyColor(b->color) : nullptr;
    for (size_t i = 0; i < swatches_.size(); ++i)
        setSwatchSelected(swatches_[i], chosen == &tk::keyColors()[i]);
    EnableWindow(btnAddStep_, has);
    EnableWindow(btnRecord_, has);
    for (HWND h : {btnSType_, btnSKeys_, btnSWait_}) EnableWindow(h, has);
    const std::vector<cfg::Step>* shown = editingSteps();
    EnableWindow(btnTest_, shown && !shown->empty());
    refreshGestureButtons();

    // LB_RESETCONTENT throws the selection away, so remember it and put it
    // back. Without this, clicking a step selected it and deselected it in the
    // same breath - Edit and Remove went grey again immediately - and the only
    // way to fix one bad step was to delete them all and record the sequence
    // over again.
    const std::string nowFor = has ? currentAlias_ + "|" + b->key : std::string();
    int keep = (int)SendMessageW(stepList_, LB_GETCURSEL, 0, 0);
    // A different key is a fresh list. Carrying an index across would land on
    // whatever step happens to sit at that position in the new one.
    if (nowFor != stepListFor_) keep = -1;
    stepListFor_ = nowFor;

    SendMessageW(stepList_, LB_RESETCONTENT, 0, 0);
    simpleRows_.clear();
    if (has && simple() && shown) {
        simpleRows_ = smp::rowsFor(*shown);
        for (size_t r = 0; r < simpleRows_.size(); ++r)
            SendMessageW(stepList_, LB_ADDSTRING, 0,
                         (LPARAM)W(std::to_string(r + 1) + ".  " +
                                   simpleRows_[r].text)
                             .c_str());
    } else if (has)
        for (size_t i = 0; shown && i < shown->size(); ++i)
            SendMessageW(stepList_, LB_ADDSTRING, 0,
                         (LPARAM)W(std::to_string(i + 1) + ".  " +
                                   (*shown)[i].summary())
                             .c_str());
    EnableWindow(stepList_, has);

    if (has && keep >= 0) {
        // The step that was selected may have just been deleted off the end.
        const int rows = (int)SendMessageW(stepList_, LB_GETCOUNT, 0, 0);
        if (keep >= rows) keep = rows - 1;
        if (keep >= 0) SendMessageW(stepList_, LB_SETCURSEL, (WPARAM)keep, 0);
    }

    updateStepButtons();
    InvalidateRect(hwnd_, &rcInspector_, FALSE);
}

void MainWindow::updateStepButtons() {
    if (simple()) {
        // A row of advanced steps can't be removed from here - only shown.
        const int row = selectedRow();
        EnableWindow(btnDelStep_,
                     row >= 0 &&
                         simpleRows_[(size_t)row].kind != smp::Kind::Advanced);
        return;
    }
    const std::vector<cfg::Step>* list = editingSteps();
    int sel = (int)SendMessageW(stepList_, LB_GETCURSEL, 0, 0);
    const bool hasStep = list && sel >= 0 && sel < (int)list->size();
    EnableWindow(btnEditStep_, hasStep);
    EnableWindow(btnDelStep_, hasStep);
    EnableWindow(btnUp_, hasStep && sel > 0);
    EnableWindow(btnDown_, hasStep && sel < (int)list->size() - 1);
}

void MainWindow::modelChanged(bool relayoutDeck) {
    config_.dirty = true;
    saveConfig();
    engine_.updateConfig(config_);
    if (relayoutDeck) layoutDeck();
    refreshInspector();
    refreshMiniDeck();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::saveConfig() {
    // One rolling copy per day, taken before the first write of each run, so
    // there is always a yesterday to go back to.
    if (!dailyBackupDone_) {
        dailyBackupDone_ = true;
        std::string where;
        if (config_.writeDailyBackup(10, &where))
            pushLog("Backed up the previous settings to " +
                    relativeToApp(where));
    }

    std::string err;
    bool saved = config_.save(&err);
    // The program's folder refuses writes - copied into Program Files, say,
    // or locked down by whoever manages the PC. Carry on in the user's
    // profile folder instead of losing every change from here on.
    if (!saved && !dataDirIsFallback() &&
        fe::saveProblem(config_.lastSaveError) == fe::Problem::SaveDenied) {
        pushLog("Dental Macros' own folder refuses changes (" + err +
                "). Trying the Windows profile folder instead.");
        std::string moveErr;
        if (moveDataToProfile(&moveErr)) {
            config_.path = dataPath("macros.json");
            saved = config_.save(&err);
            if (saved) {
                pushLog("Your settings are now kept in " + dataDir() + ".");
                setStatus("Your settings are now kept in your Windows profile, "
                          "because Dental Macros' own folder can't be changed.");
            }
        } else {
            pushLog("  The Windows profile folder can't be used either: " +
                    moveErr);
        }
    }
    if (!saved) {
        pushLog("Could not save settings: " + err);
        const fe::Problem p = fe::saveProblem(config_.lastSaveError);
        setStatus("Your changes haven't been saved. They are kept until "
                  "saving works again.",
                  StatusTone::Danger);
        // Never a dialog while closing - there is nothing left to show it on.
        if (p != saveProblemShown_ && !destroying_) {
            saveProblemShown_ = p;
            showFriendly(hwnd_, fe::explain(p, "", err));
        }
        return;
    }
    if (saveProblemShown_ != fe::Problem::None) {
        saveProblemShown_ = fe::Problem::None;
        pushLog("Saving works again.");
        setStatus("\xE2\x9C\x93 Your changes are being saved again.",
                  StatusTone::Good);
    }
    if (!config_.lastBackup.empty()) {
        pushLog("The unreadable macros.json was kept as " +
                relativeToApp(config_.lastBackup) + " before saving.");
        config_.lastBackup.clear();
    }
}

// ---------------------------------------------------------------- commands
void MainWindow::addKeyboard() {
    std::string alias;
    if (!addKeyboardDialog(hwnd_, config_, alias, false)) return;
    saveConfig();
    engine_.updateConfig(config_);
    selectKeyboard(alias);
    pushLog("Added keyboard \"" + alias + "\".");
    layout();
}

void MainWindow::addMouse() {
    std::string alias;
    if (!addKeyboardDialog(hwnd_, config_, alias, true)) return;
    countPointers();
    saveConfig();
    engine_.updateConfig(config_);
    selectKeyboard(alias);
    pushLog("Added mouse \"" + alias + "\".");
    if (config_.pointerCount <= 1)
        pushLog(
            "  It is the only pointer attached, so left click stays yours. "
            "Dental Macros won't take it, however the mouse is set up.");
    layout();
}

void MainWindow::countPointers() {
    // Asked here and kept on the config, because the answer is needed inside
    // the blocking hook - which cannot afford to enumerate devices, and must
    // not, on every click.
    const int n = (int)dev::listMice().size();
    config_.pointerCount = n > 0 ? n : 1;
}

bool MainWindow::runDial(const std::string& key, bool up) {
    cfg::Keyboard* kb = currentKb();
    if (!kb) return false;
    cfg::Binding* b = kb->findKey(key);
    if (!b) return false;
    const std::vector<cfg::Step>& steps = up ? b->scrollUp : b->scrollDown;
    if (steps.empty()) return false;
    lastFire_[currentAlias_ + "|" + b->key] = GetTickCount();
    SetTimer(hwnd_, 2, 40, nullptr);
    engine_.runSequenceAsync(steps, "Dial", currentAlias_, b->displayTitle());
    return true;
}

bool MainWindow::tileUnderCursorIsDial(POINT p, bool up) {
    for (const DeckTile& t : tiles_) {
        if (!ptIn(t.rect, p) || t.binding < 0) continue;
        return runDial(t.key, up);
    }
    return false;
}

std::vector<cfg::Step>* MainWindow::editingSteps() {
    cfg::Binding* b = selectedBinding();
    if (!b) return nullptr;
    if (gesture_ == 1) return &b->scrollUp;
    if (gesture_ == 2) return &b->scrollDown;
    return &b->steps;
}

void MainWindow::setGesture(int gesture) {
    if (gesture_ == gesture) return;
    gesture_ = gesture;
    // The list underneath is about to be a different list, so a selection
    // index carried over from the last one means nothing.
    stepListFor_.clear();
    refreshInspector();
    refreshGestureButtons();
}

void MainWindow::refreshGestureButtons() {
    cfg::Binding* b = selectedBinding();
    const bool has = b != nullptr;
    EnableWindow(btnGesturePress_, has);
    EnableWindow(btnGestureUp_, has);
    EnableWindow(btnGestureDown_, has);
    setPillActive(btnGesturePress_, gesture_ == 0);
    setPillActive(btnGestureUp_, gesture_ == 1);
    setPillActive(btnGestureDown_, gesture_ == 2);
    // A dot on a direction that already carries something, so a tile's other
    // gestures are visible without clicking through all three.
    setText(btnGestureUp_,
            std::string("Turn up") + (b && !b->scrollUp.empty() ? " \u2022" : ""));
    setText(btnGestureDown_,
            std::string("Turn down") +
                (b && !b->scrollDown.empty() ? " \u2022" : ""));
}

void MainWindow::selectTile(int tileIndex) {
    // Always land on the press list: the tile you just clicked is a different
    // tile, and showing its wheel-up steps because the last one was a dial
    // would be a surprise.
    gesture_ = 0;
    selectedTile_ = tileIndex;
    refreshInspector();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::activateTile(int tileIndex) {
    if (tileIndex < 0 || tileIndex >= (int)tiles_.size()) return;
    DeckTile& t = tiles_[(size_t)tileIndex];
    if (t.binding >= 0) {
        selectTile(tileIndex);
        return;
    }
    assignKeyToTile(tileIndex);
}

void MainWindow::assignKeyToTile(int tileIndex) {
    cfg::Keyboard* kb = currentKb();
    if (!kb) return;
    DeckTile t = tiles_[(size_t)tileIndex];
    const cfg::DeckPreset* preset = currentPreset();

    std::string key = t.key;
    if (key.empty() && kb->softwareOnly) {
        // Nothing to press. The key field is just an id here, so mint one that
        // no real key name could collide with and get straight to naming it.
        for (int n = 1;; ++n) {
            std::string id = "btn" + std::to_string(n);
            if (!kb->findKey(id)) { key = id; break; }
        }
    } else if (key.empty()) {
        // Blank slot on a generic grid (or the flow "+" tile): ask the user to
        // press the key they want this tile to be.
        KeyCaptureResult r = captureKey(
            hwnd_, kb->isMouse ? "Pick a button" : "Pick a key",
            std::string(kb->isMouse ? "Press the button you want to use"
                                    : "Press the key you want to use") +
                "\nPress it on \"" + kb->alias +
                "\". This button will run whenever you do.",
            kb->match);
        if (!r.ok) return;
        key = r.keyName;
    }

    // Some keys are never allowed to carry a macro, however you got here -
    // pressed on a mouse, typed by name, or arriving in a preset slot.
    std::string why;
    if (!cfg::keyBindable(key, &why)) {
        showMessage(hwnd_, "That one has to keep working", why);
        return;
    }

    // One binding per key: a second one would be dead weight, since lookup
    // always resolves to the first.
    if (kb->findKey(key)) {
        for (size_t i = 0; i < tiles_.size(); ++i)
            if (tiles_[i].binding >= 0 &&
                kb->active().bindings[(size_t)tiles_[i].binding].key == key) {
                selectTile((int)i);
                break;
            }
        showMessage(hwnd_, "That key already has a button",
                    "\"" + keys::displayName(key) +
                        "\" already runs a button on this keypad. Pick another key, or change that button instead.");
        return;
    }

    cfg::Binding b;
    b.key = key;
    b.slot = (preset->flow || preset->fixedKeys) ? -1 : t.slot;
    kb->active().bindings.push_back(b);
    modelChanged();

    for (size_t i = 0; i < tiles_.size(); ++i)
        if (tiles_[i].binding == (int)kb->active().bindings.size() - 1) {
            selectTile((int)i);
            break;
        }
    SetFocus(titleEdit_);
}

void MainWindow::addStep(const std::string& presetAction) {
    cfg::Binding* b = selectedBinding();
    if (!b) return;
    cfg::Step step;
    step.action = presetAction.empty() ? "type" : presetAction;
    if (!editStepDialog(hwnd_, step, config_.settings)) return;
    std::vector<cfg::Step>* steps = editingSteps();
    if (!steps) return;
    steps->push_back(step);
    modelChanged();
    // Land on the step just added, and leave it selected: the refresh that
    // used to follow this reset the list and threw the selection away again.
    SendMessageW(stepList_, LB_SETCURSEL, (WPARAM)(steps->size() - 1), 0);
    updateStepButtons();
}

void MainWindow::editStep() {
    if (simple()) {
        editSimpleRow(selectedRow());
        return;
    }
    std::vector<cfg::Step>* steps = editingSteps();
    if (!steps) return;
    int sel = (int)SendMessageW(stepList_, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)steps->size()) return;
    cfg::Step step = (*steps)[(size_t)sel];
    if (!editStepDialog(hwnd_, step, config_.settings)) return;
    (*steps)[(size_t)sel] = step;
    modelChanged();
    SendMessageW(stepList_, LB_SETCURSEL, (WPARAM)sel, 0);
    updateStepButtons();
}

void MainWindow::removeStep() {
    if (simple()) {
        removeSimpleRow(selectedRow());
        return;
    }
    std::vector<cfg::Step>* steps = editingSteps();
    if (!steps) return;
    int sel = (int)SendMessageW(stepList_, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)steps->size()) return;
    steps->erase(steps->begin() + sel);
    modelChanged();
    // Stay where they were, or on the new last step if the end was removed,
    // so a run of deletions does not need a re-click between each one.
    SendMessageW(stepList_, LB_SETCURSEL,
                 (WPARAM)std::min(sel, (int)steps->size() - 1), 0);
    updateStepButtons();
}

void MainWindow::moveStep(int delta) {
    std::vector<cfg::Step>* steps = editingSteps();
    if (!steps) return;
    int sel = (int)SendMessageW(stepList_, LB_GETCURSEL, 0, 0);
    int dst = sel + delta;
    if (sel < 0 || dst < 0 || dst >= (int)steps->size()) return;
    std::swap((*steps)[(size_t)sel], (*steps)[(size_t)dst]);
    modelChanged();
    // Follow the step being moved, so Up can be pressed repeatedly.
    SendMessageW(stepList_, LB_SETCURSEL, (WPARAM)dst, 0);
    updateStepButtons();
}

void MainWindow::recordClicks(int replaceFirst, int replaceCount) {
    cfg::Keyboard* kb = currentKb();
    if (!kb || !selectedBinding()) return;

    // Where the clicks are going, pinned down before anything can move. While
    // recording, the user works in other programs, and a page tied to one of
    // them comes to the front by itself - which clears the selection and
    // would otherwise send the clicks nowhere, or to another key entirely.
    const std::string alias = kb->alias;
    const int page = kb->activePage;
    const int bindingIndex = tiles_[(size_t)selectedTile_].binding;
    const int gesture = gesture_;

    // Out of the way while recording: the recorder sits in a corner, and
    // everything the clicks need to land on should be in plain view.
    mini_.suspend(true);
    ShowWindow(hwnd_, SW_HIDE);
    std::vector<rec::Click> clicks;
    const bool got = recordClicksPanel(hwnd_, clicks, recordSpeed_);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    mini_.suspend(false);
    if (!got) return;

    kb = config_.keyboard(alias);
    if (!kb || page >= (int)kb->pages.size() || bindingIndex < 0 ||
        bindingIndex >= (int)kb->pages[(size_t)page].bindings.size())
        return;
    cfg::Binding& b = kb->pages[(size_t)page].bindings[(size_t)bindingIndex];
    std::vector<cfg::Step>& steps =
        gesture == 1 ? b.scrollUp : gesture == 2 ? b.scrollDown : b.steps;
    const std::vector<cfg::Step> recorded = rec::buildSteps(clicks, recordSpeed_);
    size_t at = steps.size();
    if (replaceFirst >= 0 && replaceCount > 0 &&
        (size_t)(replaceFirst + replaceCount) <= steps.size()) {
        at = (size_t)replaceFirst;
        steps.erase(steps.begin() + replaceFirst,
                    steps.begin() + replaceFirst + replaceCount);
    }
    steps.insert(steps.begin() + (std::ptrdiff_t)at, recorded.begin(),
                 recorded.end());

    // Back to the key the clicks went into, on the page it lives on.
    if (kb->activePage != page) setActivePage(alias, page);
    for (size_t i = 0; i < tiles_.size(); ++i)
        if (tiles_[i].binding == bindingIndex) {
            selectTile((int)i);
            break;
        }
    gesture_ = gesture;
    modelChanged();
    refreshInspector();
    selectStep(at + recorded.size() - 1);

    const rec::Target& target = clicks.front().window;
    const std::string n = std::to_string(clicks.size());
    const std::string plural = clicks.size() == 1 ? " click" : " clicks";
    if (target.has) {
        pushLog("Recorded " + n + plural + " in " + target.displayName() +
                " into \"" + b.displayTitle() + "\" (" +
                std::to_string(recorded.size()) + " steps).");
        pushLog("  Each time it runs, " + target.displayName() +
                "'s window is put back where it was (" +
                std::to_string(target.x) + "," + std::to_string(target.y) +
                ", " + std::to_string(target.w) + "x" +
                std::to_string(target.h) +
                ") so the clicks line up, then returned afterwards.");
        if (target.maximized)
            pushLog("  It was full-screen; it will be full-screen again "
                    "afterwards.");
    } else {
        pushLog("Recorded " + n + plural + " into \"" + b.displayTitle() +
                "\".");
        pushLog("  The first click isn't inside a program, so the clicks go "
                "to those spots on the screen, wherever things are.");
    }
}

// Replace the current settings with a saved copy - an export, or one of the
// daily backups. The current file is kept in the backups folder first.
void MainWindow::loadCopy() {
    const std::string backups = dataPath("backups");
    const std::wstring initialDir = widen(dirExists(backups) ? backups : dataDir());
    wchar_t file[MAX_PATH * 2] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Dental Macros settings\0*.json\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.lpstrTitle = L"Load a copy of your settings";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;

    const std::string chosen = narrow(file);
    const std::string current = dataPath("macros.json");
    if (iequals(chosen, current)) {
        showMessage(hwnd_, "That's the file in use",
                    "That is the settings file Dental Macros is using right "
                    "now. Choose a saved copy instead - your daily copies are "
                    "in the backups folder.");
        return;
    }

    cfg::Config probe;
    probe.load(chosen);
    if (!probe.loadError.empty()) {
        showMessage(hwnd_, "Couldn't load that file",
                    "That doesn't look like a Dental Macros settings file, or "
                    "it's damaged. Nothing was changed.\n\nDetails: " +
                        probe.loadError,
                    true);
        return;
    }
    int keys = 0;
    for (const auto& k : probe.keyboards) keys += totalBindings(k);
    const size_t decks = probe.keyboards.size();
    if (!confirm(hwnd_, "Load this copy?",
                 "It has " + std::to_string(decks) +
                     (decks == 1 ? " set" : " sets") + " of buttons and " +
                     std::to_string(keys) + (keys == 1 ? " button" : " buttons") +
                     ". They will replace the buttons you have now. Your "
                     "current buttons are saved in the backups folder first, "
                     "so you can go back.",
                 "Load copy", "Keep my buttons"))
        return;

    if (config_.dirty) saveConfig();
    std::string kept;
    if (fileExists(current)) {
        ensureDir(backups);
        // Named after today's date so it sorts, and is eventually tidied
        // away, alongside the daily copies.
        kept = joinPath(backups, "macros-" + dateStamp() + "-before-load-" +
                                     replaceAll(timeStamp(), ":", "") + ".json");
        if (!copyFile(current, kept, false)) {
            showMessage(hwnd_, "Nothing was changed",
                        "Dental Macros couldn't keep a copy of your current "
                        "settings, so it didn't replace them.",
                        true);
            return;
        }
    }
    if (!copyFile(chosen, current, true)) {
        showMessage(hwnd_, "Nothing was changed",
                    "Windows didn't let Dental Macros copy that file into "
                    "place. Your buttons are as they were.",
                    true);
        return;
    }
    config_.dirty = false;   // the file on disk is now the one to show
    reloadConfig();
    pushLog("Loaded settings from " + chosen + ".");
    if (!kept.empty())
        pushLog("  The ones you had before are in " + relativeToApp(kept) + ".");
}

void MainWindow::openSettings() {
    SettingsAction action = SettingsAction::None;
    if (!settingsDialog(hwnd_, config_.settings, &action)) return;
    saveConfig();
    engine_.updateConfig(config_);
    applyToggleHotkey();
    switch (action) {
        case SettingsAction::SaveCopy: exportConfig(); break;
        case SettingsAction::LoadCopy: loadCopy(); break;
        case SettingsAction::Reload: reloadConfig(); break;
        case SettingsAction::ImportMacroKb: importYaml(); break;
        case SettingsAction::RunSetup: runSetup(); break;
        case SettingsAction::None: break;
    }
}

void MainWindow::updateFloatingButton() {
    if (!btnFloating_) return;
    // A pill that is on while they are on screen.
    setPillActive(btnFloating_, mini_.visible());
}

void MainWindow::setKeyColor(int index) {
    cfg::Binding* b = selectedBinding();
    if (!b || index < 0 || index >= (int)tk::keyColors().size()) return;
    b->color = tk::keyColors()[(size_t)index].hex;
    modelChanged(false);
}

void MainWindow::showToast(const std::string& text, bool good) {
    toastText_ = text;
    toastGood_ = good;
    SetTimer(hwnd_, kToastTimer, kToastMs, nullptr);
    InvalidateRect(hwnd_, &rcDeck_, FALSE);
}

void MainWindow::testRun() {
    cfg::Binding* b = selectedBinding();
    std::vector<cfg::Step>* steps = editingSteps();
    if (!b || !steps || steps->empty()) return;
    pushLog("Test run: " + keys::displayName(b->key) +
            (gesture_ == 1   ? " wheel up"
             : gesture_ == 2 ? " wheel down"
                             : "") +
            " (" + std::to_string(steps->size()) + " steps)");
    engine_.runSequenceAsync(*steps, "Test", currentAlias_, b->displayTitle());
}

void MainWindow::clearTile() {
    cfg::Binding* b = selectedBinding();
    if (!b) return;
    if (!confirm(hwnd_,
                 "Remove every step from \"" + b->displayTitle() + "\"?",
                 "The button stays, with nothing for it to do.", "Remove steps",
                 "Keep them", true))
        return;
    b->steps.clear();
    // Clearing a tile means the whole tile, dial gestures included. Leaving
    // invisible scroll steps on a "cleared" button is the sort of thing that
    // gets found months later.
    b->scrollUp.clear();
    b->scrollDown.clear();
    modelChanged();
}

void MainWindow::deleteBinding() {
    cfg::Keyboard* kb = currentKb();
    if (!kb || selectedTile_ < 0) return;
    int bi = tiles_[(size_t)selectedTile_].binding;
    if (bi < 0 || bi >= (int)kb->active().bindings.size()) return;
    std::string name = kb->active().bindings[(size_t)bi].displayTitle();
    if (!confirm(hwnd_, "Delete the \"" + name + "\" button?", "",
                 "Delete button", "Keep it", true))
        return;
    kb->active().bindings.erase(kb->active().bindings.begin() + bi);
    selectedTile_ = -1;
    modelChanged();
}

void MainWindow::chooseTileIcon() {
    cfg::Binding* b = selectedBinding();
    if (!b) return;
    std::string rel = b->icon;
    // The library first, with Browse still one click away inside it.
    if (!pickIconDialog(hwnd_, rel)) return;
    b->icon = rel;
    icons_.clear();
    modelChanged(false);
}

void MainWindow::chooseTileColor() {
    cfg::Binding* b = selectedBinding();
    if (!b) return;
    std::string hex = b->color;
    if (!chooseColorHex(hwnd_, hex)) return;
    b->color = hex;
    modelChanged(false);
}

void MainWindow::toggleEngine() {
    if (engine_.running()) {
        engine_.stop();
    } else {
        try {
            engine_.updateConfig(config_);
            engine_.start();
        } catch (const fe::Error& e) {
            pushLog(std::string("Could not start listening: ") + e.what());
            showFriendly(hwnd_, fe::explain(e.problem, e.subject, e.what()));
        } catch (const std::exception& e) {
            // Already a sentence meant for the user ("add a keypad first").
            pushLog(std::string("Not listening: ") + e.what());
            showMessage(hwnd_, "Not listening yet", e.what());
        }
    }
    layoutEngineButton();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::importYaml() {
    wchar_t file[MAX_PATH * 2] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"macrokb config\0*.yaml;*.yml\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"Import a macrokb macros.yaml";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;

    cfg::Config imported;
    imported.settings = config_.settings;
    std::string err;
    if (!cfg::importMacroKbYaml(narrow(file), imported, &err)) {
        showFriendly(hwnd_, fe::Friendly{
                                "That file couldn't be imported",
                                "It doesn't look like a macrokb macros.yaml, "
                                "or part of it couldn't be read. Nothing was "
                                "added.",
                                "", err});
        return;
    }

    int keys = 0;
    for (const auto& kb : imported.keyboards) keys += totalBindings(kb);
    const size_t pads = imported.keyboards.size();
    if (!confirm(hwnd_,
                 "Add " + std::to_string(pads) +
                     (pads == 1 ? " keypad" : " keypads") + " and " +
                     std::to_string(keys) + (keys == 1 ? " button?" : " buttons?"),
                 "They are added next to the buttons you have now.",
                 "Add them", "Cancel"))
        return;

    for (auto& kb : imported.keyboards) {
        kb.alias = config_.uniqueAlias(kb.alias);
        config_.keyboards.push_back(kb);
    }
    config_.settings = imported.settings;
    saveConfig();
    engine_.updateConfig(config_);
    if (!config_.keyboards.empty())
        selectKeyboard(config_.keyboards.back().alias);
    layout();
    pushLog("Imported " + std::to_string(keys) + " macro(s) from macros.yaml.");
}

void MainWindow::reloadConfig() {
    if (config_.dirty &&
        !confirm(hwnd_, "Throw away your unsaved changes?",
                 "Reloading reads your buttons again from macros.json, as "
                 "they were last saved.",
                 "Reload and throw away", "Keep my changes", true))
        return;

    const bool wasRunning = engine_.running();
    if (wasRunning) engine_.stop();

    const std::string previous = currentAlias_;
    config_.load(dataPath("macros.json"));
    dailyBackupDone_ = false;
    icons_.clear();
    engine_.updateConfig(config_);

    currentAlias_.clear();
    selectedTile_ = -1;
    if (config_.keyboard(previous)) selectKeyboard(previous);
    else if (!config_.keyboards.empty())
        selectKeyboard(config_.keyboards[0].alias);

    reportLoadProblems();
    applyToggleHotkey();
    checkDevices(false);
    layout();
    refreshInspector();

    if (wasRunning && config_.loadError.empty() && !config_.keyboards.empty())
        toggleEngine();
    else if (wasRunning) {
        setTone(btnEngine_, Tone::Good);
        layoutEngineButton();
        pushLog("Stopped listening for the reload.");
    }
}

void MainWindow::exportConfig() {
    std::wstring initial = W("dentalmacros-" + dateStamp() + ".json");
    std::vector<wchar_t> file(MAX_PATH * 2, L'\0');
    wcsncpy(file.data(), initial.c_str(), file.size() - 1);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Dental Macros settings\0*.json\0All files\0*.*\0";
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrDefExt = L"json";
    ofn.lpstrTitle = L"Export a copy of your settings";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return;

    std::string err;
    const std::string dest = narrow(file.data());
    if (!config_.saveAs(dest, &err)) {
        showFriendly(hwnd_, fe::Friendly{
                                "The copy couldn't be saved",
                                "Windows didn't let Dental Macros write that "
                                "file. Try saving it somewhere else, such as "
                                "Documents.",
                                "", err});
        return;
    }
    pushLog("Exported a copy to " + dest);
}

void MainWindow::duplicateBinding(int tileIndex) {
    cfg::Keyboard* kb = currentKb();
    if (!kb || tileIndex < 0 || tileIndex >= (int)tiles_.size()) return;
    const int bi = tiles_[(size_t)tileIndex].binding;
    if (bi < 0 || bi >= (int)kb->active().bindings.size()) return;
    const cfg::Binding source = kb->active().bindings[(size_t)bi];

    const char* title =
        kb->isMouse ? "Duplicate to which button?" : "Duplicate to which key?";
    KeyCaptureResult r = captureKey(
        hwnd_, title,
        std::string(kb->isMouse ? "Press the button for the copy"
                                : "Press the key for the copy") +
            "\nThe copy of \"" + source.displayTitle() +
            "\" goes on whichever " + (kb->isMouse ? "button" : "key") +
            " you press on \"" + kb->alias + "\".",
        kb->match);
    if (!r.ok) return;

    if (kb->findKey(r.keyName)) {
        showMessage(hwnd_, "That key already has a button",
                    "\"" + keys::displayName(r.keyName) +
                        "\" already runs a button on this keypad. Pick another key, or change that button instead.");
        return;
    }

    cfg::Binding copy = source;
    copy.key = r.keyName;
    copy.slot = -1;   // let the deck place it
    if (!copy.title.empty()) copy.title += " copy";
    kb->active().bindings.push_back(std::move(copy));
    modelChanged();

    bool shown = false;
    for (size_t i = 0; i < tiles_.size(); ++i)
        if (tiles_[i].binding == (int)kb->active().bindings.size() - 1) {
            selectTile((int)i);
            shown = true;
            break;
        }
    if (!shown)
        pushLog("Copied to " + keys::displayName(r.keyName) +
                ". That key isn't on this keypad's layout, so it doesn't "
                "show here, but it still works.");
}

void MainWindow::copySteps() {
    cfg::Binding* b = selectedBinding();
    std::vector<cfg::Step>* steps = editingSteps();
    if (!b || !steps || steps->empty()) return;
    stepClipboard_ = *steps;
    pushLog("Copied " + std::to_string(stepClipboard_.size()) +
            " step(s) from \"" + b->displayTitle() + "\".");
}

void MainWindow::pasteSteps() {
    cfg::Binding* b = selectedBinding();
    std::vector<cfg::Step>* steps = editingSteps();
    if (!b || !steps || stepClipboard_.empty()) return;
    for (const cfg::Step& s : stepClipboard_) steps->push_back(s);
    modelChanged();
    SendMessageW(stepList_, LB_SETCURSEL, (WPARAM)(steps->size() - 1), 0);
    refreshInspector();
    pushLog("Pasted " + std::to_string(stepClipboard_.size()) +
            " step(s) into \"" + b->displayTitle() + "\".");
}

// --------------------------------------------------------------- pages
void MainWindow::setActivePage(const std::string& alias, int index) {
    cfg::Keyboard* kb = config_.keyboard(alias);
    if (!kb) return;
    kb->normalizePages();
    if (index < 0 || index >= (int)kb->pages.size()) return;
    if (kb->activePage == index) return;
    kb->activePage = index;

    // Which page is in front is deliberately not saved. A deck should come
    // back on the page its rules ask for, not on whichever one happened to be
    // up when the app was last closed - so this does not mark the config
    // dirty, and changing page never rewrites macros.json.
    engine_.updateConfig(config_);
    if (alias == currentAlias_) {
        // The selection belonged to the old page's binding list, and index 3
        // there is not index 3 here.
        selectedTile_ = -1;
        hoverTile_ = -1;
        deckScroll_ = 0;
        layoutDeck();
        refreshInspector();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    refreshMiniDeck();
}

void MainWindow::addPage() {
    cfg::Keyboard* kb = currentKb();
    if (!kb) return;
    std::string name =
        kb->uniquePageName("Page " + std::to_string(kb->pages.size() + 1));
    if (!askText(hwnd_, "Add page", "What should the new page be called?",
                 name, "", "Add page")) return;
    cfg::Page page;
    page.name = kb->uniquePageName(name);
    kb->pages.push_back(std::move(page));
    const int added = (int)kb->pages.size() - 1;
    const std::string alias = kb->alias;
    modelChanged(false);
    setActivePage(alias, added);
    pushLog("Added page \"" + name + "\" to " + alias + ".");
}

void MainWindow::movePage(int pageIndex, int delta) {
    cfg::Keyboard* kb = currentKb();
    if (!kb) return;
    const int to = pageIndex + delta;
    if (pageIndex < 0 || pageIndex >= (int)kb->pages.size()) return;
    if (to < 0 || to >= (int)kb->pages.size()) return;
    std::swap(kb->pages[(size_t)pageIndex], kb->pages[(size_t)to]);
    // Follow the page that moved rather than the position it left behind.
    if (kb->activePage == pageIndex) kb->activePage = to;
    else if (kb->activePage == to) kb->activePage = pageIndex;
    modelChanged();
}

void MainWindow::removePage(int pageIndex) {
    cfg::Keyboard* kb = currentKb();
    if (!kb || pageIndex < 0 || pageIndex >= (int)kb->pages.size()) return;
    if (kb->pages.size() == 1) {
        showMessage(hwnd_, "This is the only page",
                    "A keypad always has at least one page. Add another page "
                    "before you delete this one.");
        return;
    }
    const std::string removed = kb->pages[(size_t)pageIndex].displayName();
    const size_t onPage = kb->pages[(size_t)pageIndex].bindings.size();
    if (!confirm(hwnd_,
                 "Delete the \"" + removed + "\" page" +
                     (onPage == 0 ? std::string("?")
                                  : " and its " + std::to_string(onPage) +
                                        (onPage == 1 ? " button?" : " buttons?")),
                 "", "Delete page", "Keep it", true))
        return;
    kb->pages.erase(kb->pages.begin() + pageIndex);
    if (kb->activePage == pageIndex || kb->activePage >= (int)kb->pages.size())
        kb->activePage = kb->defaultPage();
    else if (kb->activePage > pageIndex)
        --kb->activePage;
    selectedTile_ = -1;
    modelChanged();
    pushLog("Removed page \"" + removed + "\".");
}

void MainWindow::editPageAppRule(int pageIndex) {
    cfg::Keyboard* kb = currentKb();
    if (!kb || pageIndex < 0 || pageIndex >= (int)kb->pages.size()) return;

    // Pick from what is actually running rather than asking anyone to type an
    // image name.
    std::vector<win::WindowInfo> wins = win::listWindows();
    // One entry per program: a rule on the executable is what almost everyone
    // wants, and six Explorer windows are not six choices.
    std::vector<win::WindowInfo> uniq;
    for (const win::WindowInfo& w : wins) {
        if (w.exe.empty()) continue;
        bool seen = false;
        for (const win::WindowInfo& u : uniq)
            if (iequals(u.exe, w.exe)) { seen = true; break; }
        if (!seen) uniq.push_back(w);
    }
    std::sort(uniq.begin(), uniq.end(),
              [](const win::WindowInfo& a, const win::WindowInfo& b) {
                  return lower(a.exe) < lower(b.exe);
              });

    cfg::Page& page = kb->pages[(size_t)pageIndex];
    std::vector<ListChoice> items;
    items.push_back({"No program", "This page comes up only when you pick it"});
    int current = page.rule.empty() ? 0 : -1;
    for (size_t i = 0; i < uniq.size(); ++i) {
        items.push_back({uniq[i].exe, uniq[i].title});
        if (iequals(uniq[i].exe, page.rule.exe)) current = (int)i + 1;
    }
    int chosen = current;
    if (!chooseFromList(hwnd_, "Page follows a program",
                        "Which program should bring up this page?",
                        "When it comes to the front, the buttons switch to \"" +
                            page.displayName() + "\". Only programs that are "
                            "open now are listed.",
                        items, chosen, "Save"))
        return;

    if (chosen == 0) {
        page.rule = cfg::AppRule{};
        pushLog("\"" + page.displayName() +
                "\" no longer follows an application.");
    } else {
        const size_t i = (size_t)(chosen - 1);
        if (i >= uniq.size()) return;
        page.rule = cfg::AppRule{};
        // Deliberately only the executable. Adding the class as well would
        // make the rule miss the moment that program opened a different kind
        // of window, which is the opposite of what picking it here means.
        page.rule.exe = uniq[i].exe;
        pushLog("\"" + page.displayName() + "\" now comes up for " +
                uniq[i].exe + ".");
    }
    modelChanged();
}

void MainWindow::pageContextMenu(int pageIndex, POINT screenPt) {
    cfg::Keyboard* kb = currentKb();
    if (!kb || pageIndex < 0 || pageIndex >= (int)kb->pages.size()) return;

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_PAGE_RENAME, L"Rename page...");
    AppendMenuW(m, MF_STRING, IDM_PAGE_APP,
                L"Bring up this page for a program...");
    if (!kb->pages[(size_t)pageIndex].rule.empty())
        AppendMenuW(m, MF_STRING, IDM_PAGE_APP_CLEAR,
                    L"Stop following the program");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_PAGE_COLOR, L"Change page colour...");
    if (!kb->pages[(size_t)pageIndex].color.empty())
        AppendMenuW(m, MF_STRING, IDM_PAGE_COLOR_CLEAR, L"Remove page colour");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_PAGE_DUPLICATE, L"Duplicate page");
    AppendMenuW(m, MF_STRING | (pageIndex > 0 ? 0u : (UINT)MF_GRAYED),
                IDM_PAGE_LEFT, L"Move page left");
    AppendMenuW(m,
                MF_STRING | (pageIndex + 1 < (int)kb->pages.size()
                                 ? 0u
                                 : (UINT)MF_GRAYED),
                IDM_PAGE_RIGHT, L"Move page right");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_PAGE_REMOVE, L"Delete page...");

    const int cmd = (int)TrackPopupMenu(
        m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, screenPt.x, screenPt.y,
        0, hwnd_, nullptr);
    DestroyMenu(m);
    if (!cmd) return;

    cfg::Page& page = kb->pages[(size_t)pageIndex];
    switch (cmd) {
        case IDM_PAGE_RENAME: {
            std::string name = page.name;
            if (askText(hwnd_, "Rename page", "What should this page be called?",
                        name, "", "Save name")) {
                const std::string want = trim(name);
                // uniquePageName would put a number after a name that is only
                // taken by this very page, so ask only when it really moved.
                page.name = (kb->pageByName(want) == pageIndex)
                                ? want
                                : kb->uniquePageName(want);
                modelChanged();
            }
            break;
        }
        case IDM_PAGE_APP:
            editPageAppRule(pageIndex);
            break;
        case IDM_PAGE_APP_CLEAR:
            page.rule = cfg::AppRule{};
            modelChanged();
            break;
        case IDM_PAGE_COLOR: {
            std::string hex = page.color;
            if (chooseColorHex(hwnd_, hex)) {
                page.color = hex;
                modelChanged();
            }
            break;
        }
        case IDM_PAGE_COLOR_CLEAR:
            page.color.clear();
            modelChanged();
            break;
        case IDM_PAGE_DUPLICATE: {
            cfg::Page copy = page;
            copy.name = kb->uniquePageName(page.displayName() + " copy");
            // The copy does not inherit the rule: two pages claiming one
            // application would leave which of them you get down to page
            // order, which is not what pressing Duplicate asked for.
            copy.rule = cfg::AppRule{};
            kb->pages.push_back(std::move(copy));
            const int added = (int)kb->pages.size() - 1;
            const std::string alias = kb->alias;
            modelChanged(false);
            setActivePage(alias, added);
            break;
        }
        case IDM_PAGE_LEFT:
            movePage(pageIndex, -1);
            break;
        case IDM_PAGE_RIGHT:
            movePage(pageIndex, +1);
            break;
        case IDM_PAGE_REMOVE:
            removePage(pageIndex);
            break;
        default:
            break;
    }
}

void MainWindow::drainPageRequests() {
    std::deque<std::pair<std::string, int>> pending;
    {
        std::lock_guard<std::mutex> lk(pageMu_);
        pending.swap(pageQueue_);
    }
    // A macro asking for a page is not the user picking one by hand, so it
    // does not stand down the application rules the way a click does.
    for (const auto& req : pending) setActivePage(req.first, req.second);
}

void MainWindow::onForegroundChanged(HWND fg) {
    if (!config_.settings.switchPageWithApp || !fg) return;

    // Dental Macros's own windows are not an application change. Editing a deck
    // would otherwise count as leaving whatever you were working in, and the
    // page would move the moment you came here to look at it.
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return;

    win::WindowInfo info;
    if (!win::describeWindow(fg, info)) return;
    const std::string id = info.exe + "|" + info.cls + "|" + info.title;
    // Nothing happens unless the window in front is really a different one.
    // This is also what makes a page you picked by hand stay put: clicking
    // around inside the application you are already in raises no event that
    // could take it away from you, and only moving to another one does.
    if (id == lastForeground_) return;
    lastForeground_ = id;

    for (size_t i = 0; i < config_.keyboards.size(); ++i) {
        config_.keyboards[i].normalizePages();
        const std::string alias = config_.keyboards[i].alias;
        const int want = config_.keyboards[i].pageForApp(info.exe, info.cls,
                                                         info.title);
        if (want == config_.keyboards[i].activePage) continue;
        setActivePage(alias, want);
        if (const cfg::Keyboard* kb = config_.keyboard(alias))
            pushLog(alias + ": " + info.exe + " brought up \"" +
                    kb->active().displayName() + "\".");
    }
}

void MainWindow::startForegroundHook() {
    if (fgHook_) return;
    fgHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                              nullptr, &foregroundEventProc, 0, 0,
                              WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!fgHook_)
        pushLog("Windows would not say which application is in front - pages "
                "will only change when you ask them to.");
}

void MainWindow::stopForegroundHook() {
    if (!fgHook_) return;
    UnhookWinEvent(fgHook_);
    fgHook_ = nullptr;
}

// ------------------------------------------------------- global start/stop
void MainWindow::releaseToggleHotkey() {
    if (!hotkeyRegistered_) return;
    UnregisterHotKey(hwnd_, kToggleHotkeyId);
    hotkeyRegistered_ = false;
    activeHotkey_.clear();
}

void MainWindow::applyToggleHotkey() {
    releaseToggleHotkey();
    const std::string combo = trim(config_.settings.toggleHotkey);
    if (combo.empty()) return;

    unsigned mods = 0;
    int vk = 0;
    std::string err;
    if (!keys::parseHotkey(combo, mods, vk, &err)) {
        pushLog("Start/stop shortcut \"" + combo + "\" ignored: " + err);
        return;
    }
    // MOD_NOREPEAT: holding the combo should toggle once, not flip the engine
    // on and off dozens of times.
    if (RegisterHotKey(hwnd_, kToggleHotkeyId, mods | MOD_NOREPEAT, (UINT)vk)) {
        hotkeyRegistered_ = true;
        activeHotkey_ = combo;
        pushLog("Start/stop listening from anywhere with " +
                keys::prettyCombo(combo) + ".");
    } else {
        // An off switch that doesn't work is worse than a different one, so
        // borrow a stand-in for this run. The saved choice is left alone and
        // tried first again next time, in case the other program has gone.
        static const char* kStandIns[] = {"ctrl+alt+shift+m", "ctrl+alt+shift+k",
                                          "ctrl+alt+shift+f12",
                                          "ctrl+alt+shift+pause"};
        for (const char* standIn : kStandIns) {
            if (iequals(standIn, combo)) continue;
            unsigned smods = 0;
            int svk = 0;
            std::string serr;
            if (!keys::parseHotkey(standIn, smods, svk, &serr)) continue;
            if (!RegisterHotKey(hwnd_, kToggleHotkeyId, smods | MOD_NOREPEAT,
                                (UINT)svk))
                continue;
            hotkeyRegistered_ = true;
            activeHotkey_ = standIn;
            break;
        }
        if (hotkeyRegistered_) {
            pushLog("Another program already owns " + keys::prettyCombo(combo) +
                    ", so start/stop listening is " +
                    keys::prettyCombo(activeHotkey_) + " for now.");
            setStatus("Your off switch is " + keys::prettyCombo(activeHotkey_) +
                          " for now, because another program is using " +
                          keys::prettyCombo(combo) + ".",
                      StatusTone::Warn);
        } else {
            pushLog("Another program already owns " + keys::prettyCombo(combo) +
                    " - pick a different start/stop shortcut in Settings.");
            setStatus("The off switch isn't working, because another "
                      "program is using it. Choose another in Settings.",
                      StatusTone::Warn);
        }
    }
    // The button spells the shortcut out, so it has to be rebuilt whenever
    // the shortcut changes or fails to register.
    layoutEngineButton();
}

void MainWindow::addSoftwareDeck() {
    std::string name = "Buttons";
    if (!askText(hwnd_, "Add on-screen buttons",
                 "What should this set of buttons be called?", name,
                 "You press them in this window or on the floating buttons. "
                 "No keypad is needed.",
                 "Add buttons"))
        return;
    cfg::Keyboard kb;
    kb.alias = config_.uniqueAlias(name.empty() ? "Buttons" : name);
    kb.softwareOnly = true;
    kb.block = false;        // never reaches the hook
    kb.match.clear();        // matches no device: specMatches("") is false
    kb.deck = "auto";
    config_.keyboards.push_back(std::move(kb));
    saveConfig();
    engine_.updateConfig(config_);
    selectKeyboard(config_.keyboards.back().alias);
    pushLog("Added on-screen buttons \"" + config_.keyboards.back().alias +
            "\". No keypad is involved.");
    layout();
}

void MainWindow::keyboardContextMenu(int index, POINT pt) {
    if (index < 0 || index >= (int)config_.keyboards.size()) return;
    cfg::Keyboard& kb = config_.keyboards[(size_t)index];

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_KB_RENAME,
                kb.softwareOnly ? L"Rename these buttons..." : L"Rename keypad...");
    if (!simple() && !kb.softwareOnly) {
        // Ticked means the keys don't type: the same words as Settings.
        AppendMenuW(m, MF_STRING | (kb.block ? MF_CHECKED : 0), IDM_KB_BLOCK,
                    L"Stop its button keys from typing");
        // Greyed rather than hidden when the keyboard blocks nothing at all:
        // the option still describes this keyboard, it just has nothing to
        // act on.
        AppendMenuW(m,
                    MF_STRING | (kb.blockAllKeys ? MF_CHECKED : 0) |
                        (kb.block ? 0 : MF_GRAYED),
                    IDM_KB_BLOCK_ALL,
                    L"      Stop every key from typing");
    }
    if (!kb.softwareOnly)
        AppendMenuW(m, MF_STRING, IDM_KB_REMATCH,
                    L"Find this keypad again...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_KB_REMOVE,
                kb.softwareOnly ? L"Delete these buttons..." : L"Delete keypad...");
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x, pt.y,
                                  0, hwnd_, nullptr);
    DestroyMenu(m);

    switch (cmd) {
        case IDM_KB_RENAME: {
            std::string name = kb.alias;
            if (askText(hwnd_, "Rename keypad",
                        "What should this keypad be called?", name, "",
                        "Save name")) {
                std::string old = kb.alias;
                kb.alias = config_.uniqueAlias(name);
                if (currentAlias_ == old) currentAlias_ = kb.alias;
                modelChanged();
            }
            break;
        }
        case IDM_KB_BLOCK:
            kb.block = !kb.block;
            modelChanged(false);
            if (engine_.running())
                pushLog("Stop and start listening again for the blocking "
                        "change to apply.");
            break;
        case IDM_KB_BLOCK_ALL:
            kb.blockAllKeys = !kb.blockAllKeys;
            modelChanged(false);
            pushLog(kb.blockAllKeys
                        ? "\"" + kb.alias +
                              "\" now gives up every key while listening."
                        : "\"" + kb.alias +
                              "\" now gives up only the keys that have a "
                              "button. The rest type normally.");
            // Unlike the master toggle this needs no restart: the hook asks
            // the config afresh on every keystroke.
            break;
        case IDM_KB_REMATCH: {
            KeyCaptureResult r = captureKey(
                hwnd_, "Find this keypad again",
                "Press a key on the keypad\n\"" + kb.alias +
                    "\" will mean whichever keypad you press it on.",
                "", false, false);
            if (r.ok && !r.devicePath.empty()) {
                auto all = dev::listKeyboards();
                // The input the key came in on - or, failing that, another
                // input of the same keypad: a media key on a Bluetooth keypad
                // arrives on an input that is not a keyboard at all, and on
                // some keypads that is every button there is.
                const dev::InputDevice* found = nullptr;
                for (const auto& d : all)
                    if (d.path == r.devicePath) { found = &d; break; }
                if (!found)
                    for (const auto& d : all)
                        if (dev::sameDevice(d.path, r.devicePath)) {
                            found = &d;
                            break;
                        }
                if (found) {
                    bool stable = true;
                    kb.match = cfg::chooseMatch(*found, all, &stable);
                    modelChanged(false);
                    pushLog("\"" + kb.alias + "\" now matches " + kb.match);
                }
            }
            break;
        }
        case IDM_KB_REMOVE: {
            const int all = totalBindings(kb);
            if (!confirm(hwnd_,
                         "Remove \"" + kb.alias + "\"" +
                             (all == 0 ? std::string("?")
                                       : " and its " + std::to_string(all) +
                                             (all == 1 ? " button?" : " buttons?")),
                         "The keypad itself goes back to typing normally.",
                         "Remove keypad", "Keep it", true))
                break;
            std::string removed = kb.alias;
            config_.keyboards.erase(config_.keyboards.begin() + index);
            if (currentAlias_ == removed)
                currentAlias_ = config_.keyboards.empty()
                                    ? ""
                                    : config_.keyboards[0].alias;
            selectedTile_ = -1;
            saveConfig();
            engine_.updateConfig(config_);
            layout();
            refreshInspector();
            break;
        }
    }
    // Renamed, re-detected or removed: the dots are keyed by name and match.
    checkDevices(false);
}

void MainWindow::tileContextMenu(int tileIndex, POINT pt) {
    selectTile(tileIndex);
    cfg::Binding* b = selectedBinding();

    HMENU m = CreatePopupMenu();
    if (!b) {
        AppendMenuW(m, MF_STRING, IDM_TILE_REKEY, L"Add a button here...");
    } else {
        AppendMenuW(m, MF_STRING, IDM_TILE_RENAME, L"Rename button...");
        if (!simple()) {
            AppendMenuW(m, MF_STRING, IDM_TILE_ICON, L"Change icon...");
            if (!b->icon.empty())
                AppendMenuW(m, MF_STRING, IDM_TILE_ICON_CLEAR,
                            L"Remove icon");
        }
        AppendMenuW(m, MF_STRING, IDM_TILE_COLOR, L"Change colour...");
        if (!b->color.empty())
            AppendMenuW(m, MF_STRING, IDM_TILE_COLOR_CLEAR,
                        L"Use the standard colour");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (b->steps.empty() ? MF_GRAYED : 0),
                    IDM_TILE_TEST, L"Try it");
        if (!simple()) {
        AppendMenuW(m, MF_STRING, IDM_TILE_DUPLICATE, L"Copy to another key...");
        AppendMenuW(m, MF_STRING | (b->steps.empty() ? MF_GRAYED : 0),
                    IDM_TILE_COPY_STEPS, L"Copy steps");
        AppendMenuW(m, MF_STRING | (stepClipboard_.empty() ? MF_GRAYED : 0),
                    IDM_TILE_PASTE_STEPS,
                    stepClipboard_.empty()
                        ? L"Paste steps"
                        : (L"Paste " +
                           std::to_wstring(stepClipboard_.size()) +
                           (stepClipboard_.size() == 1 ? L" step" : L" steps"))
                              .c_str());
        AppendMenuW(m, MF_STRING | (b->steps.empty() ? MF_GRAYED : 0),
                    IDM_TILE_CLEAR, L"Remove all steps...");
        }
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_TILE_REMOVE,
                    L"Delete button...");
    }
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x, pt.y,
                                  0, hwnd_, nullptr);
    DestroyMenu(m);

    switch (cmd) {
        case IDM_TILE_REKEY:
            assignKeyToTile(tileIndex);
            break;
        case IDM_TILE_RENAME: {
            if (!b) break;
            std::string t = b->title;
            if (askText(hwnd_, "Rename button",
                        "What should this button say?", t,
                        "Short words fit best, like 'Next patient'",
                        "Save name")) {
                b->title = t;
                modelChanged(false);
                refreshInspector();
            }
            break;
        }
        case IDM_TILE_ICON: chooseTileIcon(); break;
        case IDM_TILE_ICON_CLEAR:
            if (b) { b->icon.clear(); modelChanged(false); }
            break;
        case IDM_TILE_COLOR: chooseTileColor(); break;
        case IDM_TILE_COLOR_CLEAR:
            if (b) { b->color.clear(); modelChanged(false); }
            break;
        case IDM_TILE_TEST: testRun(); break;
        case IDM_TILE_DUPLICATE: duplicateBinding(tileIndex); break;
        case IDM_TILE_COPY_STEPS: copySteps(); break;
        case IDM_TILE_PASTE_STEPS: pasteSteps(); break;
        case IDM_TILE_CLEAR: clearTile(); break;
        case IDM_TILE_REMOVE: deleteBinding(); break;
    }
}

// --------------------------------------------------------- first-run setup
// Welcome, checks, first button - then turn the answers into a deck with that
// one button on it, ready to press.
void MainWindow::runSetup() {
    SetupContext ctx;
    ctx.offSwitch = activeHotkey_.empty() ? "" : keys::prettyCombo(activeHotkey_);
    ctx.runAtLogin = runAtLoginEnabled();

    SetupResult r;
    const bool done = runSetupWizard(hwnd_, ctx, r);
    // Finished or skipped, it has been offered: never again uninvited.
    config_.settings.setupDone = true;
    if (!done) {
        config_.dirty = true;
        saveConfig();
        pushLog("Setup skipped.");
        setStatus("You can run setup again any time from Settings.");
        return;
    }

    cfg::Keyboard kb;
    cfg::Binding b;
    if (r.onScreen) {
        kb.alias = config_.uniqueAlias("My buttons");
        kb.softwareOnly = true;
        kb.block = false;   // nothing to block: no keyboard behind it
        b.key = "btn1";
    } else {
        kb.alias = config_.uniqueAlias(
            !r.deviceName.empty() && r.deviceName.size() <= 24 ? r.deviceName
                                                               : "My keypad");
        kb.match = r.match;
        kb.block = true;
        // Only keys that carry a button stop typing. If the wrong keyboard was
        // ever picked here, that costs one key rather than all of them.
        kb.blockAllKeys = false;
        b.key = r.keyName;
    }
    kb.deck = "auto";
    b.title = r.title;
    b.steps = r.steps;
    kb.active().bindings.push_back(b);
    const std::string alias = kb.alias;
    config_.keyboards.push_back(std::move(kb));

    config_.settings.simpleMode = true;
    // Listen from now on, whichever kind of buttons were set up.
    config_.settings.startEngineOnLaunch = true;
    if (r.runAtLogin != runAtLoginEnabled()) {
        std::string err;
        if (!setRunAtLogin(r.runAtLogin, &err))
            pushLog("Could not change starting with Windows: " + err);
    }
    config_.dirty = true;
    saveConfig();
    engine_.updateConfig(config_);

    selectKeyboard(alias);
    layout();
    refreshInspector();
    refreshModeButtons();
    checkDevices(false);
    for (size_t i = 0; i < tiles_.size(); ++i)
        if (tiles_[i].binding == 0) {
            selectTile((int)i);
            break;
        }

    if (!engine_.running()) toggleEngine();
    refreshMiniDeck();
    if (r.onScreen && r.floating && !mini_.visible()) toggleMiniDeck();

    pushLog("Setup finished: \"" + r.title + "\" on " +
            (r.onScreen ? std::string("on-screen buttons")
                        : "the " + keys::displayName(r.keyName) + " key of " +
                              alias) +
            ".");
    if (!r.onScreen && !r.matchStable)
        pushLog("  Two identical keyboards are attached, so the keypad is "
                "recognised by its USB port - keep it in the same one.");
    setStatus("\xE2\x9C\x93 You're set. Press your new button to try it.",
              StatusTone::Good);
}

// ------------------------------------------------------------ simple mode
// Simple mode is a view, not a different file: it shows a button's steps as
// a few plain rows (smp::rowsFor) and makes only four kinds of them. Steps
// it doesn't make are shown as an "advanced steps" row and left alone.
void MainWindow::setSimpleMode(bool on) {
    if (simple() == on) return;
    config_.settings.simpleMode = on;
    gesture_ = 0;              // Simple shows what a press does, nothing else
    stepListFor_.clear();      // the rows mean different things now
    saveConfig();
    layout();
    refreshInspector();
    refreshModeButtons();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::refreshModeButtons() {
    setPillActive(btnSimple_, simple());
    setPillActive(btnAdvanced_, !simple());
}

// Select the list row that shows step `index` - in Simple mode a row can
// stand for several steps.
void MainWindow::selectStep(size_t index) {
    const int row = simple() ? smp::rowOfStep(simpleRows_, index) : (int)index;
    SendMessageW(stepList_, LB_SETCURSEL, (WPARAM)row, 0);
    updateStepButtons();
}

int MainWindow::selectedRow() const {
    const int sel = (int)SendMessageW(stepList_, LB_GETCURSEL, 0, 0);
    return sel >= 0 && sel < (int)simpleRows_.size() ? sel : -1;
}

// 0 = type text, 1 = press keys, 2 = wait.
void MainWindow::addSimpleStep(int which) {
    if (!selectedBinding()) return;
    cfg::Step step;
    step.params = js::Value::object();
    if (which == 0) {
        std::string text;
        if (!askLongText(hwnd_, "Add text", "What should it type?", text,
                         "Press Enter for a new line."))
            return;
        step.action = "type";
        step.params["text"] = text;
    } else if (which == 1) {
        const std::string combo = askCombo();
        if (combo.empty()) return;
        step.action = "keys";
        step.params["combo"] = combo;
    } else {
        double seconds = 1.0;
        if (!chooseWait(hwnd_, seconds)) return;
        step.action = "sleep";
        step.params["seconds"] = seconds;
    }
    // Asked for again: the binding is not held across a dialog.
    std::vector<cfg::Step>* steps = editingSteps();
    if (!steps) return;
    steps->push_back(step);
    modelChanged();
    selectStep(steps->size() - 1);
}

std::string MainWindow::askCombo() {
    KeyCaptureResult r = captureKey(
        hwnd_, "Press keys",
        "Press the keys you want it to press\nFor example, hold Ctrl and "
        "press S.",
        "", true, false);
    if (!r.ok) return "";
    return r.combo.empty() ? r.keyName : r.combo;
}

void MainWindow::editSimpleRow(int row) {
    if (row < 0 || row >= (int)simpleRows_.size()) return;
    const smp::Row r = simpleRows_[(size_t)row];
    std::vector<cfg::Step>* steps = editingSteps();
    if (!steps || r.first >= steps->size()) return;
    // Copied: the list may be rebuilt while a dialog is open.
    cfg::Step step = (*steps)[r.first];

    switch (r.kind) {
        case smp::Kind::Type: {
            std::string text = step.params.str("text");
            if (!askLongText(hwnd_, "Type text", "What should it type?", text,
                             "Press Enter for a new line.", "Save text"))
                return;
            step.params["text"] = text;
            break;
        }
        case smp::Kind::Keys: {
            const std::string combo = askCombo();
            if (combo.empty()) return;
            step.params["combo"] = combo;
            break;
        }
        case smp::Kind::Wait: {
            double seconds = step.params.num("seconds", 1.0);
            if (!chooseWait(hwnd_, seconds)) return;
            step.params["seconds"] = seconds;
            break;
        }
        case smp::Kind::Clicks:
            if (!confirm(hwnd_, "Record these clicks again?",
                         "The clicks you record now will replace these ones.",
                         "Record again", "Cancel"))
                return;
            recordClicks((int)r.first, (int)r.count);
            return;
        case smp::Kind::Advanced:
            // Simple mode never edits these. Show them where they can be.
            setSimpleMode(false);
            selectStep(r.first);
            pushLog("Switched to Advanced to show the advanced steps.");
            return;
    }

    steps = editingSteps();
    if (!steps || r.first >= steps->size()) return;
    (*steps)[r.first] = step;
    modelChanged();
    selectStep(r.first);
}

void MainWindow::removeSimpleRow(int row) {
    if (row < 0 || row >= (int)simpleRows_.size()) return;
    const smp::Row r = simpleRows_[(size_t)row];
    if (r.kind == smp::Kind::Advanced) return;   // never from Simple mode
    std::vector<cfg::Step>* steps = editingSteps();
    if (!steps || r.first + r.count > steps->size()) return;
    steps->erase(steps->begin() + (std::ptrdiff_t)r.first,
                 steps->begin() + (std::ptrdiff_t)(r.first + r.count));
    modelChanged();
    if (!simpleRows_.empty())
        SendMessageW(stepList_, LB_SETCURSEL,
                     (WPARAM)std::min(row, (int)simpleRows_.size() - 1), 0);
    updateStepButtons();
}


// --------------------------------------------------------- keypad presence
// Look for every keypad's device, and say so when one comes or goes. A
// Bluetooth keypad that has gone to sleep looks exactly like one that was
// never set up - except for this dot.
void MainWindow::checkDevices(bool announce) {
    std::vector<dev::InputDevice> attached = dev::listKeyboards();
    for (auto& m : dev::listMice()) attached.push_back(std::move(m));

    std::map<std::string, bool> now;
    std::string gone, back;
    int goneCount = 0;
    for (const cfg::Keyboard& kb : config_.keyboards) {
        if (kb.softwareOnly || kb.match.empty()) continue;
        bool there = false;
        for (const auto& d : attached)
            if (cfg::specMatches(kb.match, d.path)) { there = true; break; }
        now[kb.alias] = there;

        auto was = present_.find(kb.alias);
        const bool known = was != present_.end();
        if (!there && (!known || was->second)) {
            gone = kb.alias;
            ++goneCount;
        }
        if (there && known && !was->second) back = kb.alias;
    }
    const bool changed = now != present_;
    present_ = std::move(now);
    if (changed) InvalidateRect(hwnd_, &rcSidebar_, FALSE);
    if (!announce) return;

    if (goneCount > 1) {
        pushLog(std::to_string(goneCount) + " keypads aren't connected.");
        setStatus(std::to_string(goneCount) +
                      " keypads aren't connected. Their buttons won't work "
                      "until you switch them on or plug them back in.",
                  StatusTone::Warn);
    } else if (!gone.empty()) {
        pushLog("\"" + gone + "\" isn't connected.");
        setStatus(gone + " isn't connected. Its buttons won't work until you "
                  "switch it on or plug it back in.",
                  StatusTone::Warn);
    } else if (!back.empty()) {
        pushLog("\"" + back + "\" is connected again.");
        setStatus("\xE2\x9C\x93 " + back + " is connected again.",
                  StatusTone::Good);
    }
}

// ------------------------------------------------------------ status line
// One line beside the Activity heading: the last thing worth knowing, in
// plain words - a button that ran, one that stopped and why, a save that
// failed. The log underneath keeps the technical detail.
void MainWindow::setStatus(const std::string& text, StatusTone tone) {
    // A success is a toast and leaves whatever problem banner there is alone
    // - except that a success that is the problem going away ("connected
    // again", "saving works again") clears the banner it answers. Those are
    // the only Good statuses that come straight after a Warn or Danger of
    // their own, so any Good clears the banner.
    // Before the window's controls exist there is nothing to lay out yet;
    // onCreate lays everything out once they do.
    const bool ready = btnSettings_ != nullptr;
    statusFromRun_ = false;
    if (tone == StatusTone::Good) {
        const bool hadBanner = bannerShown();
        statusText_.clear();
        statusTone_ = StatusTone::Good;
        showToast(text, true);
        if (hadBanner && ready) layout();
        return;
    }
    const bool layoutChanges = !bannerShown();
    statusText_ = text;
    statusTone_ = tone;
    bannerDismissed_ = false;
    if (!ready) return;
    if (layoutChanges) layout();
    else InvalidateRect(hwnd_, &rcBanner_, FALSE);
}

void MainWindow::drainOutcomes() {
    std::deque<eng::Outcome> batch;
    {
        std::lock_guard<std::mutex> lk(outcomeMu_);
        batch.swap(outcomes_);
    }
    for (const eng::Outcome& o : batch) {
        if (o.ok) {
            // A button working again answers a "stopped" banner, but not one
            // about anything else - a keypad that is still unplugged, say.
            if (statusFromRun_ && bannerShown()) {
                statusText_.clear();
                statusFromRun_ = false;
                layout();
            }
            showToast("\xE2\x9C\x93 \"" + o.button + "\" ran", true);
            continue;
        }
        const std::string line = fe::stoppedLine(o.button, o.step, o.problem,
                                                 o.subject, o.technical);
        setStatus(line, StatusTone::Warn);
        statusFromRun_ = true;
        // Buttons mostly run with this window out of sight - pressed on the
        // keypad or the floating buttons - where the status line can't be seen.
        if (!IsWindowVisible(hwnd_) || IsIconic(hwnd_))
            noticeWhileAway("A button stopped", line);
    }
}

void MainWindow::noticeWhileAway(const std::string& title,
                                 const std::string& text) {
    // One at a time: a key pressed over and over against a closed program
    // should not become a stack of notifications.
    const ULONGLONG now = GetTickCount64();
    if (lastAwayNotice_ && now - lastAwayNotice_ < 10000) return;
    lastAwayNotice_ = now;

    if (trayAdded_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = 1;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_WARNING;
        wcsncpy(nid.szInfoTitle, W(title).c_str(), 63);
        wcsncpy(nid.szInfo, W(text).c_str(), 255);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    } else {
        FLASHWINFO fi{};
        fi.cbSize = sizeof(fi);
        fi.hwnd = hwnd_;
        fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
        FlashWindowEx(&fi);
    }
}

// ------------------------------------------------------- settings problems
// The newest daily backup that loads cleanly, and the date it was taken.
// A backup made after the file went bad is a copy of the damage, so each is
// tried rather than trusting the newest.
bool MainWindow::findRestorableBackup(std::string& path, std::string& date) {
    const std::string dir = dataPath("backups");
    std::vector<std::string> names = listFiles(dir, ".json");
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        const std::string& name = *it;
        if (name.rfind("macros-", 0) != 0 || name.size() < 17) continue;
        cfg::Config probe;
        probe.load(joinPath(dir, name));
        if (!probe.loadError.empty()) continue;
        path = joinPath(dir, name);
        date = name.substr(7, 10);
        return true;
    }
    return false;
}

void MainWindow::explainLoadProblem() {
    if (config_.loadError.empty()) return;
    if (config_.loadLocked) {
        if (showFriendly(hwnd_, fe::explain(fe::Problem::ConfigLocked, "",
                                            config_.loadError)))
            reloadConfig();
        return;
    }
    std::string backup, date;
    const bool have = findRestorableBackup(backup, date);
    const fe::Friendly f =
        fe::explain(fe::Problem::ConfigDamaged,
                    have ? fe::friendlyDate(date) : "", config_.loadError);
    if (showFriendly(hwnd_, f) && have) restoreBackup(backup, date);
}

void MainWindow::restoreBackup(const std::string& backup,
                               const std::string& date) {
    const std::string current = dataPath("macros.json");
    std::string kept;
    if (fileExists(current)) {
        // The damaged file is kept, never replaced in place: whatever is
        // wrong with it, it may still hold work worth getting back.
        kept = freeBackupPath(current);
        if (!copyFile(current, kept, false)) {
            showMessage(hwnd_, "Nothing was changed",
                        "Dental Macros couldn't keep a copy of the damaged "
                        "settings file, so it didn't replace it.",
                        true);
            return;
        }
    }
    if (!copyFile(backup, current, true)) {
        showMessage(hwnd_, "Nothing was changed",
                    "Windows didn't let Dental Macros put the saved copy in "
                    "place.",
                    true);
        return;
    }
    config_.dirty = false;
    reloadConfig();
    if (!config_.loadError.empty()) return;   // explained again by the reload
    pushLog("Restored " + relativeToApp(backup) + ".");
    if (!kept.empty())
        pushLog("  The damaged file was kept as " + relativeToApp(kept) + ".");
    setStatus("\xE2\x9C\x93 Your buttons are back, from the copy saved on " +
                  fe::friendlyDate(date) + ".",
              StatusTone::Good);
}

// ------------------------------------------------------------------- logs
void MainWindow::pushLog(const std::string& line) {
    {
        std::lock_guard<std::mutex> lk(logMu_);
        if (logQueue_.size() > 500) logQueue_.pop_front();
        logQueue_.push_back(timeStamp() + "  " + line);
    }
    if (hwnd_) PostMessageW(hwnd_, WM_APP_LOG, 0, 0);
}

void MainWindow::drainLogs() {
    std::deque<std::string> batch;
    {
        std::lock_guard<std::mutex> lk(logMu_);
        batch.swap(logQueue_);
    }
    if (batch.empty() || !logEdit_) return;

    // Keep the control from growing without bound.
    int lines = (int)SendMessageW(logEdit_, EM_GETLINECOUNT, 0, 0);
    if (lines > 400) setText(logEdit_, "");

    std::string add;
    for (const auto& l : batch) add += l + "\r\n";
    int len = GetWindowTextLengthW(logEdit_);
    SendMessageW(logEdit_, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(logEdit_, EM_REPLACESEL, FALSE, (LPARAM)W(add).c_str());
    SendMessageW(logEdit_, EM_SCROLLCARET, 0, 0);
}

void MainWindow::drainKeyEvents() {
    std::deque<eng::MonitorEvent> batch;
    {
        std::lock_guard<std::mutex> lk(keyMu_);
        batch.swap(keyQueue_);
    }
    bool repaint = false;
    for (const auto& e : batch) {
        if (!e.down) continue;
        if (e.alias.empty()) continue;
        lastKeyText_ = "Last key: " + keys::displayName(e.keyName) + " on " +
                       e.alias + (e.hasMacro ? "" : " (no button)");
        lastFire_[e.alias + "|" + e.keyName] = GetTickCount();
        if (e.alias == currentAlias_) mini_.noteFired(e.keyName);
        repaint = true;
    }
    if (repaint) {
        SetTimer(hwnd_, 2, 60, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// -------------------------------------------------------------- mini deck
// The small always-on-top palette. It shows the selected keyboard's keys and
// nothing else: one keyboard's worth of buttons is what fits in a window this
// size, and guessing which of several pads the user meant would be worse than
// following the one already on screen behind it.
void MainWindow::refreshMiniDeck() {
    cfg::Keyboard* kb = currentKb();
    std::vector<MiniButton> out;
    MiniPage page;
    if (kb) {
        kb->normalizePages();
        page.name = kb->active().displayName();
        page.color = kb->active().color;
        page.index = kb->activePage;
        page.count = (int)kb->pages.size();
        for (const cfg::Binding& b : kb->active().bindings) {
            MiniButton mb;
            mb.key = b.key;
            mb.title = b.displayTitle();
            mb.icon = b.icon;
            mb.color = b.color;
            mb.hasSteps = !b.steps.empty();
            mb.isDial = b.isDial();
            mb.showKey = !kb->softwareOnly;
            if (mb.hasSteps) mb.firstAction = b.steps[0].action;
            out.push_back(std::move(mb));
        }
    }
    mini_.setPage(page);
    mini_.setButtons(kb ? kb->alias : std::string("Dental Macros"), std::move(out));
}

void MainWindow::runBindingByKey(const std::string& key) {
    cfg::Keyboard* kb = currentKb();
    if (!kb) return;
    cfg::Binding* b = kb->findKey(key);
    if (!b || b->steps.empty()) return;
    // Same path the pad takes, so a macro behaves identically however it was
    // started - including running with the engine stopped, which is the point
    // of a palette you can click.
    // A software button's key is an id, so name it by its label instead -
    // "Btn1" tells nobody which button was pressed.
    pushLog("Buttons: " +
            (kb->softwareOnly ? b->displayTitle() : keys::displayName(b->key)) +
            " (" + std::to_string(b->steps.size()) + " steps)");
    lastFire_[currentAlias_ + "|" + b->key] = GetTickCount();
    SetTimer(hwnd_, 2, 60, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
    engine_.runSequenceAsync(b->steps, "Buttons", currentAlias_,
                             b->displayTitle());
}

void MainWindow::saveMiniPlacement() {
    MiniPlacement p = mini_.placement();
    cfg::Settings& st = config_.settings;
    if (st.miniDeckVisible == p.visible && st.miniDeckX == p.x &&
        st.miniDeckY == p.y && st.miniDeckTile == p.tile &&
        st.miniDeckCols == p.cols)
        return;
    st.miniDeckVisible = p.visible;
    st.miniDeckX = p.x;
    st.miniDeckY = p.y;
    st.miniDeckTile = p.tile;
    st.miniDeckCols = p.cols;
    config_.dirty = true;
}

void MainWindow::toggleMiniDeck() {
    refreshMiniDeck();
    mini_.toggle(inst_);
    saveMiniPlacement();
    saveConfig();
}

// ------------------------------------------------------------------- tray
bool MainWindow::addTray() {
    if (trayAdded_) return true;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = iconSm_ ? iconSm_ : icon_;
    wcsncpy(nid.szTip, L"Dental Macros", 127);
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    return trayAdded_;
}

void MainWindow::removeTray() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
}

void MainWindow::showWindowAgain() {
    ShowWindow(hwnd_, SW_SHOW);
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    if (!config_.settings.startMinimized) removeTray();
}

void MainWindow::trayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, L"Open Dental Macros");
    AppendMenuW(m, MF_STRING, IDM_TRAY_TOGGLE,
                engine_.running() ? L"Stop listening" : L"Start listening");
    AppendMenuW(m, MF_STRING | (mini_.visible() ? MF_CHECKED : 0), IDM_TRAY_MINI,
                L"Show floating buttons");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"Exit Dental Macros");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd_);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y,
                                  0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd == IDM_TRAY_SHOW) showWindowAgain();
    else if (cmd == IDM_TRAY_MINI) toggleMiniDeck();
    else if (cmd == IDM_TRAY_TOGGLE) toggleEngine();
    else if (cmd == IDM_TRAY_EXIT) { quitting_ = true; DestroyWindow(hwnd_); }
}

bool MainWindow::confirmExit() {
    if (config_.settings.minimizeToTray && !quitting_) {
        // Only hide if there is actually an icon to bring it back with.
        // Hiding without one leaves a running program the user can reach
        // solely through Task Manager.
        if (!addTray()) {
            showMessage(hwnd_, "No icon in the notification area",
                        "Windows would not give Dental Macros an icon in the "
                        "notification area, so closing the window would leave "
                        "it running with no way back. Dental Macros will close instead.",
                        true);
            return true;
        }
        ShowWindow(hwnd_, SW_HIDE);
        return false;
    }
    return true;
}

// ------------------------------------------------------------- window proc
LRESULT CALLBACK MainWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp,
                                     LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        self = (MainWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = hwnd;
    } else {
        self = (MainWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CREATE:
            self->onCreate();
            return 0;

        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) self->layout();
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = (MINMAXINFO*)lp;
            // All three columns grew with the design system's sizes.
            mmi->ptMinTrackSize.x = S(1080);
            mmi->ptMinTrackSize.y = S(700);
            return 0;
        }

        case WM_DPICHANGED:
            self->onDpiChanged(HIWORD(wp), (const RECT*)lp);
            return 0;

        case WM_HOTKEY:
            if ((int)wp == kToggleHotkeyId) {
                self->toggleEngine();
                self->pushLog(
                    self->engine_.running()
                        ? "Started listening from the keyboard shortcut."
                        : "Stopped listening from the keyboard shortcut.");
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            self->paint(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DRAWITEM:
            if (handleDrawItem(hwnd, (DRAWITEMSTRUCT*)lp)) return TRUE;
            break;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            LRESULT out = 0;
            if (handleCtlColor(msg, wp, lp, out)) return out;
            break;
        }

        case WM_APP_LOG:
            self->drainLogs();
            return 0;

        case WM_APP_KEYEVENT:
            self->drainKeyEvents();
            return 0;

        case WM_APP_PAGE:
            self->drainPageRequests();
            return 0;

        case WM_APP_OUTCOME:
            self->drainOutcomes();
            return 0;

        case WM_APP_LOADPROBLEM:
            self->explainLoadProblem();
            return 0;

        case WM_APP_SETUP:
            self->runSetup();
            return 0;

        case WM_APP_FOREGROUND:
            self->onForegroundChanged((HWND)wp);
            return 0;

        case WM_DEVICECHANGE:
            // Sent to every top-level window when anything is plugged in,
            // removed, paired or drops off Bluetooth. Which device is not
            // said, so look again once the burst is over.
            if (wp == DBT_DEVNODES_CHANGED)
                SetTimer(hwnd, kDeviceCheckTimer, kDeviceCheckDelayMs, nullptr);
            return TRUE;

        case WM_TIMER:
            if (wp == kToastTimer) {
                KillTimer(hwnd, kToastTimer);
                self->toastText_.clear();
                InvalidateRect(hwnd, &self->rcDeck_, FALSE);
                return 0;
            }
            if (wp == kDeviceCheckTimer) {
                KillTimer(hwnd, kDeviceCheckTimer);
                self->checkDevices(true);
                return 0;
            }
            if (wp == 2) {
                // fade out the "just fired" glow, then stop ticking
                DWORD now = GetTickCount();
                bool any = false;
                for (auto& kv : self->lastFire_)
                    if (now - kv.second < 500) any = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                if (!any) KillTimer(hwnd, 2);
            }
            return 0;

        case WM_APP_TRAY:
            if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP)
                self->showWindowAgain();
            else if (LOWORD(lp) == WM_RBUTTONUP)
                self->trayMenu();
            return 0;

        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &p);
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (ptIn(self->rcSidebar_, p)) {
                self->paletteScroll_ =
                    std::max(0, self->paletteScroll_ - delta / 2);
                int sidebarH =
                    (int)(self->rcSidebar_.bottom - self->rcSidebar_.top);
                int maxScroll =
                    std::max(0, self->paletteContentH_ - sidebarH / 2);
                self->paletteScroll_ = std::min(self->paletteScroll_, maxScroll);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (ptIn(self->rcDeck_, p) &&
                       self->tileUnderCursorIsDial(p, delta > 0)) {
                // Handled: a dial swallows the notch rather than also
                // scrolling the deck out from under the tile being turned.
            } else if (ptIn(self->rcDeck_, p) &&
                       self->deckContentH_ > self->deckViewH_) {
                self->deckScroll_ = std::max(
                    0, std::min(self->deckScroll_ - delta / 2,
                                self->deckContentH_ - self->deckViewH_));
                self->layoutDeck();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            self->dragPos_ = p;

            if (self->maybeDrag_ && !self->dragging_) {
                if (std::abs(p.x - self->dragStart_.x) > S(5) ||
                    std::abs(p.y - self->dragStart_.y) > S(5)) {
                    self->dragging_ = true;
                    SetCapture(hwnd);
                }
            }

            int oldTile = self->hoverTile_, oldPal = self->hoverPalette_,
                oldKb = self->hoverKb_, oldPage = self->hoverPageTab_;
            self->hoverTile_ = -1;
            self->hoverPalette_ = -1;
            self->hoverKb_ = -1;
            self->hoverPageTab_ = -1;
            if (!self->dragging_) {
                for (size_t i = 0; i < self->pageTabs_.size(); ++i)
                    if (ptIn(self->pageTabs_[i].rect, p))
                        self->hoverPageTab_ = (int)i;
                if (ptIn(self->rcPagePin_, p)) self->hoverPageTab_ = -2;
            }
            for (size_t i = 0; i < self->tiles_.size(); ++i)
                if (ptIn(self->tiles_[i].rect, p)) self->hoverTile_ = (int)i;
            if (!self->dragging_) {
                for (size_t i = 0; i < self->palette_.size(); ++i)
                    if (ptIn(self->palette_[i].rect, p) &&
                        ptIn(self->rcSidebar_, p))
                        self->hoverPalette_ = (int)i;
                for (const auto& r : self->kbRows_)
                    if (ptIn(r.rect, p))
                        self->hoverKb_ =
                            r.index >= 0 ? r.index : r.index - 1;
            }
            const bool oldDismiss = self->hoverDismiss_;
            self->hoverDismiss_ = ptIn(self->rcBannerDismiss_, p);
            // The welcome card's button is painted, so its hover is too.
            static bool overWelcome = false;
            const bool welcome = ptIn(self->rcWelcomeBtn_, p);
            if (self->dragging_ || oldTile != self->hoverTile_ ||
                oldPal != self->hoverPalette_ || oldKb != self->hoverKb_ ||
                oldPage != self->hoverPageTab_ ||
                oldDismiss != self->hoverDismiss_ || welcome != overWelcome)
                InvalidateRect(hwnd, nullptr, FALSE);
            overWelcome = welcome;

            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            self->hoverTile_ = self->hoverPalette_ = -1;
            self->hoverKb_ = -1;
            self->hoverPageTab_ = -1;
            self->hoverDismiss_ = false;
            self->dragPos_ = POINT{-1, -1};
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            SetFocus(hwnd);

            if (ptIn(self->rcBannerDismiss_, p)) {
                self->bannerDismissed_ = true;
                self->hoverDismiss_ = false;
                self->layout();
                return 0;
            }
            if (self->config_.keyboards.empty()) {
                if (ptIn(self->rcWelcomeBtn_, p)) self->runSetup();
                return 0;
            }
            for (const auto& r : self->kbRows_) {
                if (!ptIn(r.rect, p)) continue;
                if (r.index == -1) self->addKeyboard();
                else if (r.index == -3) self->addMouse();
                else if (r.index == -2) self->addSoftwareDeck();
                else self->selectKeyboard(
                    self->config_.keyboards[(size_t)r.index].alias);
                return 0;
            }
            if (ptIn(self->rcPagePin_, p)) {
                bool& follow = self->config_.settings.switchPageWithApp;
                follow = !follow;
                self->saveConfig();
                self->engine_.updateConfig(self->config_);
                if (follow) {
                    // Catch up straight away rather than waiting for the next
                    // time the user happens to change window.
                    self->lastForeground_.clear();
                    self->onForegroundChanged(GetForegroundWindow());
                }
                self->pushLog(follow ? "Pages follow the application in front."
                                     : "Pages stay where you put them.");
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            for (const PageTab& t : self->pageTabs_) {
                if (!ptIn(t.rect, p)) continue;
                if (t.index < 0) self->addPage();
                else if (cfg::Keyboard* kb = self->currentKb())
                    self->setActivePage(kb->alias, t.index);
                return 0;
            }
            for (size_t i = 0; i < self->palette_.size(); ++i) {
                if (ptIn(self->palette_[i].rect, p) &&
                    ptIn(self->rcSidebar_, p)) {
                    self->maybeDrag_ = true;
                    self->dragAction_ = self->palette_[i].actionId;
                    self->dragStart_ = p;
                    return 0;
                }
            }
            for (size_t i = 0; i < self->tiles_.size(); ++i)
                if (ptIn(self->tiles_[i].rect, p)) {
                    self->activateTile((int)i);
                    return 0;
                }
            return 0;
        }

        case WM_LBUTTONUP: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (self->dragging_) {
                ReleaseCapture();
                self->dragging_ = false;
                self->maybeDrag_ = false;
                std::string action = self->dragAction_;
                self->dragAction_.clear();
                for (size_t i = 0; i < self->tiles_.size(); ++i)
                    if (ptIn(self->tiles_[i].rect, p)) {
                        if (self->tiles_[i].binding < 0)
                            self->assignKeyToTile((int)i);
                        else
                            self->selectTile((int)i);
                        if (self->selectedBinding()) self->addStep(action);
                        break;
                    }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            self->maybeDrag_ = false;
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            // Double-click renames a page, the same as it renames a tile.
            for (const PageTab& t : self->pageTabs_)
                if (ptIn(t.rect, p) && t.index >= 0) {
                    if (cfg::Keyboard* kb = self->currentKb()) {
                        std::string name = kb->pages[(size_t)t.index].name;
                        if (askText(hwnd, "Rename page",
                                    "What should this page be called?", name,
                                    "", "Save name")) {
                            const std::string want = trim(name);
                            kb->pages[(size_t)t.index].name =
                                (kb->pageByName(want) == t.index)
                                    ? want
                                    : kb->uniquePageName(want);
                            self->modelChanged();
                        }
                    }
                    return 0;
                }
            for (size_t i = 0; i < self->tiles_.size(); ++i)
                if (ptIn(self->tiles_[i].rect, p) &&
                    self->tiles_[i].binding >= 0) {
                    self->selectTile((int)i);
                    SetFocus(self->titleEdit_);
                    SendMessageW(self->titleEdit_, EM_SETSEL, 0, -1);
                    return 0;
                }
            return 0;
        }

        case WM_RBUTTONUP: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            POINT screen = p;
            ClientToScreen(hwnd, &screen);
            for (const auto& r : self->kbRows_)
                if (ptIn(r.rect, p) && r.index >= 0) {
                    self->keyboardContextMenu(r.index, screen);
                    return 0;
                }
            for (const PageTab& t : self->pageTabs_)
                if (ptIn(t.rect, p) && t.index >= 0 && !self->simple()) {
                    self->pageContextMenu(t.index, screen);
                    return 0;
                }
            for (size_t i = 0; i < self->tiles_.size(); ++i)
                if (ptIn(self->tiles_[i].rect, p)) {
                    self->tileContextMenu((int)i, screen);
                    return 0;
                }
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wp), notify = HIWORD(wp);
            if (id >= ID_SWATCH_FIRST && id <= ID_SWATCH_LAST) {
                if (notify == BN_CLICKED) self->setKeyColor(id - ID_SWATCH_FIRST);
                return 0;
            }
            switch (id) {
                case ID_GESTURE_PRESS:
                    if (notify == BN_CLICKED) self->setGesture(0);
                    return 0;
                case ID_GESTURE_UP:
                    if (notify == BN_CLICKED) self->setGesture(1);
                    return 0;
                case ID_GESTURE_DOWN:
                    if (notify == BN_CLICKED) self->setGesture(2);
                    return 0;
                case ID_BTN_ENGINE:
                    if (notify == BN_CLICKED) self->toggleEngine();
                    return 0;
                case ID_BTN_SETTINGS:
                    if (notify == BN_CLICKED) self->openSettings();
                    return 0;
                case ID_BTN_FLOATING:
                    if (notify == BN_CLICKED) self->toggleMiniDeck();
                    return 0;
                case ID_BTN_SIMPLE:
                    if (notify == BN_CLICKED) self->setSimpleMode(true);
                    return 0;
                case ID_BTN_ADVANCED:
                    if (notify == BN_CLICKED) self->setSimpleMode(false);
                    return 0;
                case ID_BTN_S_TYPE:
                    if (notify == BN_CLICKED) self->addSimpleStep(0);
                    return 0;
                case ID_BTN_S_KEYS:
                    if (notify == BN_CLICKED) self->addSimpleStep(1);
                    return 0;
                case ID_BTN_S_WAIT:
                    if (notify == BN_CLICKED) self->addSimpleStep(2);
                    return 0;
                case ID_BTN_ADD_STEP:
                    if (notify == BN_CLICKED) self->addStep();
                    return 0;
                case ID_BTN_EDIT_STEP:
                    if (notify == BN_CLICKED) self->editStep();
                    return 0;
                case ID_BTN_DEL_STEP:
                    if (notify == BN_CLICKED) self->removeStep();
                    return 0;
                case ID_BTN_UP:
                    if (notify == BN_CLICKED) self->moveStep(-1);
                    return 0;
                case ID_BTN_DOWN:
                    if (notify == BN_CLICKED) self->moveStep(1);
                    return 0;
                case ID_BTN_TEST:
                    if (notify == BN_CLICKED) self->testRun();
                    return 0;
                case ID_BTN_RECORD:
                    if (notify == BN_CLICKED) self->recordClicks();
                    return 0;
                case ID_BTN_ICON:
                    if (notify == BN_CLICKED) self->chooseTileIcon();
                    return 0;
                case ID_BTN_COLOR:
                    if (notify == BN_CLICKED) self->chooseTileColor();
                    return 0;
                case ID_STEP_LIST:
                    // Only the buttons need updating. Rebuilding the whole
                    // list on a click is what made the selection bounce
                    // straight back off again.
                    if (notify == LBN_SELCHANGE) self->updateStepButtons();
                    if (notify == LBN_DBLCLK) self->editStep();
                    return 0;
                case ID_TITLE_EDIT:
                    if (notify == EN_CHANGE) {
                        if (cfg::Binding* b = self->selectedBinding()) {
                            b->title = getText(self->titleEdit_);
                            self->config_.dirty = true;
                            InvalidateRect(hwnd, &self->rcDeck_, FALSE);
                        }
                    }
                    if (notify == EN_KILLFOCUS) self->saveConfig();
                    return 0;
                case ID_DECK_COMBO:
                    if (notify == CBN_SELCHANGE) {
                        int sel = (int)SendMessageW(self->deckCombo_,
                                                    CB_GETCURSEL, 0, 0);
                        if (cfg::Keyboard* kb = self->currentKb()) {
                            if (sel >= 0 && sel < (int)self->deckIds_.size()) {
                                kb->deck = self->deckIds_[(size_t)sel];
                                self->selectedTile_ = -1;
                                self->modelChanged();
                            }
                        }
                    }
                    return 0;
            }
            break;
        }

        case WM_CLOSE:
            if (!self->confirmExit()) return 0;
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            self->onDestroy();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

// ------------------------------------------------------------------ runApp
int runApp(HINSTANCE inst, int showCmd) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    MainWindow win;
    if (!win.create(inst, showCmd)) {
        MessageBoxW(nullptr, L"Dental Macros could not create its window.",
                    L"Dental Macros", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Without this the main window's controls cannot be reached from the
        // keyboard at all - Tab does nothing and the focus ring never shows.
        // Modal dialogs run their own loop and do this for themselves.
        if (IsDialogMessageW(win.hwnd(), &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

}  // namespace ui
