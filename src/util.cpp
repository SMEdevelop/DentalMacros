#include "util.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <new>

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n,
                        nullptr, nullptr);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return r;
}

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && lower(a) == lower(b);
}

bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    return lower(hay).find(lower(needle)) != std::string::npos;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string replaceAll(std::string s, const std::string& from,
                       const std::string& to) {
    if (from.empty()) return s;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
    return s;
}

std::string exePath() {
    // Magic-static initialisation is thread-safe; a plain "assign once if
    // empty" is not, and both the UI thread and the macro worker ask for this.
    static const std::string cached = [] {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
        return narrow(std::wstring(buf, n));
    }();
    return cached;
}

std::string exeDir() {
    static const std::string cached = [] {
        std::string p = exePath();
        size_t slash = p.find_last_of("\\/");
        return slash == std::string::npos ? p : p.substr(0, slash);
    }();
    return cached;
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    // already absolute?
    if (b.size() > 1 && (b[1] == ':' || b[0] == '\\' || b[0] == '/')) return b;
    char last = a[a.size() - 1];
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

std::string appPath(const std::string& relative) {
    return joinPath(exeDir(), relative);
}

std::string relativeToApp(const std::string& absolute) {
    const std::string low = lower(absolute);
    for (const std::string& dir : {dataDir(), exeDir()}) {
        const std::string root = lower(dir) + "\\";
        if (low.rfind(root, 0) == 0) return absolute.substr(root.size());
    }
    return absolute;
}

// ------------------------------------------------------------ data folder
bool canCreateIn(const std::string& dir) {
    if (dir.empty()) return false;
    ensureDir(dir);
    const std::string probe =
        joinPath(dir, "~dm-write-test-" + std::to_string(GetCurrentProcessId()) +
                          ".tmp");
    HANDLE h = CreateFileW(widen(probe).c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY |
                                           FILE_FLAG_DELETE_ON_CLOSE,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);   // and it is gone
    return true;
}

bool fileDeniesWrites(const std::string& file) {
    const DWORD attrs = GetFileAttributesW(widen(file).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    if (attrs & FILE_ATTRIBUTE_READONLY) return true;
    // Opened for writing and closed again at once, sharing everything so it
    // can't disturb anyone who has it open. Only "access denied" counts: a
    // sharing violation is a sync client passing through, not a refusal.
    HANDLE h = CreateFileW(widen(file).c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return false;
    }
    return GetLastError() == ERROR_ACCESS_DENIED;
}

std::string profileDataDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    return joinPath(narrow(buf), "DentalMacros");
}

std::string chooseDataDir(const std::string& exe, const std::string& profile,
                          const DataDirProbe& probe) {
    const std::string settings = joinPath(exe, "macros.json");
    const bool exeOk = probe.canCreateIn(exe) &&
                       !(probe.exists(settings) && probe.deniesWrites(settings));
    if (exeOk || profile.empty()) return exe;
    // Nowhere better to go: stay, and let the first save explain why.
    if (!probe.canCreateIn(profile)) return exe;
    return profile;
}

bool migrateData(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || iequals(from, to)) return false;
    if (fileExists(joinPath(to, "macros.json"))) return false;
    ensureDir(to);
    bool copied = false;
    if (fileExists(joinPath(from, "macros.json")))
        copied = copyFile(joinPath(from, "macros.json"),
                          joinPath(to, "macros.json"), false);
    // The pictures and layouts the settings refer to by relative path. They
    // would still be found beside the program, but a copy makes the profile
    // folder whole - and editable.
    for (const char* sub : {"images", "layouts"}) {
        const std::string src = joinPath(from, sub);
        const std::vector<std::string> files = listFiles(src, "");
        if (files.empty()) continue;
        ensureDir(joinPath(to, sub));
        for (const std::string& f : files)
            if (copyFile(joinPath(src, f), joinPath(joinPath(to, sub), f), false))
                copied = true;
    }
    return copied;
}

namespace {
std::mutex g_dataMu;
std::string g_dataDir;   // empty until first asked for

const DataDirProbe kRealProbe{&canCreateIn, &fileExists, &fileDeniesWrites};
}  // namespace

