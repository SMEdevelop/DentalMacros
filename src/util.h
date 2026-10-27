// Small shared helpers: string conversion, paths, files.
#pragma once
#include <string>
#include <vector>
#include <windows.h>

std::wstring widen(const std::string& s);
std::string  narrow(const std::wstring& s);

std::string trim(const std::string& s);
std::string lower(const std::string& s);
bool iequals(const std::string& a, const std::string& b);
bool icontains(const std::string& hay, const std::string& needle);
std::vector<std::string> split(const std::string& s, char sep);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);

// Paths are all resolved relative to a fixed folder so the app behaves the
// same however it was launched (shortcut, Explorer, console).
std::string exePath();            // full path to DentalMacros.exe
std::string exeDir();
// Beside the program itself. For the user's files - settings, backups,
// pictures, layouts - use dataPath() instead.
std::string appPath(const std::string& relative);
std::string joinPath(const std::string& a, const std::string& b);
std::string baseName(const std::string& path);
std::string stripExt(const std::string& path);
// The shortest way to store a path: relative to the data folder, or to the
// program's folder, when it is inside one of them; unchanged otherwise.
std::string relativeToApp(const std::string& absolute);

// ------------------------------------------------------------ data folder
// Where the user's files live: beside the program whenever that folder can
// be written - installed and portable copies alike, as it has always been -
// and otherwise %LOCALAPPDATA%\DentalMacros, so a copy run from a protected
// folder (Program Files, a locked-down share, a write-protected stick) still
// keeps its settings. Decided on first use; moveDataToProfile() can move it
// later in the run. Safe to call from any thread.
std::string dataDir();
std::string dataPath(const std::string& relative);
bool dataDirIsFallback();          // true when it is the profile folder
std::string profileDataDir();      // %LOCALAPPDATA%\DentalMacros, or ""

// A relative path the user stored ("images\\stamp.png"): found in the data
// folder first, then beside the program, where older setups kept them.
// Absolute paths come back unchanged; a file found nowhere resolves into the
// data folder.
std::string resolveDataPath(const std::string& relative);

// Switch to the profile folder for the rest of the run, after a save was
// refused. Copies settings, pictures and layouts across first if the profile
// folder has no settings yet. False if there is no profile folder or it
// can't be written either.
bool moveDataToProfile(std::string* error = nullptr);

// The decision itself, with the file system passed in so it can be tested.
struct DataDirProbe {
    bool (*canCreateIn)(const std::string& dir);
    bool (*exists)(const std::string& file);
    bool (*deniesWrites)(const std::string& file);
};
std::string chooseDataDir(const std::string& exeDir,
                          const std::string& profileDir,
                          const DataDirProbe& probe);
// Copy macros.json and the images and layouts folders from `from` to `to`,
// unless `to` already has a macros.json. Never overwrites anything.
bool migrateData(const std::string& from, const std::string& to);

// Can a new file be created in `dir`? Tries it.
bool canCreateIn(const std::string& dir);
// Is `file` there but refusing writes - read-only, or denied to this user?
// A file merely held open by another program is not refusing anything.
bool fileDeniesWrites(const std::string& file);

bool fileExists(const std::string& path);
bool dirExists(const std::string& path);
void ensureDir(const std::string& path);
// Distinguishes "there is no such file" from "the file is there but Windows
// would not open it" - the caller needs to react very differently to each.
enum class ReadResult { Ok, Missing, Unreadable };
ReadResult readFileEx(const std::string& path, std::string& out);
bool readFile(const std::string& path, std::string& out);
bool writeFileAtomic(const std::string& path, const std::string& data);
bool copyFile(const std::string& from, const std::string& to, bool overwrite);
bool deleteFile(const std::string& path);
// "x.json" -> "x.json.bak", or ".bak.2", ".bak.3"... if earlier ones exist, so
// a rescued file is never overwritten by the next rescue.
std::string freeBackupPath(const std::string& path);
std::vector<std::string> listFiles(const std::string& dir, const std::string& ext);

// Launch Dental Macros at sign-in (HKCU ...\Run). Reading never throws; writing
// reports why it failed.
bool runAtLoginEnabled();
bool setRunAtLogin(bool on, std::string* error = nullptr);
// Before the rename to Dental Macros, the Run entry was called "MacroDeck"
// and started MacroDeck.exe. Moves such an entry for this folder's copy over
// to the current name and exe. Called once at startup; does nothing at all
// when there is no old entry.
void migrateRunAtLogin();

std::string timeStamp();          // "14:03:22"
std::string dateStamp();          // "2026-08-09"
std::string lastErrorMessage(DWORD err = GetLastError());
