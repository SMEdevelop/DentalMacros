#include "winman.h"

#include "json.h"
#include "util.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>

namespace win {
namespace {

std::string exeForWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return "";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return "";
    wchar_t buf[MAX_PATH * 2];
    DWORD size = MAX_PATH * 2;
    std::string out;
    if (QueryFullProcessImageNameW(h, 0, buf, &size))
        out = baseName(narrow(std::wstring(buf, size)));
    CloseHandle(h);
    return out;
}

struct EnumCtx {
    std::vector<WindowInfo>* out;
    bool includeUntitled;
};

BOOL CALLBACK enumProc(HWND hwnd, LPARAM param) {
    auto* ctx = (EnumCtx*)param;
    if (!IsWindowVisible(hwnd)) return TRUE;

    int len = GetWindowTextLengthW(hwnd);
    if (len == 0 && !ctx->includeUntitled) return TRUE;

    std::wstring title((size_t)len + 1, L'\0');
    if (len > 0) {
        int got = GetWindowTextW(hwnd, &title[0], len + 1);
        title.resize((size_t)std::max(0, got));
    } else {
        title.clear();
    }

    // Skip tool windows and the invisible shell windows that clutter lists.
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;

    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return TRUE;

    WindowInfo wi;
    wi.hwnd = hwnd;
    wi.title = narrow(title);
    wi.exe = exeForWindow(hwnd);
    wi.x = r.left;
    wi.y = r.top;
    wi.w = r.right - r.left;
    wi.h = r.bottom - r.top;
    wi.minimized = IsIconic(hwnd) != 0;
    wi.maximized = IsZoomed(hwnd) != 0;
    ctx->out->push_back(std::move(wi));
    return TRUE;
}

std::string layoutPath(const std::string& name, const std::string& dir) {
    std::string safe;
    for (char c : name)
        safe += (strchr("\\/:*?\"<>|", c) ? '_' : c);
    return joinPath(joinPath(exeDir(), dir), safe + ".json");
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(lower(s));
    std::string w;
    while (is >> w) out.push_back(w);
    return out;
}

}  // namespace

std::vector<WindowInfo> listWindows(bool includeUntitled) {
    std::vector<WindowInfo> out;
    EnumCtx ctx{&out, includeUntitled};
    EnumWindows(enumProc, (LPARAM)&ctx);
    return out;
}

bool findWindow(const std::string& title, const std::string& exe,
                WindowInfo& out) {
    for (const auto& w : listWindows()) {
        if (!title.empty() && !icontains(w.title, title)) continue;
        if (!exe.empty() && !iequals(w.exe, exe)) continue;
        out = w;
        return true;
    }
    return false;
}

void focusWindow(const WindowInfo& w) {
    if (!IsWindow(w.hwnd)) throw std::runtime_error("that window is gone");
    if (IsIconic(w.hwnd)) ShowWindow(w.hwnd, SW_RESTORE);

    // Windows only lets the foreground thread hand focus over. Briefly
    // attaching to it makes SetForegroundWindow reliable instead of leaving
    // the taskbar button flashing.
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myThread = GetCurrentThreadId();
    bool attached = false;
    if (fgThread && fgThread != myThread)
        attached = AttachThreadInput(myThread, fgThread, TRUE) != 0;
    SetForegroundWindow(w.hwnd);
    BringWindowToTop(w.hwnd);
    if (attached) AttachThreadInput(myThread, fgThread, FALSE);
}

