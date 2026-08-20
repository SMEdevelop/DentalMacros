// A library of built-in tile icons, drawn in code rather than shipped as
// image files.
//
// Same reasoning as the action glyphs in theme.cpp: nothing to install beside
// the executable, nothing to lose when a folder is moved, identical on every
// machine, and crisp at any DPI or tile size. A PNG picked off disk is still
// allowed - this is the answer to "I want a printer icon and I do not want to
// go and find one".
//
// A binding stores a library icon as "lib:<id>" in its `icon` field, which is
// the same string field a file path lives in. Anything without the prefix is
// still treated as a path, so configs written before this existed are read
// exactly as they were.
#pragma once

#include "gdiplus_inc.h"

#include <string>
#include <vector>
#include <windows.h>

namespace ui {

struct LibIcon {
    const char* id;
    const char* label;
    const char* category;
};

// Every icon in the library, grouped by category in display order.
const std::vector<LibIcon>& iconLibrary();
const std::vector<std::string>& iconCategories();

// "lib:printer" -> true. A plain path -> false.
bool isLibraryIcon(const std::string& iconField);
// "lib:printer" -> "printer". Empty for anything that is not a library icon.
std::string libraryIconId(const std::string& iconField);
// "printer" -> "lib:printer"
std::string libraryIconRef(const std::string& id);
// Does the library actually contain this id? An id from a newer build must
// not be silently redrawn as something else.
bool libraryHasIcon(const std::string& id);

// Draws the icon into `box`. Returns false, having drawn nothing, if the id
// is not one this build knows.
bool drawLibraryIcon(Gdiplus::Graphics& g, const std::string& id,
                     const Gdiplus::RectF& box, const Gdiplus::Color& c);

// The icon picker. `iconField` carries the current value in and the chosen
// one out - a "lib:<id>" reference, a file path from the Browse button, or
// empty for no icon. Returns false if the user cancelled.
bool pickIconDialog(HWND parent, std::string& iconField);

}  // namespace ui
