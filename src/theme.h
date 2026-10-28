// Colours, DPI scaling and the drawing helpers the custom-painted UI uses.
// Every value comes from the SMEdevelop design system (tokens.h).
#pragma once
#include "gdiplus_inc.h"
#include "tokens.h"

#include <string>

namespace ui {

inline Gdiplus::Color gc(const tk::Rgb& c) {
    return Gdiplus::Color(c.a, c.r, c.g, c.b);
}
// The design system's colours, ready for GDI+.
struct Tok {
    Gdiplus::Color surface, surfaceRaised, surfaceSunken, surfaceHover;
    Gdiplus::Color line, lineStrong, ink, inkMuted;
    Gdiplus::Color sky100, sky300, sand100, sand300, clay, clayInk;
    Gdiplus::Color primary, primaryHover, onPrimary, primaryTint, primaryInk;
    Gdiplus::Color focusRing;
    Gdiplus::Color success, onSuccess, successTint;
    Gdiplus::Color warning, warningTint, warningInk;
    Gdiplus::Color danger, onDanger, dangerTint, dangerInk;
    Gdiplus::Color overlay;
    Gdiplus::Color white;
};
const Tok& tok();

// The colour roles the screens were first written against, each now pointing
// at its design-system token (docs/REDESIGN.md section 1). Screens move to
// tok() names as they are redone.
struct Theme {
    Gdiplus::Color bg;          // surface
    Gdiplus::Color panel;       // surface-raised
    Gdiplus::Color panelAlt;    // surface-sunken
    Gdiplus::Color tile;        // the default key colour, walnut
    Gdiplus::Color tileHover;   // surface-hover
    Gdiplus::Color tileEmpty;   // surface
    Gdiplus::Color border;      // line-strong
    Gdiplus::Color borderSoft;  // line
    Gdiplus::Color text;        // ink
    Gdiplus::Color textDim;     // ink-muted
    Gdiplus::Color textFaint;   // ink-muted: nothing paler passes 5:1
    Gdiplus::Color accent;      // primary
    Gdiplus::Color accentDim;   // primary-tint
    Gdiplus::Color good;        // success
    Gdiplus::Color warn;        // warning-ink (it is used for text)
    Gdiplus::Color danger;      // danger
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

// What a key with this saved colour is drawn in: the nearest of the seven
// key colours (tokens.h). Empty means `fallback`.
Gdiplus::Color readableTileColor(const std::string& hex,
                                 const Gdiplus::Color& fallback);

// ------------------------------------------------------------------ shapes
void fillRounded(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                 const Gdiplus::Color& c);
void strokeRounded(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                   const Gdiplus::Color& c, float width = 1.0f);
void strokeRoundedDashed(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
                         float radius, const Gdiplus::Color& c, float width);

enum class Shadow { Soft, Hover, Bar, Sheet };
// A shadow-* token under the rounded rect `r`, drawn before the shape.
void paintShadow(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                 Shadow kind);

enum class Align { Left, Center, Right };

// Text. `size` is in design pixels and gets DPI-scaled.
void drawText(Gdiplus::Graphics& g, const std::wstring& text,
              const Gdiplus::RectF& box, float size, const Gdiplus::Color& c,
              Align align = Align::Left, bool bold = false,
              bool vcenter = true, bool wrap = false);
Gdiplus::SizeF measureText(Gdiplus::Graphics& g, const std::wstring& text,
                           float size, bool bold = false);
// Text in one of the design system's styles (size, weight; overline in
// capitals).
void drawStyled(Gdiplus::Graphics& g, const std::wstring& text,
                const Gdiplus::RectF& box, tk::Text style,
                const Gdiplus::Color& c, Align align = Align::Left,
                bool vcenter = true, bool wrap = false);
Gdiplus::SizeF measureStyled(Gdiplus::Graphics& g, const std::wstring& text,
                             tk::Text style, float wrapWidth = 0);
// The same with any size and weight, for controls whose font was set by
// size rather than by style.
void drawSpec(Gdiplus::Graphics& g, const std::wstring& text,
              const Gdiplus::RectF& box, const tk::TextSpec& font,
              const Gdiplus::Color& c, Align align = Align::Left,
              bool vcenter = true, bool wrap = false);
Gdiplus::SizeF measureSpec(Gdiplus::Graphics& g, const std::wstring& text,
                           const tk::TextSpec& font);

// ------------------------------------------------------------- components
// The design system's components, painted with GDI+. Rectangles are device
// pixels; sizes inside are design pixels scaled here.

enum class ButtonKind { Primary, Secondary, Quiet, Success, Danger };
struct ButtonState {
    bool hover = false;
    bool pressed = false;
    bool focused = false;    // keyboard focus: draws the focus ring
    bool disabled = false;
};
// `behind` is the colour around the button, for the corners and the gap
// inside the focus ring. `iconId` (optional) is an action or UI icon drawn at
// 22px before the label.
void paintButton(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
                 const std::wstring& label, ButtonKind kind,
                 const ButtonState& st, const Gdiplus::Color& behind,
                 const tk::TextSpec& font = tk::spec(tk::Text::Button),
                 const std::string& iconId = "");

// The keyboard focus ring, drawn inside `r` (a child window can't paint
// outside itself): a 3px ring at the edge, then a 2px gap. Returns the
// rect left for the control's own shape.
Gdiplus::RectF paintFocusRing(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
                              float radius, const Gdiplus::Color& behind);

// `part`: 0 a whole pill, 1 the first of several joined into one switch,
// 2 a middle one, 3 the last.
void paintPill(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
               const std::wstring& label, bool active, bool hover,
               bool focused, const Gdiplus::Color& behind,
               const std::string& iconId = "", int part = 0);

enum class BannerKind { Danger, Warning, Info };
// `reserveRight` keeps room at the right end for a button inside the banner.
void paintBanner(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
                 BannerKind kind, const std::wstring& text,
                 float reserveRight = 0);
// The banner's own background colour, for a button drawn on it.
Gdiplus::Color bannerFill(BannerKind kind);

// Walnut (or sage, once something is confirmed) pill with white text.
void paintToast(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
                const std::wstring& text, bool success);
// The width a toast needs for `text`, in device pixels.
float toastWidth(Gdiplus::Graphics& g, const std::wstring& text);

// "STEP 1 OF 3". Returns the badge's width in device pixels.
float paintStepBadge(Gdiplus::Graphics& g, float x, float y,
                     const std::wstring& text);

// A large tile you pick one of (RoomTile): round check, a sub-line, and the
// selected and away states.
struct ChoiceState {
    bool selected = false;
    bool hover = false;
    bool focused = false;
    bool away = false;       // dashed edge, warning-ink sub-line
};
void paintChoiceTile(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
                     const std::wstring& title, const std::wstring& sub,
                     const ChoiceState& st, const Gdiplus::Color& behind,
                     tk::Text titleStyle = tk::Text::TileLabel);

// Line icons on a 24px grid, 2px stroke: "alert", "info", "check", "close",
// "chevron-down", "plus". Unknown ids fall back to the action icons.
void drawUiIcon(Gdiplus::Graphics& g, const std::string& id,
                const Gdiplus::RectF& box, const Gdiplus::Color& c);

// One hand-drawn icon per action id - no icon fonts or image assets, so it
// renders identically on every machine and stays crisp at any DPI.
void drawActionIcon(Gdiplus::Graphics& g, const std::string& actionId,
                    const Gdiplus::RectF& box, const Gdiplus::Color& c);

void drawGlyphPlus(Gdiplus::Graphics& g, const Gdiplus::RectF& box,
                   const Gdiplus::Color& c, float thickness);
void drawGlyphChevron(Gdiplus::Graphics& g, const Gdiplus::RectF& box,
                      const Gdiplus::Color& c, bool down);
// Marks a tile whose wheel does something - two chevrons, up and down.
void drawGlyphDial(Gdiplus::Graphics& g, const Gdiplus::RectF& box,
                   const Gdiplus::Color& c);
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