void moveResize(const WindowInfo& w, bool hasX, int x, bool hasY, int y,
                bool hasW, int width, bool hasH, int height) {
    if (!IsWindow(w.hwnd)) throw std::runtime_error("that window is gone");
    if (IsIconic(w.hwnd) || IsZoomed(w.hwnd)) ShowWindow(w.hwnd, SW_RESTORE);
    RECT r{};
    GetWindowRect(w.hwnd, &r);
    int nx = hasX ? x : r.left;
    int ny = hasY ? y : r.top;
    int nw = hasW ? width : (r.right - r.left);
    int nh = hasH ? height : (r.bottom - r.top);
    SetWindowPos(w.hwnd, nullptr, nx, ny, nw, nh,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void minimizeWindow(const WindowInfo& w) { ShowWindow(w.hwnd, SW_MINIMIZE); }
void maximizeWindow(const WindowInfo& w) { ShowWindow(w.hwnd, SW_MAXIMIZE); }

// ---------------------------------------------------------------- layouts
bool saveLayout(const std::string& name, const std::string& layoutsDir,
                std::string* error) {
    js::Value arr = js::Value::array();
    for (const auto& w : listWindows()) {
        if (w.title.empty() || w.minimized) continue;
        js::Value e = js::Value::object();
        e["title"] = w.title;
        e["exe"] = w.exe;
        e["x"] = w.x;
        e["y"] = w.y;
        e["w"] = w.w;
        e["h"] = w.h;
        e["maximized"] = w.maximized;
        arr.push(std::move(e));
    }
    if (!writeFileAtomic(layoutPath(name, layoutsDir), arr.dump(2))) {
        if (error) *error = "could not write the layout file";
        return false;
    }
    return true;
}

int restoreLayout(const std::string& name, const std::string& layoutsDir,
                  std::string* error) {
    std::string text;
    if (!readFile(layoutPath(name, layoutsDir), text)) {
        if (error) *error = "no saved layout called '" + name + "'";
        return 0;
    }
    std::string perr;
    js::Value arr = js::Value::parse(text, &perr);
    if (!arr.isArr()) {
        if (error) *error = "layout file is damaged: " + perr;
        return 0;
    }

    std::vector<WindowInfo> current;
    for (auto& w : listWindows())
        if (!w.title.empty()) current.push_back(w);

    std::set<HWND> used;
    int restored = 0;

    // Match on exe first, then title similarity, so a layout still applies
    // after the apps have been restarted (and their HWNDs changed).
    auto score = [&](const js::Value& e, const WindowInfo& w) {
        int s = 0;
        std::string eExe = e.str("exe"), eTitle = e.str("title");
        if (!eExe.empty() && iequals(eExe, w.exe)) s += 10;
        std::string et = lower(eTitle), ct = lower(w.title);
        if (!et.empty() && et == ct) s += 8;
        else if (!et.empty() && (ct.find(et) != std::string::npos ||
                                 et.find(ct) != std::string::npos))
            s += 4;
        else {
            auto ew = words(et), cw = words(ct);
            int overlap = 0;
            for (auto& a : ew)
                if (std::find(cw.begin(), cw.end(), a) != cw.end()) ++overlap;
            s += std::min(overlap, 3);
        }
        return s;
    };

    for (const js::Value& e : arr.arr) {
        const WindowInfo* best = nullptr;
        int bestScore = 0;
        for (const auto& w : current) {
            if (used.count(w.hwnd)) continue;
            int s = score(e, w);
            if (s > bestScore) { bestScore = s; best = &w; }
        }
        if (!best || bestScore < 4) continue;
        used.insert(best->hwnd);
        int x = e.integer("x"), y = e.integer("y");
        int w = e.integer("w"), h = e.integer("h");
        if (e.boolean("maximized")) {
            ShowWindow(best->hwnd, SW_RESTORE);
            moveResize(*best, true, x, true, y, true, w, true, h);
            ShowWindow(best->hwnd, SW_MAXIMIZE);
        } else {
            moveResize(*best, true, x, true, y, true, w, true, h);
        }
        ++restored;
    }
    return restored;
}

std::vector<std::string> listLayouts(const std::string& layoutsDir) {
    std::vector<std::string> out;
    for (const std::string& f :
         listFiles(joinPath(exeDir(), layoutsDir), ".json"))
        out.push_back(stripExt(f));
    return out;
}

bool deleteLayout(const std::string& name, const std::string& layoutsDir) {
    return DeleteFileW(widen(layoutPath(name, layoutsDir)).c_str()) != 0;
}

}  // namespace win
