// The SMEdevelop design system's values: colours, type, spacing, corners and
// tap sizes, named as in its tokens.json. Every screen takes its look from
// here and nowhere else.
//
// Pure data and arithmetic - no GDI+ - so the self-test can check the values
// and the contrast they promise.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tk {

struct Rgb {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

// Colours. Light only: the design system has no dark mode.
struct Colors {
    Rgb surface;          // page background, warm oat
    Rgb surfaceRaised;    // cards, panels, bars, dialogs
    Rgb surfaceSunken;    // quiet button fill, wells, empty slots
    Rgb surfaceHover;     // hover on anything sunken
    Rgb line;             // dividers, resting card outline (decoration only)
    Rgb lineStrong;       // edges of fields, checkboxes, outlined controls
    Rgb ink;              // all text
    Rgb inkMuted;         // secondary text
    Rgb sky100, sky300;   // light blue wash; 300 is decoration only
    Rgb sand100, sand300; // light brown wash; 300 is decoration only
    Rgb clay, clayInk;
    Rgb primary, primaryHover, onPrimary, primaryTint, primaryInk;
    Rgb focusRing;
    Rgb success, onSuccess, successTint;
    Rgb warning, warningTint, warningInk;
    Rgb danger, onDanger, dangerTint, dangerInk;
    Rgb overlay;          // warm dimmed backdrop, 40%
};
const Colors& colors();

// The seven colours a macro key, a floating button or a page may be. Nothing
// else: white text stays readable on every one of them.
struct KeyColor {
    const char* id;     // "walnut" - matches the msg-* token
    const char* name;   // "Walnut" - what the swatch is called on screen
    const char* hex;    // "#6f5a48"
    Rgb rgb;
};
const std::vector<KeyColor>& keyColors();
// Walnut: what a new key is, and what a key with no colour saved shows.
const KeyColor& defaultKeyColor();
// The key colour closest to a saved one. Files written before the redesign
// hold any colour at all; they are drawn as the nearest of the seven, and
// the file keeps its own value until someone picks a new one. Empty or
// unreadable means the default.
const KeyColor& nearestKeyColor(const std::string& hex);

// "#3d6b8e" -> {61,107,142}. False if it isn't a 3- or 6-digit hex colour.
bool parseHex(const std::string& hex, Rgb& out);
std::string toHex(const Rgb& c);

// WCAG contrast ratio of two opaque colours, 1.0 .. 21.0.
double contrast(const Rgb& a, const Rgb& b);

// Type. One family, Segoe UI.
enum class Text {
    Display,     // 44 / 50 / 800 - the one big message
    Title,       // 26 / 32 / 700 - screen and dialog headings
    TileLabel,   // 24 / 30 / 700 - choice tiles
    KeyLabel,    // 22 / 28 / 700 - macro key labels (MessageButton)
    Button,      // 18 / 24 / 600 - every button
    ButtonBig,   // 20 / 26 / 700 - the one big action
    Body,        // 17 / 26 / 400 - anything a person must read
    Label,       // 15 / 20 / 600 - field labels, sub-lines
    Overline,    // 13 / 18 / 700, capitals - step badges, section kickers
    // Advanced mode's own controls are compact by choice (docs/REDESIGN.md
    // section 6): the label size, at reading weight and with buttons' weight.
    Compact,     // 15 / 20 / 400
    CompactBold, // 15 / 20 / 600
};
struct TextSpec {
    int size;        // px
    int lineHeight;  // px
    int weight;      // 400, 600, 700, 800
    float tracking;  // em, overline only
};
TextSpec spec(Text t);

// Spacing steps.
constexpr int kSpace1 = 4;
constexpr int kSpace2 = 8;
constexpr int kSpace3 = 12;
constexpr int kSpace4 = 16;
constexpr int kSpace5 = 24;
constexpr int kSpace6 = 32;
constexpr int kSpace7 = 48;

// Corners.
constexpr int kRadiusSm = 8;     // key badges, small chips, list rows
constexpr int kRadiusMd = 16;    // buttons, keys, tiles, fields
constexpr int kRadiusLg = 24;    // sheets
constexpr int kRadiusPill = 999; // pills, toasts, step badges

// Tap sizes.
constexpr int kTargetMin = 48;   // the smallest anything clickable may be
constexpr int kTargetBig = 64;   // the main action on setup and prompts
constexpr int kTileMin = 220;
// Advanced mode's own rows and buttons (docs/REDESIGN.md section 6).
constexpr int kTargetCompact = 40;

constexpr int kOutline = 2;      // every outline
constexpr int kFocusRing = 3;    // keyboard focus ring
constexpr int kFocusGap = 2;     // between the ring and the control

}  // namespace tk