std::string dataDir() {
    std::lock_guard<std::mutex> lk(g_dataMu);
    if (g_dataDir.empty()) {
        const std::string exe = exeDir();
        g_dataDir = chooseDataDir(exe, profileDataDir(), kRealProbe);
        // Moving out for the first time: bring the settings along.
        if (!iequals(g_dataDir, exe)) migrateData(exe, g_dataDir);
    }
    return g_dataDir;
}

std::string dataPath(const std::string& relative) {
    return joinPath(dataDir(), relative);
}

bool dataDirIsFallback() { return !iequals(dataDir(), exeDir()); }

std::string resolveDataPath(const std::string& rel) {
    if (rel.empty()) return rel;
    const bool absolute =
        rel.size() > 1 && (rel[1] == ':' || rel[0] == '\\' || rel[0] == '/');
    if (absolute) return rel;
    const std::string inData = joinPath(dataDir(), rel);
    if (fileExists(inData)) return inData;
    const std::string inExe = joinPath(exeDir(), rel);
    if (fileExists(inExe)) return inExe;
    if (fileExists(rel)) return rel;   // the working folder, as it always was
    return inData;
}

bool moveDataToProfile(std::string* error) {
    const std::string profile = profileDataDir();
    if (profile.empty()) {
        if (error) *error = "Windows did not say where this user's profile is";
        return false;
    }
    if (!canCreateIn(profile)) {
        if (error) *error = "could not write in " + profile;
        return false;
    }
    const std::string from = dataDir();
    if (iequals(from, profile)) return true;
    migrateData(from, profile);
    std::lock_guard<std::mutex> lk(g_dataMu);
    g_dataDir = profile;
    return true;
}

std::string baseName(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string stripExt(const std::string& path) {
    size_t dot = path.find_last_of('.');
    size_t slash = path.find_last_of("\\/");
    if (dot == std::string::npos) return path;
    if (slash != std::string::npos && dot < slash) return path;
    return path.substr(0, dot);
}

bool fileExists(const std::string& path) {
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::string& path) {
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void ensureDir(const std::string& path) {
    if (path.empty() || dirExists(path)) return;
    size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos && slash > 2)
        ensureDir(path.substr(0, slash));
    CreateDirectoryW(widen(path).c_str(), nullptr);
}

ReadResult readFileEx(const std::string& path, std::string& out) {
    out.clear();
    // FILE_SHARE_WRITE as well as READ: a sync client or backup tool holding
    // the file open for writing should not look like a missing file.
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        bool missing = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
        return missing ? ReadResult::Missing : ReadResult::Unreadable;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return ReadResult::Unreadable; }
    // A config or layout file is never this big; refuse rather than trying to
    // allocate whatever was pointed at us.
    if (sz.QuadPart > 64LL * 1024 * 1024) {
        CloseHandle(h);
        return ReadResult::Unreadable;
    }
    try {
        out.resize((size_t)sz.QuadPart);
    } catch (const std::bad_alloc&) {
        CloseHandle(h);
        return ReadResult::Unreadable;
    }
    DWORD got = 0;
    BOOL ok = TRUE;
    if (sz.QuadPart > 0)
        ok = ReadFile(h, &out[0], (DWORD)sz.QuadPart, &got, nullptr);
    CloseHandle(h);
    if (!ok) { out.clear(); return ReadResult::Unreadable; }
    out.resize(got);
    // tolerate a UTF-8 BOM
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF &&
        (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return ReadResult::Ok;
}

bool readFile(const std::string& path, std::string& out) {
    return readFileEx(path, out) == ReadResult::Ok;
}

bool copyFile(const std::string& from, const std::string& to, bool overwrite) {
    return CopyFileW(widen(from).c_str(), widen(to).c_str(),
                     overwrite ? FALSE : TRUE) != 0;
}

bool deleteFile(const std::string& path) {
    return DeleteFileW(widen(path).c_str()) != 0;
}

std::string freeBackupPath(const std::string& path) {
    std::string candidate = path + ".bak";
    for (int n = 2; n < 100 && fileExists(candidate); ++n)
        candidate = path + ".bak." + std::to_string(n);
    return candidate;
}

bool writeFileAtomic(const std::string& path, const std::string& data) {
    size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) ensureDir(path.substr(0, slash));
    std::string tmp = path + ".tmp";
    HANDLE h = CreateFileW(widen(tmp).c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = TRUE;
    if (!data.empty())
        ok = WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr);
    DWORD err = ok ? 0 : GetLastError();
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || wrote != data.size()) {
        if (!err) err = ERROR_DISK_FULL;   // a short write is a full disk
        DeleteFileW(widen(tmp).c_str());
        SetLastError(err);
        return false;
    }
    // Sync and antivirus software open a file the moment it changes and hold
    // it for a fraction of a second, which makes the swap fail with a sharing
    // violation or access denied. Worth a few quiet retries before giving up.
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(widen(tmp).c_str(), widen(path).c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        err = GetLastError();
        const bool transient =
            err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION ||
            err == ERROR_ACCESS_DENIED || err == ERROR_USER_MAPPED_FILE;
        if (!transient || attempt >= 4) break;
        Sleep(100);
    }
    DeleteFileW(widen(tmp).c_str());
    SetLastError(err);   // for the caller, who has to explain it
    return false;
}

