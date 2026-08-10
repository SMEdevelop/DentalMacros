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

// Paths are all resolved relative to the folder holding the .exe so the app
// behaves the same however it was launched (shortcut, Explorer, console).
std::string exePath();            // full path to MacroDeck.exe
std::string exeDir();
std::string appPath(const std::string& relative);
std::string joinPath(const std::string& a, const std::string& b);
std::string baseName(const std::string& path);
std::string stripExt(const std::string& path);
std::string relativeToApp(const std::string& absolute);

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

// Launch MacroDeck at sign-in (HKCU ...\Run). Reading never throws; writing
// reports why it failed.
bool runAtLoginEnabled();
bool setRunAtLogin(bool on, std::string* error = nullptr);

std::string timeStamp();          // "14:03:22"
std::string dateStamp();          // "2026-08-09"
std::string lastErrorMessage(DWORD err = GetLastError());
