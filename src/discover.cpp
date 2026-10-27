#include "discover.h"

#include "actions.h"
#include "util.h"
#include "winman.h"

#include <algorithm>
#include <windows.h>

namespace discover {
namespace {

// Practice-management and imaging products, by the name their makers use.
// Kept to names distinctive enough not to turn up in unrelated software.
const char* const kProducts[] = {
    "Open Dental", "Dentrix",  "Eaglesoft",   "Easy Dental", "Curve Dental",
    "Denticon",    "Dexis",    "Carestream",  "Romexis",     "Sidexis",
    "XrayVision",  "VixWin",   "Practice-Web", "Apteryx",
};

// Lower-case letters and digits only.
std::string squash(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') out += (char)(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
    }
    return out;
}

void addOrUpdate(std::vector<DentalProgram>& found, const std::string& name,
                 bool running, bool elevated) {
    auto it = std::find_if(found.begin(), found.end(),
                           [&](const DentalProgram& p) { return p.name == name; });
    if (it == found.end()) {
        DentalProgram p;
        p.name = name;
        found.push_back(p);
        it = found.end() - 1;
    }
    it->running = it->running || running;
    it->elevated = it->elevated || elevated;
}

void fromUninstallKey(HKEY root, const wchar_t* path,
                      std::vector<DentalProgram>& found) {
    HKEY key;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t sub[256];
        DWORD len = 256;
        if (RegEnumKeyExW(key, i, sub, &len, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS)
            break;
        wchar_t name[512];
        DWORD bytes = sizeof(name);
        if (RegGetValueW(key, sub, L"DisplayName", RRF_RT_REG_SZ, nullptr, name,
                         &bytes) != ERROR_SUCCESS)
            continue;
        const std::string product = matchProduct(narrow(name));
        if (!product.empty()) addOrUpdate(found, product, false, false);
    }
    RegCloseKey(key);
}

}  // namespace

std::string matchProduct(const std::string& text) {
    const std::string hay = squash(text);
    if (hay.empty()) return "";
    for (const char* p : kProducts)
        if (hay.find(squash(p)) != std::string::npos) return p;
    return "";
}

std::vector<DentalProgram> findDentalPrograms() {
    std::vector<DentalProgram> found;
    const wchar_t* kUninstall =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    fromUninstallKey(HKEY_LOCAL_MACHINE, kUninstall, found);
    fromUninstallKey(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion"
                     L"\\Uninstall",
                     found);
    fromUninstallKey(HKEY_CURRENT_USER, kUninstall, found);

    // Open windows: the program's file name first - it can't carry a
    // patient's name the way a title can - then the title.
    for (const win::WindowInfo& w : win::listWindows()) {
        std::string product = matchProduct(w.exe);
        if (product.empty()) product = matchProduct(w.title);
        if (product.empty()) continue;
        addOrUpdate(found, product, true, !act::blockedProgram(w.hwnd).empty());
    }

    std::stable_sort(found.begin(), found.end(),
                     [](const DentalProgram& a, const DentalProgram& b) {
                         return a.running > b.running;
                     });
    return found;
}

}  // namespace discover
