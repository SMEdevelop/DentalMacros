#include "app.h"

#include "config.h"
#include "device.h"
#include "dialogs.h"
#include "engine.h"
#include "gdiplus_inc.h"
#include "iconlib.h"
#include "keys.h"
#include "minideck.h"
#include "theme.h"
#include "ui_common.h"
#include "util.h"
#include "vision.h"
#include "winman.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

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
    ID_BTN_MENU,
    ID_BTN_SETTINGS,
    ID_DECK_COMBO,
    // The three gestures a tile can carry. One list is shown at a time and
    // every step button below acts on whichever is selected.
    ID_GESTURE_PRESS,
    ID_GESTURE_UP,
    ID_GESTURE_DOWN,

    IDM_IMPORT = 400,
    IDM_SAVE,
    IDM_RELOAD,
    IDM_EXPORT,
    IDM_BACKUP,
    IDM_SNIP,
    IDM_LAYOUTS,
    IDM_OPEN_FOLDER,
    IDM_ABOUT,
    IDM_EXIT,

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
    // The running windows offered as an application rule. One id per window,
    // so the range has to be wide enough for a busy desktop.
    IDM_PAGE_APP_PICK = 1200,

    IDM_TRAY_SHOW = 480,
    IDM_TRAY_TOGGLE,
    IDM_TRAY_EXIT,
    IDM_TRAY_MINI,

    IDM_MINIDECK = 490,
};

// The system-wide start/stop shortcut. Any id unique within this window will
// do; RegisterHotKey ids are per-window.
constexpr int kToggleHotkeyId = 1;

// Design-pixel metrics.
constexpr int kToolbarH = 60;
constexpr int kSidebarW = 214;
constexpr int kInspectorW = 344;
constexpr int kLogH = 132;
constexpr int kPad = 14;
constexpr int kRowH = 30;

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
        std::string full = joinPath(exeDir(), relPath);
        if (!fileExists(full)) full = relPath;
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
    void recordClicks();
    void testRun();
    void clearTile();
    void deleteBinding();
    void chooseTileIcon();
    void chooseTileColor();
    void toggleEngine();
    void showMenu();
    void importYaml();
    void showLayouts();
    void reloadConfig();
    void exportConfig();
    void backupNow();
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
    void refreshMiniDeck();
    // Fire a binding by key name, for a click on the mini deck.
    void runBindingByKey(const std::string& key);
    void saveMiniPlacement();

    // ---- tray
    bool addTray();          // false if Windows refused the icon
    void removeTray();
    void trayMenu();
    void showWindowAgain();

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
    HWND btnEngine_ = nullptr, btnMenu_ = nullptr, btnSettings_ = nullptr;
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
    wc.lpszClassName = L"MacroDeckMain";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = icon_;
    wc.hIconSm = iconSm_;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    // Centre on the primary monitor's work area, and never larger than it -
    // the default size is bigger than a small laptop screen at 150% scaling.
    RECT work = workAreaNear(nullptr);
    int w = std::min<int>(S(1240), work.right - work.left);
    int h = std::min<int>(S(830), work.bottom - work.top);
    int sx = work.left + ((work.right - work.left) - w) / 2;
    int sy = work.top + ((work.bottom - work.top) - h) / 2;

    // WS_EX_CONTROLPARENT so IsDialogMessage in the main loop will walk into
    // the child controls when the user presses Tab.
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, L"MacroDeckMain", L"MacroDeck",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, sx, sy, w, h,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    applyDarkFrame(hwnd_);

    // Start hidden only if there is actually a tray icon to come back from.
    bool startHidden = config_.settings.startMinimized;
    if (startHidden && !addTray()) {
        startHidden = false;
        pushLog("Windows would not provide a notification-area icon, so "
                "MacroDeck opened its window instead of starting minimised.");
    }
    ShowWindow(hwnd_, startHidden ? SW_HIDE : showCmd);
    UpdateWindow(hwnd_);
    return true;
}

void MainWindow::onCreate() {
    // DPI: everything downstream scales off this.
    UINT dpi = GetDpiForWindow(hwnd_);
    if (dpi >= 72 && dpi <= 480) setDpiScale(dpi / 96.0f);

    config_.load(appPath("macros.json"));
    countPointers();

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
    engine_.updateConfig(config_);

    // ---- toolbar controls
    btnEngine_ = makeButton(hwnd_, "Start Listening", 0, 0, 132, 34,
                            ID_BTN_ENGINE);
    setTone(btnEngine_, Tone::Good);
    btnSettings_ = makeButton(hwnd_, "Settings", 0, 0, 92, 34, ID_BTN_SETTINGS);
    btnMenu_ = makeButton(hwnd_, "More", 0, 0, 76, 34, ID_BTN_MENU);

    // ---- deck controls
    std::vector<std::string> deckLabels;
    for (const auto& p : cfg::deckPresets()) {
        deckIds_.push_back(p.id);
        deckLabels.push_back(p.label);
    }
    deckCombo_ = makeCombo(hwnd_, 0, 0, 220, 28, ID_DECK_COMBO, deckLabels, 0);

    // ---- inspector controls
    titleEdit_ = makeEdit(hwnd_, "", 0, 0, 100, 26, ID_TITLE_EDIT);
    btnIcon_ = makeButton(hwnd_, "Icon...", 0, 0, 90, 28, ID_BTN_ICON);
    btnColor_ = makeButton(hwnd_, "Colour...", 0, 0, 90, 28, ID_BTN_COLOR);
    btnGesturePress_ = makeButton(hwnd_, "Press", 0, 0, 76, 26,
                                  ID_GESTURE_PRESS);
    btnGestureUp_ = makeButton(hwnd_, "Wheel \u2191", 0, 0, 76, 26,
                               ID_GESTURE_UP);
    btnGestureDown_ = makeButton(hwnd_, "Wheel \u2193", 0, 0, 76, 26,
                                 ID_GESTURE_DOWN);
    stepList_ = makeList(hwnd_, 0, 0, 100, 100, ID_STEP_LIST);
    btnAddStep_ = makeButton(hwnd_, "Add step", 0, 0, 90, 30, ID_BTN_ADD_STEP,
                             true);
    btnEditStep_ = makeButton(hwnd_, "Edit", 0, 0, 68, 30, ID_BTN_EDIT_STEP);
    btnDelStep_ = makeButton(hwnd_, "Remove", 0, 0, 78, 30, ID_BTN_DEL_STEP);
    btnUp_ = makeButton(hwnd_, "Up", 0, 0, 52, 30, ID_BTN_UP);
    btnDown_ = makeButton(hwnd_, "Down", 0, 0, 62, 30, ID_BTN_DOWN);
    btnTest_ = makeButton(hwnd_, "Test run", 0, 0, 100, 30, ID_BTN_TEST);
    btnRecord_ = makeButton(hwnd_, "Record clicks...", 0, 0, 100, 30,
                            ID_BTN_RECORD);

    // ---- log
    logEdit_ = makeEdit(hwnd_, "", 0, 0, 100, 100, ID_LOG_EDIT, true, true);

    if (!config_.keyboards.empty()) selectKeyboard(config_.keyboards[0].alias);

    reportLoadProblems();
    applyToggleHotkey();
    startForegroundHook();
    // Start on the page the application already in front asks for, rather
    // than waiting for the user to alt-tab once before the rules mean
    // anything.
    onForegroundChanged(GetForegroundWindow());

    layout();
    refreshInspector();

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

    if (config_.settings.startEngineOnLaunch && !config_.keyboards.empty())
        toggleEngine();
}

void MainWindow::reportLoadProblems() {
    for (const std::string& w : config_.loadWarnings)
        pushLog("macros.json: " + w);

    if (config_.loadError.empty()) {
        pushLog(config_.keyboards.empty() ? "MacroDeck ready."
                                          : "Loaded macros.json.");
        return;
    }

    pushLog("Could not read macros.json: " + config_.loadError);
    showMessage(hwnd_, "Configuration problem",
                "macros.json could not be read:\n\n" + config_.loadError +
                    "\n\nMacroDeck opened with an empty setup and has not "
                    "touched your file. Fix or move it and choose "
                    "More > Reload macros.json.\n\nIf you save anything before "
                    "then, the file you have now is copied to macros.json.bak "
                    "first - nothing is thrown away.",
                true);
}

