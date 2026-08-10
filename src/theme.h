// Colours, DPI scaling and the drawing helpers the custom-painted UI uses.
#pragma once
#include "gdiplus_inc.h"

#include <string>

namespace ui {

struct Theme {
    Gdiplus::Color bg{255, 24, 26, 30};
    Gdiplus::Color panel{255, 32, 35, 41};
    Gdiplus::Color panelAlt{255, 41, 45, 53};
    Gdiplus::Color tile{255, 47, 52, 61};
    Gdiplus::Color tileHover{255, 60, 66, 77};
    Gdiplus::Color tileEmpty{255, 34, 37, 43};
    Gdiplus::Color border{255, 58, 63, 72};
    Gdiplus::Color borderSoft{255, 45, 49, 57};
    Gdiplus::Color text{255, 232, 234, 237};
    Gdiplus::Color textDim{255, 150, 157, 167};
    Gdiplus::Color textFaint{255, 108, 115, 125};
    Gdiplus::Color accent{255, 79, 140, 255};
    Gdiplus::Color accentDim{255, 42, 74, 134};
    Gdiplus::Color good{255, 55, 201, 120};
    Gdiplus::Color warn{255, 240, 178, 64};
    Gdiplus::Color danger{255, 255, 95, 86};
};

const Theme& theme();

// DPI. Set once at startup from the window's DPI.
void setDpiScale(float s);
float dpiScale();
int S(int px);          // scale a design-time pixel value
float Sf(float px);

COLORREF toColorRef(const Gdiplus::Color& c);
Gdiplus::Color fromHex(const std::string& hex, const Gdiplus::Color& fallback);
std::string toHex(const Gdiplus::Color& c);
Gdiplus::Color mix(const Gdiplus::Color& a, const Gdiplus::Color& b, float t);
Gdiplus::Color withAlpha(const Gdiplus::Color& c, int alpha);

// A tile colour that reads well behind white text.
Gdiplus::Color readableTileColor(const std::string& hex,
                                 const Gdiplus::Color& fallback);

// ------------------------------------------------------------------ shapes
void fillRounded(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                 const Gdiplus::Color& c);
void strokeRounded(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                   const Gdiplus::Color& c, float width = 1.0f);
void strokeRoundedDashed(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
                         float radius, const Gdiplus::Color& c, float width);

enum class Align { Left, Center, Right };

// Text. `size` is in design pixels and gets DPI-scaled.
void drawText(Gdiplus::Graphics& g, const std::wstring& text,
              const Gdiplus::RectF& box, float size, const Gdiplus::Color& c,
              Align align = Align::Left, bool bold = false,
              bool vcenter = true, bool wrap = false);
Gdiplus::SizeF measureText(Gdiplus::Graphics& g, const std::wstring& text,
                           float size, bool bold = false);

// One hand-drawn icon per action id - no icon fonts or image assets, so it
// renders identically on every machine and stays crisp at any DPI.
void drawActionIcon(Gdiplus::Graphics& g, const std::string& actionId,
                    const Gdiplus::RectF& box, const Gdiplus::Color& c);

void drawGlyphPlus(Gdiplus::Graphics& g, const Gdiplus::RectF& box,
                   const Gdiplus::Color& c, float thickness);
void drawGlyphChevron(Gdiplus::Graphics& g, const Gdiplus::RectF& box,
                      const Gdiplus::Color& c, bool down);
void drawGlyphPlay(Gdiplus::Graphics& g, const Gdiplus::RectF& box,
                   const Gdiplus::Color& c);
void drawGlyphStop(Gdiplus::Graphics& g, const Gdiplus::RectF& box,
                   const Gdiplus::Color& c);
void drawGlyphGear(Gdiplus::Graphics& g, const Gdiplus::RectF& box,
                   const Gdiplus::Color& c);
void drawGlyphKeyboard(Gdiplus::Graphics& g, const Gdiplus::RectF& box,
                       const Gdiplus::Color& c);

// The window/tray icon, drawn at runtime so the app ships without any binary
// image assets. Caller owns the HICON and must DestroyIcon it.
// The app mark, drawn into a square 32bpp bitmap. makeAppIcon() wraps this
// for the window and tray; tools/make_icon.cpp uses it to export src\app.ico,
// so the file on disk and the icon drawn at runtime cannot drift apart.
void drawAppIcon(Gdiplus::Bitmap& bmp, int size);
HICON makeAppIcon(int size);

}  // namespace ui
