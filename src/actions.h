// Synthetic input (SendInput) and program launching.
#pragma once
#include <string>
#include <vector>

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

}  // namespace act
