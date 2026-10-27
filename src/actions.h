// Synthetic input (SendInput) and program launching.
#pragma once
#include <string>
#include <vector>
#include <windows.h>

namespace act {

// All of these throw std::runtime_error on failure so the engine can report
// which step of a macro went wrong.
void pressCombo(const std::string& combo, int holdMs = 0);
void typeText(const std::string& text, int intervalMs = 0);

void moveMouse(int x, int y);
void getMousePos(int& x, int& y);
void click(bool hasPos, int x, int y, const std::string& button = "left",
           bool doubleClick = false, bool restorePos = false);
void scroll(int notches);

void runCommand(const std::string& cmd, bool wait);
void openPath(const std::string& target);

// Typing, key presses and clicks check this first. Windows silently drops
// synthetic input sent to a program running at a higher integrity level -
// "as administrator" - than the sender, so the macro would carry on as if it
// had worked. Returns that program's path when `target` (or its top-level
// window) belongs to one; empty when input will get through, or when Windows
// won't say.
std::string blockedProgram(HWND target);

}  // namespace act
