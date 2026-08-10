// GDI+ is picky about include order and needs the min/max macros from
// windows.h, so every translation unit that touches it goes through here.
#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
