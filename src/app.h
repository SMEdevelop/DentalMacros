#pragma once
#include <windows.h>

namespace ui {

// Creates the main window and runs the message loop. Returns the exit code.
int runApp(HINSTANCE instance, int showCmd);

}  // namespace ui