void MainWindow::onDestroy() {
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
void MainWindow::layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int W_ = rc.right, H_ = rc.bottom;

    rcToolbar_ = {0, 0, W_, S(kToolbarH)};
    rcLog_ = {0, H_ - S(kLogH), W_, H_};
    rcSidebar_ = {0, S(kToolbarH), S(kSidebarW), rcLog_.top};
    rcInspector_ = {W_ - S(kInspectorW), S(kToolbarH), W_, rcLog_.top};
    rcDeck_ = {rcSidebar_.right, S(kToolbarH), rcInspector_.left, rcLog_.top};

    // toolbar buttons, right-aligned
    int by = (S(kToolbarH) - S(34)) / 2;
    int x = W_ - S(kPad) - S(76);
    MoveWindow(btnMenu_, x, by, S(76), S(34), TRUE);
    x -= S(8) + S(92);
    MoveWindow(btnSettings_, x, by, S(92), S(34), TRUE);
    layoutEngineButton();

    // Deck header. A combo box's window height controls how far its list
    // drops down, not the height of the closed control - sizing it to 28px
    // would leave the dropdown unusable.
    MoveWindow(deckCombo_, rcDeck_.left + S(kPad), rcDeck_.top + S(14), S(240),
               S(28) * 9, TRUE);

    // inspector
    const int ix = rcInspector_.left + S(kPad);
    const int iw = S(kInspectorW) - S(kPad) * 2;
    int y = rcInspector_.top + S(58);
    MoveWindow(titleEdit_, ix, y, iw, S(26), TRUE);
    y += S(34);
    MoveWindow(btnIcon_, ix, y, S(100), S(28), TRUE);
    MoveWindow(btnColor_, ix + S(108), y, S(100), S(28), TRUE);
    y += S(50);

    MoveWindow(btnGesturePress_, ix, y, S(76), S(26), TRUE);
    MoveWindow(btnGestureUp_, ix + S(80), y, S(76), S(26), TRUE);
    MoveWindow(btnGestureDown_, ix + S(160), y, S(76), S(26), TRUE);
    y += S(32);

    int listBottom = rcInspector_.bottom - S(kPad) - S(34) * 3 - S(8);
    MoveWindow(stepList_, ix, y, iw, std::max(S(60), listBottom - y), TRUE);
    int by2 = listBottom + S(8);
    MoveWindow(btnAddStep_, ix, by2, S(94), S(30), TRUE);
    MoveWindow(btnEditStep_, ix + S(100), by2, S(64), S(30), TRUE);
    MoveWindow(btnDelStep_, ix + S(170), by2, S(78), S(30), TRUE);
    int by3 = by2 + S(36);
    MoveWindow(btnUp_, ix, by3, S(56), S(30), TRUE);
    MoveWindow(btnDown_, ix + S(62), by3, S(66), S(30), TRUE);
    MoveWindow(btnTest_, ix + iw - S(104), by3, S(104), S(30), TRUE);
    int by4 = by3 + S(36);
    MoveWindow(btnRecord_, ix, by4, iw, S(30), TRUE);

    // log
    MoveWindow(logEdit_, S(kPad), rcLog_.top + S(26), W_ - S(kPad) * 2,
               S(kLogH) - S(26) - S(10), TRUE);

    layoutDeck();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::string MainWindow::engineButtonLabel() const {
    if (!engine_.running()) return "Start Listening";
    // Show the way out only while there is something to escape from, and only
    // when the shortcut really is ours - if another program claimed it,
    // printing it here would be a lie.
    if (hotkeyRegistered_)
        return "Stop Listening  (" +
               keys::prettyCombo(config_.settings.toggleHotkey) + ")";
    return "Stop Listening";
}

void MainWindow::layoutEngineButton() {
    if (!btnEngine_ || !hwnd_) return;
    const std::string label = engineButtonLabel();
    setText(btnEngine_, label);

    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int by = (S(kToolbarH) - S(34)) / 2;
    const int w = std::max(S(132), textWidthOf(btnEngine_, label) + S(28));
    // sits to the left of Settings and More
    const int x = rc.right - S(kPad) - S(76) - S(8) - S(92) - S(14) - w;
    MoveWindow(btnEngine_, x, by, w, S(34), TRUE);
}

std::string MainWindow::listeningSummary() const {
    if (config_.keyboards.empty()) return "Listening";
    if (config_.keyboards.size() == 1) {
        const cfg::Keyboard& k = config_.keyboards[0];
        if (!k.block)
            return "Listening to " + k.alias +
                   " - its keys run macros and still type";
        if (!k.blockAllKeys)
            return "Listening to " + k.alias +
                   " - only its macro keys stop typing";
        return "Listening to " + k.alias +
               " - its keys run macros and do not type";
    }
    int silent = 0;   // every key given up
    int partial = 0;  // only the macro keys given up
    for (const auto& k : config_.keyboards) {
        if (!k.block) continue;
        if (k.blockAllKeys) ++silent; else ++partial;
    }
    std::string s = "Listening to " + std::to_string(config_.keyboards.size()) +
                    " keyboards";
    if (silent) s += " - " + std::to_string(silent) + " of them do not type";
    if (partial)
        s += (silent ? ", " : " - ") + std::to_string(partial) +
             " only on macro keys";
    return s;
}

// How far below the top of the deck area the tiles begin. The deck preset
// combo sits in the first band and the page strip in the second, and both the
// tile layout and the scroll clip have to agree about where that ends.
constexpr int kDeckHeaderH = 90;
constexpr int kPageStripY = 50;
constexpr int kPageStripH = 26;

void MainWindow::layoutPages() {
    pageTabs_.clear();
    rcPagePin_ = RECT{};

    cfg::Keyboard* kb = currentKb();
    if (!kb) return;
    kb->normalizePages();

    const int y = rcDeck_.top + S(kPageStripY);
    const int h = S(kPageStripH);
    // The padlock is pinned to the right end, and the tabs stop short of it
    // rather than sliding underneath.
    const int pinW = S(30);
    rcPagePin_ = {rcDeck_.right - S(kPad) - pinW, y, rcDeck_.right - S(kPad),
                  y + h};
    const int limit = rcPagePin_.left - S(8);

    HDC dc = GetDC(hwnd_);
    Graphics g(dc);
    int x = rcDeck_.left + S(kPad);
    auto place = [&](int index, const std::wstring& label) {
        const int w = (int)measureText(g, label, 11, false).Width + S(26);
        if (x + w > limit && !pageTabs_.empty()) return false;
        PageTab t;
        t.index = index;
        t.rect = {x, y, x + w, y + h};
        pageTabs_.push_back(t);
        x += w + S(6);
        return true;
    };
    for (size_t i = 0; i < kb->pages.size(); ++i)
        if (!place((int)i, W(kb->pages[i].displayName()))) break;
    place(-1, L"+");
    ReleaseDC(hwnd_, dc);
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
    area.top += S(kDeckHeaderH);
    area.bottom -= S(kPad);
    const int availW = area.right - area.left;
    const int availH = area.bottom - area.top;
    if (availW < S(80) || availH < S(80)) return;

    const int gap = S(12);
    deckViewH_ = availH;

    if (preset->flow) {
        // Flow layout: every binding in order, plus a trailing "add" tile.
        int count = (int)kb->active().bindings.size() + 1;
        int cols = std::max(1, std::min(count, (availW + gap) / (S(120) + gap)));
        int rows = (count + cols - 1) / cols;
        // Size tiles from the width alone and scroll if the rows overflow.
        // Squeezing them to fit the height instead is what made a deck with
        // many keys unusable - past about thirty they either vanished or ran
        // off the bottom into the log.
        int byWidth = (availW - gap * (cols - 1)) / cols;
        int side = std::max(S(56), std::min(byWidth, S(150)));
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

    // Fixed presets are sized to fit, so they never scroll.
    deckScroll_ = 0;

    // Fixed grid: size tiles so the whole preset fits the available area.
    int cols = preset->cols, rows = preset->rows;
    int tileW = (availW - gap * (cols - 1)) / cols;
    int tileH = (availH - gap * (rows - 1)) / rows;
    int side = std::max(S(56), std::min({tileW, tileH, S(150)}));
    int gridW = side * cols + gap * (cols - 1);
    int gridH = side * rows + gap * (rows - 1);
    int ox = area.left + std::max(0, (availW - gridW) / 2);
    int oy = area.top;

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
    (void)gridH;
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
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        SolidBrush bg(theme().bg);
        g.FillRectangle(&bg, 0, 0, W_, H_);

        paintToolbar(g);
        paintSidebar(g);
        if (config_.keyboards.empty())
            paintWelcome(g);
        else
            paintDeck(g);

        // inspector + log backgrounds
        SolidBrush panel(theme().panel);
        g.FillRectangle(&panel, toRectF(rcInspector_));
        g.FillRectangle(&panel, toRectF(rcLog_));
        Pen line(theme().borderSoft, 1.0f);
        g.DrawLine(&line, (REAL)rcInspector_.left, (REAL)rcInspector_.top,
                   (REAL)rcInspector_.left, (REAL)rcInspector_.bottom);
        g.DrawLine(&line, 0.0f, (REAL)rcLog_.top, (REAL)W_, (REAL)rcLog_.top);

        // inspector heading
        cfg::Binding* b = selectedBinding();
        RectF head((REAL)(rcInspector_.left + S(kPad)),
                   (REAL)(rcInspector_.top + S(14)), (REAL)S(kInspectorW - kPad * 2),
                   (REAL)S(22));
        if (b)
            drawText(g, W("Key: " + keys::displayName(b->key)), head, 15,
                     theme().text, Align::Left, true);
        else
            drawText(g, L"Nothing selected", head, 15, theme().textDim,
                     Align::Left, true);

        RectF sub((REAL)(rcInspector_.left + S(kPad)),
                  (REAL)(rcInspector_.top + S(38)),
                  (REAL)S(kInspectorW - kPad * 2), (REAL)S(18));
        drawText(g, b ? L"Label shown on the key" : L"Pick a key on the deck",
                 sub, 12, theme().textFaint, Align::Left);

        // log heading
        RectF logHead((REAL)S(kPad), (REAL)(rcLog_.top + S(6)), (REAL)S(300),
                      (REAL)S(18));
        drawText(g, L"Activity", logHead, 12, theme().textDim, Align::Left, true);

        if (dragging_) paintDragGhost(g);
    }

    BitBlt(hdc, 0, 0, W_, H_, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void MainWindow::paintToolbar(Graphics& g) {
    SolidBrush panel(theme().panel);
    g.FillRectangle(&panel, toRectF(rcToolbar_));
    Pen line(theme().borderSoft, 1.0f);
    g.DrawLine(&line, 0.0f, (REAL)rcToolbar_.bottom, (REAL)rcToolbar_.right,
               (REAL)rcToolbar_.bottom);

    // logo mark
    RectF mark((REAL)S(kPad), (REAL)S(14), (REAL)S(32), (REAL)S(32));
    fillRounded(g, mark, S(8.0f), theme().accent);
    RectF inner(mark.X + S(8.0f), mark.Y + S(8.0f), S(16.0f), S(16.0f));
    SolidBrush white(Color(255, 255, 255, 255));
    for (int i = 0; i < 4; ++i) {
        REAL cw = S(6.0f), gp = S(4.0f);
        g.FillRectangle(&white, inner.X + (i % 2) * (cw + gp),
                        inner.Y + (i / 2) * (cw + gp), cw, cw);
    }

    RectF title((REAL)S(kPad + 44), (REAL)S(12), (REAL)S(260), (REAL)S(20));
    drawText(g, L"MacroDeck", title, 16, theme().text, Align::Left, true);

    // Stop the status line before the buttons rather than at a fixed width -
    // the listen button changes size with its label.
    int statusRight = rcToolbar_.right - S(kPad);
    if (btnEngine_) {
        RECT eb{};
        GetWindowRect(btnEngine_, &eb);
        MapWindowPoints(nullptr, hwnd_, (POINT*)&eb, 2);
        statusRight = eb.left - S(12);
    }
    const int statusLeft = S(kPad + 44);
    RectF status((REAL)statusLeft, (REAL)S(32),
                 (REAL)std::max(S(120), statusRight - statusLeft), (REAL)S(18));

    std::wstring statusText;
    Color statusColor = theme().textDim;
    if (engine_.running()) {
        statusText = W(listeningSummary());
        statusColor = theme().good;
        if (!lastKeyText_.empty())
            statusText += L"   \x2022   " + W(lastKeyText_);
    } else if (config_.keyboards.empty()) {
        statusText = L"No macro keyboard set up yet";
    } else {
        statusText = L"Not listening - your macro keys type normally";
    }
    drawText(g, statusText, status, 12, statusColor, Align::Left);
}

void MainWindow::paintSidebar(Graphics& g) {
    SolidBrush panel(theme().panel);
    g.FillRectangle(&panel, toRectF(rcSidebar_));
    Pen line(theme().borderSoft, 1.0f);
    g.DrawLine(&line, (REAL)rcSidebar_.right, (REAL)rcSidebar_.top,
               (REAL)rcSidebar_.right, (REAL)rcSidebar_.bottom);

    kbRows_.clear();
    palette_.clear();

    int x = rcSidebar_.left + S(kPad);
    int w = S(kSidebarW) - S(kPad) * 2;
    int y = rcSidebar_.top + S(14);

    drawText(g, L"KEYBOARDS", RectF((REAL)x, (REAL)y, (REAL)w, (REAL)S(16)), 11,
             theme().textFaint, Align::Left, true);
    y += S(24);

    for (size_t i = 0; i < config_.keyboards.size(); ++i) {
        const cfg::Keyboard& kb = config_.keyboards[i];
        KbRow row;
        row.index = (int)i;
        row.rect = {x, y, x + w, y + S(kRowH)};
        kbRows_.push_back(row);

        bool sel = kb.alias == currentAlias_;
        bool hot = hoverKb_ == (int)i;
        if (sel || hot)
            fillRounded(g, toRectF(row.rect), S(6.0f),
                        sel ? theme().accentDim : theme().panelAlt);
        RectF tr((REAL)(x + S(10)), (REAL)y, (REAL)(w - S(20)), (REAL)S(kRowH));
        drawText(g, W(kb.alias), tr, 13, sel ? theme().text : theme().textDim,
                 Align::Left, sel);
        // No badge is the default, fully-blocking case. "pass" = nothing is
        // taken from this keyboard; "some" = only the keys that run macros.
        const wchar_t* badgeText =
            kb.softwareOnly ? L"click"
            : kb.isMouse && !kb.block ? L"mouse"
            : !kb.block     ? L"pass"
            : !kb.blockAllKeys ? L"some"
                               : nullptr;
        if (badgeText) {
            // Sized to its text rather than to a fixed box: "mouse" is longer
            // than "some", and a clipped badge reads as a rendering bug.
            const REAL bw =
                measureText(g, badgeText, 8).Width + (REAL)S(8);
            RectF badge((REAL)(x + w - S(8)) - bw, (REAL)(y + S(8)), bw,
                        (REAL)S(14));
            fillRounded(g, badge, S(4.0f), theme().panelAlt);
            drawText(g, badgeText, badge, 8, theme().textFaint, Align::Center);
        }
        y += S(kRowH) + S(2);
    }

    {
        KbRow row;
        row.index = -1;
        row.rect = {x, y, x + w, y + S(kRowH)};
        kbRows_.push_back(row);
        bool hot = hoverKb_ == -2;
        if (hot) fillRounded(g, toRectF(row.rect), S(6.0f), theme().panelAlt);
        RectF plus((REAL)(x + S(10)), (REAL)(y + S(10)), (REAL)S(10),
                   (REAL)S(10));
        drawGlyphPlus(g, plus, theme().accent, S(1.6f));
        RectF tr((REAL)(x + S(28)), (REAL)y, (REAL)(w - S(30)), (REAL)S(kRowH));
        drawText(g, L"Add a keyboard", tr, 13, theme().accent, Align::Left);
        y += S(kRowH) + S(2);
    }

    {
        // A spare mouse or a trackpad. Its own row rather than a checkbox in
        // the keyboard scan, because you are looking for a different piece of
        // hardware and the list you need is a different list.
        KbRow row;
        row.index = -3;
        row.rect = {x, y, x + w, y + S(kRowH)};
        kbRows_.push_back(row);
        bool hot = hoverKb_ == -4;
        if (hot) fillRounded(g, toRectF(row.rect), S(6.0f), theme().panelAlt);
        RectF plus((REAL)(x + S(10)), (REAL)(y + S(10)), (REAL)S(10),
                   (REAL)S(10));
        drawGlyphPlus(g, plus, theme().accent, S(1.6f));
        RectF tr((REAL)(x + S(28)), (REAL)y, (REAL)(w - S(30)), (REAL)S(kRowH));
        drawText(g, L"Add a mouse or trackpad", tr, 13, theme().accent,
                 Align::Left);
        y += S(kRowH) + S(2);
    }

    {
        // Buttons with no keyboard behind them. Worth its own row rather than
        // an option inside the keyboard scan, because the whole point is that
        // there is no device to go looking for.
        KbRow row;
        row.index = -2;
        row.rect = {x, y, x + w, y + S(kRowH)};
        kbRows_.push_back(row);
        bool hot = hoverKb_ == -3;
        if (hot) fillRounded(g, toRectF(row.rect), S(6.0f), theme().panelAlt);
        RectF plus((REAL)(x + S(10)), (REAL)(y + S(10)), (REAL)S(10),
                   (REAL)S(10));
        drawGlyphPlus(g, plus, theme().accent, S(1.6f));
        RectF tr((REAL)(x + S(28)), (REAL)y, (REAL)(w - S(30)), (REAL)S(kRowH));
        drawText(g, L"Add software buttons", tr, 13, theme().accent,
                 Align::Left);
        y += S(kRowH) + S(14);
    }

    // ---- action palette
    drawText(g, L"DRAG ONTO A KEY",
             RectF((REAL)x, (REAL)y, (REAL)w, (REAL)S(16)), 11,
             theme().textFaint, Align::Left, true);
    y += S(22);

    const int paletteTop = y;
    const int paletteBottom = rcSidebar_.bottom - S(8);
    int py = y - paletteScroll_;

    Region oldClip;
    g.GetClip(&oldClip);
    g.SetClip(Rect(x, paletteTop, w, std::max(0, paletteBottom - paletteTop)));

    std::string lastCat;
    for (const auto& a : cfg::actionDefs()) {
        if (a.category != lastCat) {
            lastCat = a.category;
            if (py + S(18) > paletteTop - S(20) && py < paletteBottom)
                drawText(g, W(lastCat),
                         RectF((REAL)x, (REAL)py, (REAL)w, (REAL)S(18)), 10,
                         theme().textFaint, Align::Left);
            py += S(20);
        }
        PaletteRow row;
        row.actionId = a.id;
        row.rect = {x, py, x + w, py + S(26)};
        palette_.push_back(row);

        if (py + S(26) > paletteTop && py < paletteBottom) {
            bool hot = hoverPalette_ == (int)palette_.size() - 1;
            if (hot) fillRounded(g, toRectF(row.rect), S(5.0f), theme().panelAlt);
            RectF ic((REAL)(x + S(6)), (REAL)(py + S(5)), (REAL)S(16),
                     (REAL)S(16));
            drawActionIcon(g, a.id, ic, hot ? theme().accent : theme().textDim);
            RectF tr((REAL)(x + S(30)), (REAL)py, (REAL)(w - S(34)),
                     (REAL)S(26));
            drawText(g, W(a.label), tr, 12,
                     hot ? theme().text : theme().textDim, Align::Left);
        }
        py += S(28);
    }
    paletteContentH_ = py + paletteScroll_ - paletteTop;
    g.SetClip(&oldClip);
}

void MainWindow::paintWelcome(Graphics& g) {
    RECT a = rcDeck_;
    int cx = (a.left + a.right) / 2;
    int cy = (a.top + a.bottom) / 2;

    RectF box((REAL)(cx - S(230)), (REAL)(cy - S(120)), (REAL)S(460),
              (REAL)S(240));
    fillRounded(g, box, S(12.0f), theme().panel);
    strokeRounded(g, box, S(12.0f), theme().borderSoft, 1.0f);

    RectF icon((REAL)(cx - S(24)), (REAL)(cy - S(92)), (REAL)S(48), (REAL)S(48));
    drawActionIcon(g, "keys", icon, theme().accent);

    drawText(g, L"Set up your macro keyboard",
             RectF((REAL)(cx - S(210)), (REAL)(cy - S(34)), (REAL)S(420),
                   (REAL)S(26)),
             17, theme().text, Align::Center, true);
    drawText(g,
             L"MacroDeck can tell your keyboards apart, so a spare keyboard or "
             L"numpad can run macros while you keep typing normally on your "
             L"main one.",
             RectF((REAL)(cx - S(200)), (REAL)(cy - S(4)), (REAL)S(400),
                   (REAL)S(56)),
             12, theme().textDim, Align::Center, false, false, true);

    rcWelcomeBtn_ = {cx - S(90), cy + S(60), cx + S(90), cy + S(96)};
    bool hot = ptIn(rcWelcomeBtn_, dragPos_);
    fillRounded(g, toRectF(rcWelcomeBtn_), S(8.0f),
                hot ? mix(theme().accent, Color(255, 255, 255, 255), 0.12f)
                    : theme().accent);
    drawText(g, L"Scan for keyboards", toRectF(rcWelcomeBtn_), 13,
             Color(255, 255, 255, 255), Align::Center, true);
}

void MainWindow::paintDeck(Graphics& g) {
    cfg::Keyboard* kb = currentKb();
    if (!kb) return;

    RectF hint((REAL)(rcDeck_.left + S(kPad) + S(252)), (REAL)(rcDeck_.top + S(14)),
               (REAL)std::max(0, (int)(rcDeck_.right - rcDeck_.left) - S(280)),
               (REAL)S(28));
    drawText(g, L"Click a key to edit it  \x2022  drag an action onto it  \x2022  "
                L"right-click for more",
             hint, 11, theme().textFaint, Align::Left);

    // Tiles can be scrolled, so keep them inside the deck area rather than
    // letting them paint over the header, the sidebar or the log.
    RECT clip = rcDeck_;
    clip.top += S(kDeckHeaderH - 4);
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
            RectF more((REAL)(rcDeck_.right - S(150)),
                       (REAL)(rcDeck_.bottom - S(20)), (REAL)S(136),
                       (REAL)S(16));
            drawText(g, L"scroll for more keys \x2193", more, 10,
                     theme().textFaint, Align::Right);
        }
    }
}

void MainWindow::paintPages(Graphics& g) {
    cfg::Keyboard* kb = currentKb();
    if (!kb || pageTabs_.empty()) return;

    for (size_t i = 0; i < pageTabs_.size(); ++i) {
        const PageTab& t = pageTabs_[i];
        RectF r = toRectF(t.rect);
        const bool hover = hoverPageTab_ == (int)i;
        const bool add = t.index < 0;
        const bool current = !add && t.index == kb->activePage;

        if (add) {
            RectF plus(r.X + r.Width / 2 - S(6.0f),
                       r.Y + r.Height / 2 - S(6.0f), S(12.0f), S(12.0f));
            drawGlyphPlus(g, plus, hover ? theme().accent : theme().textFaint,
                          S(1.6f));
            if (hover)
                strokeRounded(g, r, S(6.0f), theme().accent, 1.0f);
            continue;
        }

        const cfg::Page& page = kb->pages[(size_t)t.index];
        // A page's own colour is what tells you which one you are on at a
        // glance - the same job the RGB does on a hardware pad. It only
        // paints the tab it belongs to, so an unconfigured strip stays quiet.
        Color base = current ? readableTileColor(page.color, theme().accentDim)
                             : (hover ? theme().tileHover : theme().panelAlt);
        fillRounded(g, r, S(6.0f), base);
        if (current) strokeRounded(g, r, S(6.0f), theme().accent, S(1.4f));

        RectF label(r.X + S(9.0f), r.Y, r.Width - S(18.0f), r.Height);
        drawText(g, W(page.displayName()), label, 11,
                 current ? theme().text : theme().textDim, Align::Center,
                 current);

        // A dot marks a page that an application can bring up by itself, so
        // the ones that switch on their own are told apart from the ones that
        // only ever switch because you asked.
        if (!page.rule.empty()) {
            RectF dot(r.GetRight() - S(8.0f), r.Y + S(6.0f), S(4.0f), S(4.0f));
            SolidBrush b(current ? theme().text : theme().textFaint);
            g.FillEllipse(&b, dot);
        }
    }

    // The padlock: shut means the deck stops following the application in
    // front. Drawn as a shackle over a body rather than shipped as an image,
    // like every other glyph here.
    const bool follow = config_.settings.switchPageWithApp;
    RectF pin = toRectF(rcPagePin_);
    const bool pinHover = hoverPageTab_ == -2;
    if (pinHover) fillRounded(g, pin, S(6.0f), theme().tileHover);
    Color pc = follow ? theme().good : theme().textFaint;
    const float bw = S(11.0f), bh = S(8.0f);
    RectF body(pin.X + (pin.Width - bw) / 2, pin.Y + pin.Height / 2 - S(1.0f),
               bw, bh);
    fillRounded(g, body, S(2.0f), pc);
    Pen pen(pc, S(1.6f));
    const float ar = S(3.6f);
    // An open shackle leans out to one side; a shut one sits square on top.
    RectF arc(body.X + body.Width / 2 - ar - (follow ? 0.0f : S(3.0f)),
              body.Y - ar * 2 + S(1.0f), ar * 2, ar * 2);
    g.DrawArc(&pen, arc, 180.0f, follow ? 180.0f : 150.0f);
}

void MainWindow::paintTile(Graphics& g, const DeckTile& tile,
                           cfg::Keyboard* kb) {
    RectF r = toRectF(tile.rect);
    const float radius = S(10.0f);
    int index = (int)(&tile - tiles_.data());
    bool hover = hoverTile_ == index;
    bool selected = selectedTile_ == index;

    const cfg::Binding* b =
        tile.binding >= 0 && tile.binding < (int)kb->active().bindings.size()
            ? &kb->active().bindings[(size_t)tile.binding]
            : nullptr;

    // recently-fired glow
    float fire = 0.0f;
    if (b) {
        auto it = lastFire_.find(currentAlias_ + "|" + b->key);
        if (it != lastFire_.end()) {
            DWORD age = GetTickCount() - it->second;
            if (age < 450) fire = 1.0f - age / 450.0f;
        }
    }

    if (!b) {
        fillRounded(g, r, radius, theme().tileEmpty);
        strokeRoundedDashed(g, r, radius, hover ? theme().accent : theme().border,
                            S(1.4f));
        RectF plus(r.X + r.Width / 2 - S(9.0f), r.Y + r.Height / 2 - S(9.0f),
                   S(18.0f), S(18.0f));
        drawGlyphPlus(g, plus, hover ? theme().accent : theme().textFaint,
                      S(1.8f));
        if (!tile.key.empty() && !kb->softwareOnly) {
            RectF kr(r.X, r.GetBottom() - S(22.0f), r.Width, S(16.0f));
            drawText(g, W(keys::displayName(tile.key)), kr, 10,
                     theme().textFaint, Align::Center);
        }
        if (selected) strokeRounded(g, r, radius, theme().accent, S(2.0f));
        return;
    }

    Color base = readableTileColor(b->color, hover ? theme().tileHover
                                                   : theme().tile);
    if (hover && trim(b->color).empty())
        base = theme().tileHover;
    if (fire > 0)
        base = mix(base, theme().good, fire * 0.55f);
    fillRounded(g, r, radius, base);
    strokeRounded(g, r, radius,
                  selected ? theme().accent
                           : withAlpha(theme().border, 140),
                  selected ? S(2.0f) : 1.0f);

    // key badge - meaningless on a button that has no key behind it
    if (!kb->softwareOnly) {
        RectF badge(r.X + S(7.0f), r.Y + S(6.0f), 0, S(15.0f));
        std::wstring keyLabel = W(keys::displayName(b->key));
        SizeF ks = measureText(g, keyLabel, 9, true);
        badge.Width = ks.Width + S(10.0f);
        fillRounded(g, badge, S(4.0f), withAlpha(Color(255, 0, 0, 0), 70));
        drawText(g, keyLabel, badge, 9, withAlpha(theme().text, 220),
                 Align::Center, true);
    }

    // icon: custom image if set, otherwise the first step's action glyph
    float iconSize = std::min(r.Width, r.Height) * 0.36f;
    RectF iconBox(r.X + (r.Width - iconSize) / 2, r.Y + r.Height * 0.24f,
                  iconSize, iconSize);
    // A "lib:" icon is drawn from code and never touches the image cache.
    if (isLibraryIcon(b->icon) &&
        drawLibraryIcon(g, libraryIconId(b->icon), iconBox,
                        withAlpha(Color(255, 255, 255, 255), 235))) {
        // drawn
    } else if (Image* img = b->icon.empty() || isLibraryIcon(b->icon)
                                ? nullptr
                                : icons_.get(b->icon)) {
        // preserve aspect ratio inside the box
        float iw = (float)img->GetWidth(), ih = (float)img->GetHeight();
        if (iw > 0 && ih > 0) {
            float scale = std::min(iconBox.Width / iw, iconBox.Height / ih);
            RectF dst(iconBox.X + (iconBox.Width - iw * scale) / 2,
                      iconBox.Y + (iconBox.Height - ih * scale) / 2, iw * scale,
                      ih * scale);
            g.DrawImage(img, dst);
        }
    } else if (!b->steps.empty()) {
        drawActionIcon(g, b->steps[0].action, iconBox,
                       withAlpha(Color(255, 255, 255, 255), 235));
    } else {
        RectF q(iconBox.X, iconBox.Y, iconBox.Width, iconBox.Height);
        drawText(g, L"?", q, iconSize * 0.8f / dpiScale(),
                 withAlpha(theme().textFaint, 200), Align::Center, true);
    }

    // A tile you can also turn says so, in the corner the key badge does
    // not use. Without it a dial is a feature nobody discovers.
    if (b->isDial()) {
        RectF mark(r.GetRight() - S(20.0f), r.Y + S(7.0f), S(12.0f), S(14.0f));
        drawGlyphDial(g, mark, withAlpha(theme().text, 170));
    }

    // label
    RectF label(r.X + S(5.0f), r.GetBottom() - S(30.0f), r.Width - S(10.0f),
                S(26.0f));
    drawText(g, W(b->displayTitle()), label, 11,
             Color(255, 255, 255, 255), Align::Center, false, true, true);

    if (b->steps.empty() && !b->isDial()) {
        RectF warn(r.X, r.Y + r.Height * 0.60f, r.Width, S(14.0f));
        drawText(g, L"no steps yet", warn, 9, theme().warn, Align::Center);
    } else if (b->steps.empty()) {
        // A dial with nothing on its press is finished, not half-built: it
        // does its job when you scroll it. Saying "no steps yet" here would
        // be telling the user to fix something that is not broken.
        RectF hint(r.X, r.Y + r.Height * 0.60f, r.Width, S(14.0f));
        drawText(g, L"scroll to use", hint, 9, theme().textFaint,
                 Align::Center);
    }
}

void MainWindow::paintDragGhost(Graphics& g) {
    const cfg::ActionDef* def = cfg::findAction(dragAction_);
    if (!def) return;
    RectF chip((REAL)(dragPos_.x + S(10)), (REAL)(dragPos_.y + S(8)),
               (REAL)S(170), (REAL)S(30));
    fillRounded(g, chip, S(6.0f), withAlpha(theme().accent, 235));
    RectF ic(chip.X + S(8.0f), chip.Y + S(7.0f), S(16.0f), S(16.0f));
    drawActionIcon(g, dragAction_, ic, Color(255, 255, 255, 255));
    RectF tr(chip.X + S(30.0f), chip.Y, chip.Width - S(34.0f), chip.Height);
    drawText(g, W(def->label), tr, 12, Color(255, 255, 255, 255), Align::Left);
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
    EnableWindow(btnIcon_, has);
    EnableWindow(btnColor_, has);
    EnableWindow(btnAddStep_, has);
    EnableWindow(btnRecord_, has);
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
    if (has)
        for (size_t i = 0; shown && i < shown->size(); ++i)
            SendMessageW(stepList_, LB_ADDSTRING, 0,
                         (LPARAM)W(std::to_string(i + 1) + ".  " +
                                   (*shown)[i].summary())
                             .c_str());
    EnableWindow(stepList_, has);

    if (has && keep >= 0) {
        // The step that was selected may have just been deleted off the end.
        if (shown && keep >= (int)shown->size())
            keep = (int)shown->size() - 1;
        if (keep >= 0) SendMessageW(stepList_, LB_SETCURSEL, (WPARAM)keep, 0);
    }

    updateStepButtons();
    InvalidateRect(hwnd_, &rcInspector_, FALSE);
}

void MainWindow::updateStepButtons() {
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
    if (!config_.save(&err)) {
        pushLog("Could not save settings: " + err);
        showMessage(hwnd_, "Could not save", err, true);
        return;
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
            "  It is the only pointer attached, so left click stays yours - "
            "MacroDeck will not take it however this deck is configured.");
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
    engine_.runSequenceAsync(steps, "Dial", currentAlias_);
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
    setPrimary(btnGesturePress_, gesture_ == 0);
    setPrimary(btnGestureUp_, gesture_ == 1);
    setPrimary(btnGestureDown_, gesture_ == 2);
    // A dot on a direction that already carries something, so a tile's other
    // gestures are visible without clicking through all three.
    setText(btnGestureUp_,
            std::string("Wheel \u2191") +
                (b && !b->scrollUp.empty() ? " \u2022" : ""));
    setText(btnGestureDown_,
            std::string("Wheel \u2193") +
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
            hwnd_, "Which key?",
            "Press the key on \"" + kb->alias + "\" that should run this macro.",
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
        showMessage(hwnd_, "Already used",
                    "\"" + keys::displayName(key) +
                        "\" already runs a macro on this keyboard.");
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

void MainWindow::recordClicks() {
    cfg::Binding* b = selectedBinding();
    if (!b) return;

    std::string countText = "1";
    if (!askText(hwnd_, "Record clicks",
                 "How many clicks do you want to record?", countText))
        return;
    char* end = nullptr;
    long count = strtol(countText.c_str(), &end, 10);
    if (!end || *end != '\0' || count < 1 || count > 200) {
        showMessage(hwnd_, "Check that number",
                    "Enter a whole number of clicks, from 1 to 200.", true);
        return;
    }

    // The window the first click landed in. Recorded clicks are absolute
    // screen coordinates, so they only land correctly if that window is back
    // at the size and position it had while recording - which is what the
    // window_prepare / window_release pair around the clicks arranges.
    ClickCapture target;
    bool haveTarget = false;

    std::vector<cfg::Step> recorded;
    for (int i = 1; i <= (int)count; ++i) {
        ClickCapture cc;
        std::string prompt =
            "Click " + std::to_string(i) + " of " + std::to_string(count) +
            ":\n\nMove to where you want this click, then press the RIGHT "
            "mouse button to record that spot.\n\nLeft click still works "
            "normally, so you can open whatever the click needs to land on "
            "first. Esc stops recording and keeps what's captured so far.";
        if (!captureClick(hwnd_, prompt, cc)) break;

        // As in the AutoHotkey original: the window is taken from the first
        // click only. Later clicks are expected to be inside it.
        if (i == 1 && cc.hasWindow) {
            target = cc;
            haveTarget = true;
        }

        cfg::Step click;
        click.action = "click";
        click.params = js::Value::object();
        click.params["x"] = cc.x;
        click.params["y"] = cc.y;
        recorded.push_back(click);

        std::string delayText = "200";
        if (!askText(hwnd_, "Record clicks",
                     "Delay after click " + std::to_string(i) +
                         ", in milliseconds:",
                     delayText,
                     "200 = fast    500 = half a second    1500 = slow dialog"))
            break;
        char* dend = nullptr;
        double ms = strtod(delayText.c_str(), &dend);
        if (!dend || *dend != '\0' || ms < 0) {
            showMessage(hwnd_, "Check that number",
                        "Enter a delay in milliseconds, 0 or more.", true);
            break;
        }
        if (ms > 0) {
            cfg::Step wait;
            wait.action = "sleep";
            wait.params = js::Value::object();
            wait.params["seconds"] = ms / 1000.0;
            recorded.push_back(wait);
        }
    }

    if (recorded.empty()) return;

    if (haveTarget) {
        cfg::Step prep;
        prep.action = "window_prepare";
        prep.params = js::Value::object();
        prep.params["class"] = target.cls;
        if (!target.exe.empty()) prep.params["exe"] = target.exe;
        prep.params["x"] = target.winX;
        prep.params["y"] = target.winY;
        prep.params["w"] = target.winW;
        prep.params["h"] = target.winH;
        recorded.insert(recorded.begin(), prep);

        cfg::Step release;
        release.action = "window_release";
        release.params = js::Value::object();
        recorded.push_back(release);
    }

    std::vector<cfg::Step>* steps = editingSteps();
    if (!steps) return;
    for (auto& s : recorded) steps->push_back(s);
    modelChanged();
    SendMessageW(stepList_, LB_SETCURSEL, (WPARAM)(steps->size() - 1), 0);
    refreshInspector();

    pushLog("Recorded " + std::to_string(recorded.size()) + " step(s) into \"" +
            b->displayTitle() + "\".");
    if (haveTarget) {
        pushLog("  locked to " + target.cls +
                (target.exe.empty() ? "" : " (" + target.exe + ")") + " at " +
                std::to_string(target.winX) + "," +
                std::to_string(target.winY) + " " +
                std::to_string(target.winW) + "x" +
                std::to_string(target.winH) +
                " - the clicks only land correctly there.");
        if (target.maximized)
            pushLog("  it was maximised; it will be un-maximised to that exact "
                    "size, then put back afterwards.");
    } else {
        pushLog("  no window was detected under the first click, so the clicks "
                "are not locked to one - they will go wherever those screen "
                "positions happen to be.");
    }
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
    engine_.runSequenceAsync(*steps, "Test", currentAlias_);
}

void MainWindow::clearTile() {
    cfg::Binding* b = selectedBinding();
    if (!b) return;
    if (!askYesNo(hwnd_, "Clear this key",
                  "Remove every step from \"" + b->displayTitle() + "\"?"))
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
    if (!askYesNo(hwnd_, "Remove this key",
                  "Remove \"" + name + "\" from the deck?"))
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
        setTone(btnEngine_, Tone::Good);
    } else {
        try {
            engine_.updateConfig(config_);
            engine_.start();
            setTone(btnEngine_, Tone::Danger);
        } catch (const std::exception& e) {
            showMessage(hwnd_, "Could not start listening", e.what(), true);
            pushLog(std::string("Could not start listening: ") + e.what());
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
        showMessage(hwnd_, "Import failed", err, true);
        return;
    }

    int keys = 0;
    for (const auto& kb : imported.keyboards) keys += totalBindings(kb);
    if (!askYesNo(hwnd_, "Import",
                  "Found " + std::to_string(imported.keyboards.size()) +
                      " keyboard(s) and " + std::to_string(keys) +
                      " macro(s).\n\nAdd them to MacroDeck?"))
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

void MainWindow::showLayouts() {
    auto names = win::listLayouts(config_.settings.layoutsDir);
    std::string body;
    if (names.empty())
        body =
            "No window layouts saved yet.\n\nAdd a \"Save the window layout\" "
            "step to a key, run it, and the arrangement of your windows is "
            "stored here.";
    else {
        body = "Saved window layouts:\n\n";
        for (const auto& n : names) body += "   \xE2\x80\xA2  " + n + "\n";
        body += "\nUse them with the \"Restore a window layout\" action.";
    }
    showMessage(hwnd_, "Window layouts", body);
}

void MainWindow::reloadConfig() {
    if (config_.dirty &&
        !askYesNo(hwnd_, "Reload macros.json",
                  "There are unsaved changes. Reloading from disk throws "
                  "them away.\n\nReload anyway?"))
        return;

    const bool wasRunning = engine_.running();
    if (wasRunning) engine_.stop();

    const std::string previous = currentAlias_;
    config_.load(appPath("macros.json"));
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
    std::wstring initial = W("macrodeck-" + dateStamp() + ".json");
    std::vector<wchar_t> file(MAX_PATH * 2, L'\0');
    wcsncpy(file.data(), initial.c_str(), file.size() - 1);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"MacroDeck settings\0*.json\0All files\0*.*\0";
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrDefExt = L"json";
    ofn.lpstrTitle = L"Export a copy of your settings";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return;

    std::string err;
    const std::string dest = narrow(file.data());
    if (!config_.saveAs(dest, &err)) {
        showMessage(hwnd_, "Export failed", err, true);
        return;
    }
    pushLog("Exported a copy to " + dest);
}

void MainWindow::backupNow() {
    if (config_.dirty) saveConfig();
    std::string where;
    if (config_.writeDailyBackup(10, &where)) {
        pushLog("Backed up to " + relativeToApp(where));
        showMessage(hwnd_, "Backed up",
                    "Today's copy is in:\n\n" + where);
    } else {
        showMessage(hwnd_, "Already backed up",
                    "Today's backup already exists in the backups folder next "
                    "to MacroDeck. Use \"Export a copy\" for a second one.");
    }
}

void MainWindow::duplicateBinding(int tileIndex) {
    cfg::Keyboard* kb = currentKb();
    if (!kb || tileIndex < 0 || tileIndex >= (int)tiles_.size()) return;
    const int bi = tiles_[(size_t)tileIndex].binding;
    if (bi < 0 || bi >= (int)kb->active().bindings.size()) return;
    const cfg::Binding source = kb->active().bindings[(size_t)bi];

    KeyCaptureResult r =
        captureKey(hwnd_, "Duplicate to which key?",
                   "Press the key on \"" + kb->alias +
                       "\" that should get a copy of \"" +
                       source.displayTitle() + "\".",
                   kb->match);
    if (!r.ok) return;

    if (kb->findKey(r.keyName)) {
        showMessage(hwnd_, "Already used",
                    "\"" + keys::displayName(r.keyName) +
                        "\" already runs a macro on this keyboard.");
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
                " - that key is not part of this deck layout, so it has no "
                "tile here. It still works.");
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
    if (!askText(hwnd_, "New page", "Name for this page:", name)) return;
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
        showMessage(hwnd_, "Remove page",
                    "A deck always has at least one page. Add another one "
                    "before removing this.");
        return;
    }
    const std::string removed = kb->pages[(size_t)pageIndex].displayName();
    if (!askYesNo(hwnd_, "Remove page",
                  "Remove \"" + removed + "\" and all " +
                      std::to_string(kb->pages[(size_t)pageIndex].bindings.size()) +
                      " of its keys?"))
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

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_PAGE_APP_CLEAR, L"No application rule");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < uniq.size() && i < 60; ++i)
        AppendMenuW(m, MF_STRING, (UINT_PTR)(IDM_PAGE_APP_PICK + i),
                    W(uniq[i].exe + "   -   " + uniq[i].title).c_str());

    POINT pt;
    GetCursorPos(&pt);
    const int cmd = (int)TrackPopupMenu(
        m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd_,
        nullptr);
    DestroyMenu(m);
    if (!cmd) return;

    cfg::Page& page = kb->pages[(size_t)pageIndex];
    if (cmd == IDM_PAGE_APP_CLEAR) {
        page.rule = cfg::AppRule{};
        pushLog("\"" + page.displayName() +
                "\" no longer follows an application.");
    } else {
        const size_t i = (size_t)(cmd - IDM_PAGE_APP_PICK);
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
    AppendMenuW(m, MF_STRING, IDM_PAGE_RENAME, L"Rename this page...");
    AppendMenuW(m, MF_STRING, IDM_PAGE_APP,
                L"Show this page for an application...");
    if (!kb->pages[(size_t)pageIndex].rule.empty())
        AppendMenuW(m, MF_STRING, IDM_PAGE_APP_CLEAR,
                    L"Stop following an application");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_PAGE_COLOR, L"Page colour...");
    if (!kb->pages[(size_t)pageIndex].color.empty())
        AppendMenuW(m, MF_STRING, IDM_PAGE_COLOR_CLEAR, L"Clear the colour");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_PAGE_DUPLICATE, L"Duplicate this page");
    AppendMenuW(m, MF_STRING | (pageIndex > 0 ? 0u : (UINT)MF_GRAYED),
                IDM_PAGE_LEFT, L"Move left");
    AppendMenuW(m,
                MF_STRING | (pageIndex + 1 < (int)kb->pages.size()
                                 ? 0u
                                 : (UINT)MF_GRAYED),
                IDM_PAGE_RIGHT, L"Move right");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_PAGE_REMOVE, L"Remove this page");

    const int cmd = (int)TrackPopupMenu(
        m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, screenPt.x, screenPt.y,
        0, hwnd_, nullptr);
    DestroyMenu(m);
    if (!cmd) return;

    cfg::Page& page = kb->pages[(size_t)pageIndex];
    switch (cmd) {
        case IDM_PAGE_RENAME: {
            std::string name = page.name;
            if (askText(hwnd_, "Rename page", "Name for this page:", name)) {
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

    // MacroDeck's own windows are not an application change. Editing a deck
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
        pushLog("Start/stop listening from anywhere with " +
                keys::prettyCombo(combo) + ".");
    } else {
        pushLog("Another program already owns " + keys::prettyCombo(combo) +
                " - pick a different start/stop shortcut in Settings.");
    }
    // The button spells the shortcut out, so it has to be rebuilt whenever
    // the shortcut changes or fails to register.
    layoutEngineButton();
}

void MainWindow::showMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (mini_.visible() ? MF_CHECKED : 0), IDM_MINIDECK,
                L"Buttons on top of everything");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_SNIP, L"Snip a template image...");
    AppendMenuW(m, MF_STRING, IDM_LAYOUTS, L"Window layouts...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_IMPORT, L"Import from macrokb (macros.yaml)...");
    AppendMenuW(m, MF_STRING, IDM_SAVE, L"Save now");
    AppendMenuW(m, MF_STRING, IDM_RELOAD, L"Reload macros.json");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_BACKUP, L"Back up settings now");
    AppendMenuW(m, MF_STRING, IDM_EXPORT, L"Export a copy...");
    AppendMenuW(m, MF_STRING, IDM_OPEN_FOLDER, L"Open the MacroDeck folder");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_ABOUT, L"About MacroDeck");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");

    RECT r;
    GetWindowRect(btnMenu_, &r);
    TrackPopupMenu(m, TPM_RIGHTALIGN | TPM_TOPALIGN, r.right, r.bottom, 0,
                   hwnd_, nullptr);
    DestroyMenu(m);
}

