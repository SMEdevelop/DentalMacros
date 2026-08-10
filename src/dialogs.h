#pragma once
#include "config.h"

#include <string>
#include <windows.h>

namespace ui {

struct KeyCaptureResult {
    bool ok = false;
    std::string keyName;      // canonical name, e.g. "num1"
    std::string combo;        // "ctrl+shift+t" when captured in combo mode
    std::string devicePath;   // device the key came from ("" if picked by name)
    std::string deviceLabel;
};

// Wait for a physical keypress. `restrictMatch` (optional) keeps only keys
// from a specific configured keyboard. `comboMode` records modifiers too.
KeyCaptureResult captureKey(HWND parent, const std::string& title,
                            const std::string& prompt,
                            const std::string& restrictMatch = "",
                            bool comboMode = false, bool allowPickByName = true);

// Scan attached keyboards and add one to the config. Returns the new alias.
bool addKeyboardDialog(HWND parent, cfg::Config& config, std::string& newAlias);

// Wait for a physical left mouse click anywhere on screen and report where it
// landed. Used to record a click's position while the target stays visible
// (the recording status window is small and does not cover the screen).
bool captureClick(HWND parent, const std::string& prompt, int& x, int& y);

// Build or edit one macro step.
bool editStepDialog(HWND parent, cfg::Step& step, const cfg::Settings& settings);

// Fullscreen snip: drag a box, save it as a PNG template.
// `outRelPath` receives the path relative to the exe folder.
bool snipTemplate(HWND parent, std::string& outRelPath);

bool settingsDialog(HWND parent, cfg::Settings& settings);

// Common file / colour pickers.
bool chooseImageFile(HWND parent, std::string& relPath);
bool chooseColorHex(HWND parent, std::string& hex);

}  // namespace ui