std::vector<std::string> listFiles(const std::string& dir,
                                   const std::string& ext) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(widen(joinPath(dir, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = narrow(fd.cFileName);
        if (ext.empty() || (name.size() > ext.size() &&
                            iequals(name.substr(name.size() - ext.size()), ext)))
            out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

// ------------------------------------------------------------- run at login
namespace {
const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"DentalMacros";
// What the entry was called before the rename to Dental Macros.
const wchar_t* kOldRunValue = L"MacroDeck";

// Quoted so a path containing spaces still launches.
std::wstring runCommand() { return L"\"" + widen(exePath()) + L"\""; }
}  // namespace

bool runAtLoginEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) !=
        ERROR_SUCCESS)
        return false;
    wchar_t val[MAX_PATH * 2];
    DWORD sz = sizeof(val) - sizeof(wchar_t), type = 0;
    bool on = false;
    if (RegQueryValueExW(key, kRunValue, nullptr, &type, (LPBYTE)val, &sz) ==
            ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        val[sz / sizeof(wchar_t)] = L'\0';
        // Only report "on" if the entry still points at this copy of the exe;
        // a stale entry from a folder the user has since moved is not enabled.
        std::string recorded = narrow(val);
        std::string want = exePath();
        on = icontains(recorded, want);
    }
    RegCloseKey(key);
    return on;
}

bool setRunAtLogin(bool on, std::string* error) {
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                              KEY_SET_VALUE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        if (error) *error = lastErrorMessage((DWORD)rc);
        return false;
    }
    if (on) {
        std::wstring cmd = runCommand();
        rc = RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                            (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, kRunValue);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;  // already off
    }
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        if (error) *error = lastErrorMessage((DWORD)rc);
        return false;
    }
    return true;
}

void migrateRunAtLogin() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                      &key) != ERROR_SUCCESS)
        return;
    wchar_t val[MAX_PATH * 2];
    DWORD sz = sizeof(val) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(key, kOldRunValue, nullptr, &type, (LPBYTE)val, &sz) ==
            ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        val[sz / sizeof(wchar_t)] = L'\0';
        // Only an entry that started the copy in this folder is ours to move:
        // the person upgraded this install and had run-at-sign-in on. One
        // for a copy kept somewhere else is left alone. The trailing slash
        // stops "...\MacroDeck" matching "...\MacroDeck2".
        if (icontains(narrow(val), exeDir() + "\\")) {
            std::wstring cmd = runCommand();
            if (RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                               (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) ==
                ERROR_SUCCESS)
                RegDeleteValueW(key, kOldRunValue);
        }
    }
    RegCloseKey(key);
}

std::string timeStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", st.wHour, st.wMinute,
                  st.wSecond);
    return buf;
}

std::string dateStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", st.wYear, st.wMonth,
                  st.wDay);
    return buf;
}

std::string lastErrorMessage(DWORD err) {
    if (!err) return "";
    LPWSTR msg = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&msg, 0, nullptr);
    std::string out = n ? trim(narrow(std::wstring(msg, n))) : "";
    if (msg) LocalFree(msg);
    if (out.empty()) out = "error " + std::to_string((unsigned)err);
    return out;
}