void MainWindow::addSoftwareDeck() {
    std::string name = "Buttons";
    if (!askText(hwnd_, "Software buttons",
                 "What should this set of buttons be called?", name,
                 "They live on the deck and on the always-on-top palette. "
                 "No keyboard is involved, so nothing is taken from anything "
                 "you type."))
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
    pushLog("Added software buttons \"" + config_.keyboards.back().alias +
            "\". Nothing is taken from any keyboard.");
    layout();
}

void MainWindow::keyboardContextMenu(int index, POINT pt) {
    if (index < 0 || index >= (int)config_.keyboards.size()) return;
    cfg::Keyboard& kb = config_.keyboards[(size_t)index];

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_KB_RENAME, L"Rename...");
    AppendMenuW(m, MF_STRING | (kb.block ? MF_CHECKED : 0), IDM_KB_BLOCK,
                L"Block these keys from apps");
    // Greyed rather than hidden when the keyboard blocks nothing at all: the
    // option still describes this keyboard, it just has nothing to act on.
    AppendMenuW(m,
                MF_STRING | (kb.blockAllKeys ? MF_CHECKED : 0) |
                    (kb.block ? 0 : MF_GRAYED),
                IDM_KB_BLOCK_ALL,
                L"      Turn off all normal input, not just macro keys");
    AppendMenuW(m, MF_STRING, IDM_KB_REMATCH, L"Re-detect this keyboard...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_KB_REMOVE, L"Remove this keyboard");
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x, pt.y,
                                  0, hwnd_, nullptr);
    DestroyMenu(m);

    switch (cmd) {
        case IDM_KB_RENAME: {
            std::string name = kb.alias;
            if (askText(hwnd_, "Rename keyboard", "New name:", name)) {
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
                              "\" now gives up only the keys that run macros - "
                              "the rest type normally.");
            // Unlike the master toggle this needs no restart: the hook asks
            // the config afresh on every keystroke.
            break;
        case IDM_KB_REMATCH: {
            KeyCaptureResult r = captureKey(
                hwnd_, "Re-detect keyboard",
                "Press a key on the keyboard you want \"" + kb.alias +
                    "\" to mean.",
                "", false, false);
            if (r.ok && !r.devicePath.empty()) {
                auto all = dev::listKeyboards();
                for (const auto& d : all)
                    if (d.path == r.devicePath) {
                        bool stable = true;
                        kb.match = cfg::chooseMatch(d, all, &stable);
                        modelChanged(false);
                        pushLog("\"" + kb.alias + "\" now matches " + kb.match);
                        break;
                    }
            }
            break;
        }
        case IDM_KB_REMOVE: {
            if (!askYesNo(hwnd_, "Remove keyboard",
                          "Remove \"" + kb.alias + "\" and all " +
                              std::to_string(totalBindings(kb)) +
                              " of its keys?"))
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
}

void MainWindow::tileContextMenu(int tileIndex, POINT pt) {
    selectTile(tileIndex);
    cfg::Binding* b = selectedBinding();

    HMENU m = CreatePopupMenu();
    if (!b) {
        AppendMenuW(m, MF_STRING, IDM_TILE_REKEY, L"Assign a key here...");
    } else {
        AppendMenuW(m, MF_STRING, IDM_TILE_RENAME, L"Rename...");
        AppendMenuW(m, MF_STRING, IDM_TILE_ICON, L"Choose an icon...");
        if (!b->icon.empty())
            AppendMenuW(m, MF_STRING, IDM_TILE_ICON_CLEAR, L"Remove the icon");
        AppendMenuW(m, MF_STRING, IDM_TILE_COLOR, L"Choose a colour...");
        if (!b->color.empty())
            AppendMenuW(m, MF_STRING, IDM_TILE_COLOR_CLEAR,
                        L"Use the default colour");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (b->steps.empty() ? MF_GRAYED : 0),
                    IDM_TILE_TEST, L"Test run");
        AppendMenuW(m, MF_STRING, IDM_TILE_DUPLICATE, L"Duplicate to another key...");
        AppendMenuW(m, MF_STRING | (b->steps.empty() ? MF_GRAYED : 0),
                    IDM_TILE_COPY_STEPS, L"Copy the steps");
        AppendMenuW(m, MF_STRING | (stepClipboard_.empty() ? MF_GRAYED : 0),
                    IDM_TILE_PASTE_STEPS,
                    stepClipboard_.empty()
                        ? L"Paste steps"
                        : (L"Paste " +
                           std::to_wstring(stepClipboard_.size()) +
                           L" step(s)").c_str());
        AppendMenuW(m, MF_STRING | (b->steps.empty() ? MF_GRAYED : 0),
                    IDM_TILE_CLEAR, L"Clear all steps");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_TILE_REMOVE, L"Remove this key");
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
            if (askText(hwnd_, "Rename key", "Label for this key:", t)) {
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
                       e.alias + (e.hasMacro ? "" : " (no macro)");
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
    mini_.setButtons(kb ? kb->alias : std::string("MacroDeck"), std::move(out));
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
    engine_.runSequenceAsync(b->steps, "Buttons", currentAlias_);
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
    wcsncpy(nid.szTip, L"MacroDeck", 127);
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
    AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, L"Open MacroDeck");
    AppendMenuW(m, MF_STRING, IDM_TRAY_TOGGLE,
                engine_.running() ? L"Stop listening" : L"Start listening");
    AppendMenuW(m, MF_STRING | (mini_.visible() ? MF_CHECKED : 0), IDM_TRAY_MINI,
                L"Buttons on top of everything");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"Exit");
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
            showMessage(hwnd_, "No notification-area icon",
                        "Windows would not give MacroDeck an icon in the "
                        "notification area, so closing the window would leave "
                        "it running with no way back.\n\nExiting instead.",
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
            mmi->ptMinTrackSize.x = S(940);
            mmi->ptMinTrackSize.y = S(600);
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

        case WM_APP_FOREGROUND:
            self->onForegroundChanged((HWND)wp);
            return 0;

        case WM_TIMER:
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
            if (self->dragging_ || oldTile != self->hoverTile_ ||
                oldPal != self->hoverPalette_ || oldKb != self->hoverKb_ ||
                oldPage != self->hoverPageTab_)
                InvalidateRect(hwnd, nullptr, FALSE);

            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            self->hoverTile_ = self->hoverPalette_ = -1;
            self->hoverKb_ = -1;
            self->hoverPageTab_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            SetFocus(hwnd);

            if (self->config_.keyboards.empty()) {
                if (ptIn(self->rcWelcomeBtn_, p)) self->addKeyboard();
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
                        if (askText(hwnd, "Rename page", "Name for this page:",
                                    name)) {
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
                if (ptIn(t.rect, p) && t.index >= 0) {
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
                    if (notify == BN_CLICKED &&
                        settingsDialog(hwnd, self->config_.settings)) {
                        self->saveConfig();
                        self->engine_.updateConfig(self->config_);
                        self->applyToggleHotkey();
                    }
                    return 0;
                case ID_BTN_MENU:
                    if (notify == BN_CLICKED) self->showMenu();
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

                case IDM_SNIP: {
                    std::string path;
                    if (snipTemplate(hwnd, path))
                        self->pushLog("Saved template " + path);
                    return 0;
                }
                case IDM_MINIDECK: self->toggleMiniDeck(); return 0;
                case IDM_LAYOUTS: self->showLayouts(); return 0;
                case IDM_IMPORT: self->importYaml(); return 0;
                case IDM_SAVE:
                    self->saveConfig();
                    self->pushLog("Settings saved.");
                    return 0;
                case IDM_RELOAD: self->reloadConfig(); return 0;
                case IDM_EXPORT: self->exportConfig(); return 0;
                case IDM_BACKUP: self->backupNow(); return 0;
                case IDM_OPEN_FOLDER:
                    ShellExecuteW(nullptr, L"open", widen(exeDir()).c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case IDM_ABOUT:
                    showMessage(
                        hwnd, "About MacroDeck",
                        "MacroDeck - a per-keyboard macro deck for Windows.\n\n"
                        "Because Windows Raw Input reports which physical "
                        "keyboard each keystroke came from, a spare keyboard "
                        "or numpad can be dedicated entirely to macros while "
                        "you keep typing normally on your main one.\n\n"
                        "Everything runs locally. Nothing is sent anywhere.\n\n"
                        "Settings are stored in macros.json next to the "
                        "program.");
                    return 0;
                case IDM_EXIT:
                    self->quitting_ = true;
                    DestroyWindow(hwnd);
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
        MessageBoxW(nullptr, L"MacroDeck could not create its window.",
                    L"MacroDeck", MB_ICONERROR);
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
