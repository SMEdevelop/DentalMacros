#include "theme.h"

#include "util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>

using namespace Gdiplus;

namespace ui {
namespace {
float g_dpi = kUiSize;

GraphicsPath* roundedPath(const RectF& r, float radius) {
    auto* p = new GraphicsPath();
    float d = radius * 2;
    if (d <= 0 || r.Width < d || r.Height < d) {
        p->AddRectangle(r);
        return p;
    }
    p->AddArc(r.X, r.Y, d, d, 180, 90);
    p->AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p->AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p->AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p->CloseFigure();
    return p;
}

const FontFamily& uiFamily() {
    // Segoe UI on every supported Windows; GDI+ falls back gracefully if a
    // machine somehow lacks it.
    static FontFamily* fam = [] {
        auto* f = new FontFamily(L"Segoe UI");
        if (f->GetLastStatus() != Ok) {
            delete f;
            f = new FontFamily(L"Tahoma");
        }
        return f;
    }();
    return *fam;
}

// The weights the design system uses, as GDI+ can draw them: Segoe UI has a
// Semibold face for 600; 700 and 800 both come out as its Bold.
const FontFamily& semiboldFamily() {
    static FontFamily* fam = [] {
        auto* f = new FontFamily(L"Segoe UI Semibold");
        if (f->GetLastStatus() != Ok) {
            delete f;
            f = nullptr;
        }
        return f;
    }();
    return fam ? *fam : uiFamily();
}

struct FontPick {
    const FontFamily* family;
    INT style;
};
FontPick pickFont(int weight) {
    if (weight >= 700) return {&uiFamily(), FontStyleBold};
    if (weight >= 600) {
        const FontFamily& s = semiboldFamily();
        if (&s != &uiFamily()) return {&s, FontStyleRegular};
        return {&uiFamily(), FontStyleBold};
    }
    return {&uiFamily(), FontStyleRegular};
}

std::wstring upper(const std::wstring& s) {
    std::wstring u = s;
    for (wchar_t& ch : u) ch = (wchar_t)towupper(ch);
    return u;
}

}  // namespace

const Tok& tok() {
    static const Tok t = [] {
        const tk::Colors& c = tk::colors();
        Tok o;
        o.surface = gc(c.surface);
        o.surfaceRaised = gc(c.surfaceRaised);
        o.surfaceSunken = gc(c.surfaceSunken);
        o.surfaceHover = gc(c.surfaceHover);
        o.line = gc(c.line);
        o.lineStrong = gc(c.lineStrong);
        o.ink = gc(c.ink);
        o.inkMuted = gc(c.inkMuted);
        o.sky100 = gc(c.sky100);
        o.sky300 = gc(c.sky300);
        o.sand100 = gc(c.sand100);
        o.sand300 = gc(c.sand300);
        o.clay = gc(c.clay);
        o.clayInk = gc(c.clayInk);
        o.primary = gc(c.primary);
        o.primaryHover = gc(c.primaryHover);
        o.onPrimary = gc(c.onPrimary);
        o.primaryTint = gc(c.primaryTint);
        o.primaryInk = gc(c.primaryInk);
        o.focusRing = gc(c.focusRing);
        o.success = gc(c.success);
        o.onSuccess = gc(c.onSuccess);
        o.successTint = gc(c.successTint);
        o.warning = gc(c.warning);
        o.warningTint = gc(c.warningTint);
        o.warningInk = gc(c.warningInk);
        o.danger = gc(c.danger);
        o.onDanger = gc(c.onDanger);
        o.dangerTint = gc(c.dangerTint);
        o.dangerInk = gc(c.dangerInk);
        o.overlay = gc(c.overlay);
        o.white = Color(255, 255, 255, 255);
        return o;
    }();
    return t;
}

const Theme& theme() {
    static const Theme t = [] {
        const Tok& k = tok();
        Theme o;
        o.bg = k.surface;
        o.panel = k.surfaceRaised;
        o.panelAlt = k.surfaceSunken;
        o.tile = gc(tk::defaultKeyColor().rgb);
        o.tileHover = k.surfaceHover;
        o.tileEmpty = k.surface;
        o.border = k.lineStrong;
        o.borderSoft = k.line;
        o.text = k.ink;
        o.textDim = k.inkMuted;
        o.textFaint = k.inkMuted;
        o.accent = k.primary;
        o.accentDim = k.primaryTint;
        o.good = k.success;
        o.warn = k.warningInk;
        o.danger = k.danger;
        return o;
    }();
    return t;
}

float g_rawDpi = 1.0f;
void setDpiScale(float s) {
    g_rawDpi = (s > 0.1f && s < 8.0f) ? s : 1.0f;
    g_dpi = g_rawDpi * kUiSize;
}
FullSize::FullSize() : saved(g_dpi) { g_dpi = g_rawDpi; }
FullSize::~FullSize() { g_dpi = saved; }
float dpiScale() { return g_dpi; }
int S(int px) { return (int)std::lround(px * g_dpi); }
float Sf(float px) { return px * g_dpi; }

COLORREF toColorRef(const Color& c) {
    return RGB(c.GetR(), c.GetG(), c.GetB());
}

Color fromHex(const std::string& hex, const Color& fallback) {
    std::string h = trim(hex);
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() == 3) {
        std::string e;
        for (char c : h) { e += c; e += c; }
        h = e;
    }
    if (h.size() != 6) return fallback;
    for (char c : h)
        if (!isxdigit((unsigned char)c)) return fallback;
    unsigned v = (unsigned)strtoul(h.c_str(), nullptr, 16);
    return Color(255, (BYTE)((v >> 16) & 0xFF), (BYTE)((v >> 8) & 0xFF),
                 (BYTE)(v & 0xFF));
}

std::string toHex(const Color& c) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.GetR(), c.GetG(),
                  c.GetB());
    return buf;
}

Color mix(const Color& a, const Color& b, float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    auto lerp = [&](BYTE x, BYTE y) {
        return (BYTE)std::lround(x + (y - x) * t);
    };
    return Color(lerp(a.GetA(), b.GetA()), lerp(a.GetR(), b.GetR()),
                 lerp(a.GetG(), b.GetG()), lerp(a.GetB(), b.GetB()));
}

Color withAlpha(const Color& c, int alpha) {
    return Color((BYTE)std::max(0, std::min(255, alpha)), c.GetR(), c.GetG(),
                 c.GetB());
}

Color readableTileColor(const std::string& hex, const Color& fallback) {
    if (trim(hex).empty()) return fallback;
    return gc(tk::nearestKeyColor(hex).rgb);
}

// ------------------------------------------------------------------ shapes
void fillRounded(Graphics& g, const RectF& r, float radius, const Color& c) {
    GraphicsPath* p = roundedPath(r, radius);
    SolidBrush b(c);
    g.FillPath(&b, p);
    delete p;
}

void strokeRounded(Graphics& g, const RectF& r, float radius, const Color& c,
                   float width) {
    RectF inset(r.X + width / 2, r.Y + width / 2, r.Width - width,
                r.Height - width);
    GraphicsPath* p = roundedPath(inset, std::max(0.0f, radius - width / 2));
    Pen pen(c, width);
    g.DrawPath(&pen, p);
    delete p;
}

void strokeRoundedDashed(Graphics& g, const RectF& r, float radius,
                         const Color& c, float width) {
    RectF inset(r.X + width / 2, r.Y + width / 2, r.Width - width,
                r.Height - width);
    GraphicsPath* p = roundedPath(inset, std::max(0.0f, radius - width / 2));
    Pen pen(c, width);
    REAL dashes[] = {4.0f, 3.5f};
    pen.SetDashPattern(dashes, 2);
    g.DrawPath(&pen, p);
    delete p;
}

void paintShadow(Graphics& g, const RectF& r, float radius, Shadow kind) {
    // GDI+ has no blur, so each shadow-* layer is a stack of rounded rects
    // growing outwards, each faint, which adds up to a soft falloff.
    struct Layer { float dy, blur; Color c; int alpha; };
    Layer layers[2];
    int n = 0;
    switch (kind) {
        case Shadow::Soft:   // 0 1px 2px .06, 0 2px 8px .05 (warm brown)
            layers[n++] = {1, 2, Color(255, 58, 48, 39), 15};
            layers[n++] = {2, 8, Color(255, 58, 48, 39), 13};
            break;
        case Shadow::Hover:  // 0 4px 16px .16 (denim)
            layers[n++] = {4, 16, Color(255, 61, 107, 142), 41};
            break;
        case Shadow::Bar:    // 0 -6px 24px .08
            layers[n++] = {-6, 24, Color(255, 58, 48, 39), 20};
            break;
        case Shadow::Sheet:  // 0 16px 48px .20
            layers[n++] = {16, 48, Color(255, 58, 48, 39), 51};
            break;
    }
    for (int li = 0; li < n; ++li) {
        const Layer& L = layers[li];
        const float blur = Sf(L.blur);
        const int steps = std::max(2, std::min(12, (int)(blur / 2)));
        const int per = std::max(1, L.alpha / steps);
        for (int i = steps; i >= 1; --i) {
            const float grow = blur / 2 * (float)i / steps;
            RectF s(r.X - grow, r.Y - grow + Sf(L.dy), r.Width + grow * 2,
                    r.Height + grow * 2);
            fillRounded(g, s, radius + grow, withAlpha(L.c, per));
        }
    }
}

// ------------------------------------------------------------------- text
SizeF measureText(Graphics& g, const std::wstring& text, float size, bool bold) {
    Font font(&uiFamily(), Sf(size), bold ? FontStyleBold : FontStyleRegular,
              UnitPixel);
    RectF bounds;
    StringFormat fmt(StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoClip);
    g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &fmt, &bounds);
    return SizeF(bounds.Width, bounds.Height);
}

void drawText(Graphics& g, const std::wstring& text, const RectF& box,
              float size, const Color& c, Align align, bool bold, bool vcenter,
              bool wrap) {
    if (text.empty()) return;
    Font font(&uiFamily(), Sf(size), bold ? FontStyleBold : FontStyleRegular,
              UnitPixel);
    SolidBrush brush(c);
    StringFormat fmt;
    fmt.SetAlignment(align == Align::Left     ? StringAlignmentNear
                     : align == Align::Center ? StringAlignmentCenter
                                              : StringAlignmentFar);
    fmt.SetLineAlignment(vcenter ? StringAlignmentCenter : StringAlignmentNear);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    if (!wrap) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(text.c_str(), -1, &font, box, &fmt, &brush);
}

void drawStyled(Graphics& g, const std::wstring& textIn, const RectF& box,
                tk::Text style, const Color& c, Align align, bool vcenter,
                bool wrap) {
    if (textIn.empty()) return;
    drawSpec(g, style == tk::Text::Overline ? upper(textIn) : textIn, box,
             tk::spec(style), c, align, vcenter, wrap);
}

SizeF measureSpec(Graphics& g, const std::wstring& text,
                  const tk::TextSpec& sp) {
    const FontPick fp = pickFont(sp.weight);
    Font font(fp.family, Sf((float)sp.size), fp.style, UnitPixel);
    RectF bounds;
    StringFormat fmt(StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoClip |
                       StringFormatFlagsMeasureTrailingSpaces);
    g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &fmt, &bounds);
    return SizeF(bounds.Width, bounds.Height);
}

void drawSpec(Graphics& g, const std::wstring& text, const RectF& box,
              const tk::TextSpec& sp, const Color& c, Align align, bool vcenter,
              bool wrap) {
    if (text.empty()) return;
    const FontPick fp = pickFont(sp.weight);
    Font font(fp.family, Sf((float)sp.size), fp.style, UnitPixel);
    SolidBrush brush(c);
    StringFormat fmt;
    fmt.SetAlignment(align == Align::Left     ? StringAlignmentNear
                     : align == Align::Center ? StringAlignmentCenter
                                              : StringAlignmentFar);
    fmt.SetLineAlignment(vcenter ? StringAlignmentCenter : StringAlignmentNear);
    fmt.SetTrimming(wrap ? StringTrimmingWord : StringTrimmingEllipsisCharacter);
    if (!wrap) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(text.c_str(), -1, &font, box, &fmt, &brush);
}

SizeF measureStyled(Graphics& g, const std::wstring& textIn, tk::Text style,
                    float wrapWidth) {
    const tk::TextSpec sp = tk::spec(style);
    const std::wstring text = style == tk::Text::Overline ? upper(textIn) : textIn;
    const FontPick fp = pickFont(sp.weight);
    Font font(fp.family, Sf((float)sp.size), fp.style, UnitPixel);
    RectF bounds;
    if (wrapWidth > 0) {
        StringFormat fmt;
        g.MeasureString(text.c_str(), -1, &font, RectF(0, 0, wrapWidth, 100000),
                        &fmt, &bounds);
    } else {
        StringFormat fmt(StringFormat::GenericTypographic());
        fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoClip |
                           StringFormatFlagsMeasureTrailingSpaces);
        g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &fmt, &bounds);
    }
    return SizeF(bounds.Width, bounds.Height);
}

// ------------------------------------------------------------- components
RectF paintFocusRing(Graphics& g, const RectF& r, float radius,
                     const Color& behind) {
    const float ring = Sf((float)tk::kFocusRing);
    const float gap = Sf((float)tk::kFocusGap);
    fillRounded(g, r, radius, behind);
    strokeRounded(g, r, radius, tok().focusRing, ring);
    const float in = ring + gap;
    return RectF(r.X + in, r.Y + in, r.Width - in * 2, r.Height - in * 2);
}

namespace {

RectF shrunk(const RectF& r, float f) {
    const float dw = r.Width * (1 - f), dh = r.Height * (1 - f);
    return RectF(r.X + dw / 2, r.Y + dh / 2, r.Width - dw, r.Height - dh);
}

// An icon, then the label, centred together in `r`.
void iconAndLabel(Graphics& g, const RectF& r, const std::wstring& label,
                  const std::string& iconId, const tk::TextSpec& font,
                  const Color& c, float iconPx = 22) {
    if (iconId.empty()) {
        drawSpec(g, label, r, font, c, Align::Center);
        return;
    }
    const float isz = Sf(iconPx), gap = label.empty() ? 0 : Sf(8);
    // GDI+ draws a little wider than it measures, so allow for it, or the
    // last letters turn into an ellipsis.
    const float slack = Sf(6);
    const float tw = label.empty() ? 0 : measureSpec(g, label, font).Width + slack;
    const float total = std::min(r.Width, isz + gap + tw);
    const float x0 = r.X + (r.Width - total) / 2;
    drawUiIcon(g, iconId, RectF(x0, r.Y + (r.Height - isz) / 2, isz, isz), c);
    if (!label.empty())
        drawSpec(g, label,
                 RectF(x0 + isz + gap - slack / 2, r.Y,
                       r.GetRight() - (x0 + isz + gap) + slack, r.Height),
                 font, c, Align::Left);
}

}  // namespace

void paintButton(Graphics& g, const RectF& rIn, const std::wstring& label,
                 ButtonKind kind, const ButtonState& st, const Color& behind,
                 const tk::TextSpec& style, const std::string& iconId) {
    const Tok& k = tok();
    fillRounded(g, rIn, 0, behind);
    float radius = std::min(Sf((float)tk::kRadiusMd), rIn.Height / 2);
    RectF r = rIn;
    if (st.focused && !st.disabled) {
        r = paintFocusRing(g, rIn, radius, behind);
        radius = std::max(0.0f, radius - (rIn.Height - r.Height) / 2);
    }
    if (st.pressed && !st.disabled) r = shrunk(r, 0.97f);

    Color fill, text, edge(0, 0, 0, 0);
    if (st.disabled) {
        fill = k.surfaceSunken;
        text = k.inkMuted;
    } else {
        switch (kind) {
            case ButtonKind::Primary:
                fill = st.hover || st.pressed ? k.primaryHover : k.primary;
                text = k.onPrimary;
                break;
            case ButtonKind::Secondary:
                fill = st.hover || st.pressed ? k.primaryTint : k.surfaceRaised;
                text = k.primaryInk;
                edge = k.primary;
                break;
            case ButtonKind::Quiet:
                fill = st.hover || st.pressed ? k.surfaceHover : k.surfaceSunken;
                text = k.ink;
                break;
            case ButtonKind::Success:
                fill = st.hover || st.pressed
                           ? mix(k.success, Color(255, 0, 0, 0), 0.10f)
                           : k.success;
                text = k.onSuccess;
                break;
            case ButtonKind::Danger:
                fill = st.hover || st.pressed ? k.danger : k.dangerTint;
                text = st.hover || st.pressed ? k.onDanger : k.dangerInk;
                break;
        }
    }
    fillRounded(g, r, radius, fill);
    if (edge.GetA())
        strokeRounded(g, r, radius, edge, Sf((float)tk::kOutline));
    // The label's own room: the design system pads buttons 24px a side, but
    // a fixed-width button in a row of three needs its label more than its
    // padding, so the label may run to 8px from the edge.
    RectF inner(r.X + Sf(8), r.Y, r.Width - Sf(16), r.Height);
    iconAndLabel(g, inner, label, iconId, style, text);
}

void paintPill(Graphics& g, const RectF& rIn, const std::wstring& label,
               bool active, bool hover, bool focused, const Color& behind,
               const std::string& iconId, int part) {
    const Tok& k = tok();
    fillRounded(g, rIn, 0, behind);
    float radius = rIn.Height / 2;
    RectF r = rIn;
    if (focused) {
        r = paintFocusRing(g, rIn, radius, behind);
        radius = r.Height / 2;
    }
    const Color fill = active ? k.primaryTint : k.surfaceRaised;
    const Color edge = active || hover ? k.primary : k.lineStrong;
    const float ow = Sf((float)tk::kOutline);
    if (part == 0) {
        fillRounded(g, r, radius, fill);
        strokeRounded(g, r, radius, edge, ow);
    } else {
        // One segment of a joined switch: the shape runs on past the sides
        // that join a neighbour and is cut off there, so only the outer ends
        // are round. The neighbours share one 2px line where they meet.
        const bool roundLeft = part == 1, roundRight = part == 3;
        RectF shape = r;
        const float over = radius + ow;
        if (!roundLeft) { shape.X -= over; shape.Width += over; }
        if (!roundRight) shape.Width += over;
        Region old;
        g.GetClip(&old);
        g.SetClip(r);
        fillRounded(g, shape, radius, fill);
        strokeRounded(g, shape, radius, edge, ow);
        g.SetClip(&old);
        if (!roundRight) {
            Pen join(edge, ow);
            const float x = r.GetRight() - ow / 2;
            g.DrawLine(&join, x, r.Y, x, r.GetBottom());
        }
    }
    const Color text = active || hover ? k.primaryInk : k.ink;
    // 18px a side on a pill of its own; a segment of a switch is sized to
    // share its row, so it keeps less.
    const float side = part == 0 ? Sf(18) : Sf(8);
    RectF inner(r.X + side, r.Y, r.Width - side * 2, r.Height);
    // Pills are 16px / 700 (bundle.css .sme-pill).
    if (iconId.empty()) {
        drawText(g, label, inner, 16, text, Align::Center, true);
    } else {
        const float isz = Sf(18), gap = Sf(8);
        const float tw = measureText(g, label, 16, true).Width;
        const float x0 = inner.X + std::max(0.0f, (inner.Width - isz - gap - tw) / 2);
        drawUiIcon(g, iconId, RectF(x0, r.Y + (r.Height - isz) / 2, isz, isz),
                   text);
        drawText(g, label,
                 RectF(x0 + isz + gap, r.Y, inner.GetRight() - x0 - isz - gap,
                       r.Height),
                 16, text, Align::Left, true);
    }
}

Color bannerFill(BannerKind kind) {
    switch (kind) {
        case BannerKind::Danger: return tok().dangerTint;
        case BannerKind::Warning: return tok().warningTint;
        default: return tok().primaryTint;
    }
}

void paintBanner(Graphics& g, const RectF& r, BannerKind kind,
                 const std::wstring& text, float reserveRight) {
    const Tok& k = tok();
    Color bg, edge(0, 0, 0, 0), icon;
    const char* iconId = "alert";
    switch (kind) {
        case BannerKind::Danger:
            bg = k.dangerTint; edge = k.danger; icon = k.dangerInk;
            break;
        case BannerKind::Warning:
            bg = k.warningTint; edge = k.warning; icon = k.warningInk;
            break;
        case BannerKind::Info:
            bg = k.primaryTint; icon = k.primaryInk; iconId = "info";
            break;
    }
    const float radius = std::min(Sf((float)tk::kRadiusMd), r.Height / 2);
    fillRounded(g, r, radius, bg);
    if (edge.GetA()) strokeRounded(g, r, radius, edge, Sf((float)tk::kOutline));
    const float isz = Sf(22);
    const float x = r.X + Sf((float)tk::kSpace4);
    drawUiIcon(g, iconId, RectF(x, r.Y + (r.Height - isz) / 2, isz, isz), icon);
    const float tx = x + isz + Sf((float)tk::kSpace3);
    // bundle.css .sme-banner: 700 17px / 24px.
    drawText(g, text,
             RectF(tx, r.Y,
                   r.GetRight() - Sf((float)tk::kSpace4) - reserveRight - tx,
                   r.Height),
             17, k.ink, Align::Left, true, true, true);
}

float toastWidth(Graphics& g, const std::wstring& text) {
    // Measured in the font it is drawn in, plus the padding either side and
    // the bit GDI+ adds around text it draws.
    return measureText(g, text, 17, true).Width + Sf(48) + Sf(10);
}

void paintToast(Graphics& g, const RectF& r, const std::wstring& text,
                bool success) {
    paintShadow(g, r, r.Height / 2, Shadow::Sheet);
    fillRounded(g, r, r.Height / 2,
                success ? tok().success : gc(tk::defaultKeyColor().rgb));
    // bundle.css .sme-toast: 600 17px / 24px, white.
    drawText(g, text, RectF(r.X + Sf(24), r.Y, r.Width - Sf(48), r.Height), 17,
             tok().white, Align::Center, true);
}

float paintStepBadge(Graphics& g, float x, float y, const std::wstring& text) {
    const float w = measureStyled(g, text, tk::Text::Overline).Width + Sf(24);
    const float h = Sf(26);
    RectF r(x, y, w, h);
    fillRounded(g, r, h / 2, tok().primaryTint);
    drawStyled(g, text, r, tk::Text::Overline, tok().primaryInk, Align::Center);
    return w;
}

void paintChoiceTile(Graphics& g, const RectF& rIn, const std::wstring& title,
                     const std::wstring& sub, const ChoiceState& st,
                     const Color& behind, tk::Text titleStyle) {
    const Tok& k = tok();
    fillRounded(g, rIn, 0, behind);
    float radius = Sf((float)tk::kRadiusMd);
    RectF r = rIn;
    if (st.focused) {
        r = paintFocusRing(g, rIn, radius, behind);
        radius = std::max(0.0f, radius - (rIn.Height - r.Height) / 2);
    }
    const bool lifted = st.selected || st.hover;
    paintShadow(g, r, radius, lifted ? Shadow::Hover : Shadow::Soft);
    fillRounded(g, r, radius, st.selected ? k.primaryTint : k.surfaceRaised);
    const Color edge = lifted ? k.primary : (st.away ? k.lineStrong : k.line);
    if (st.away && !lifted)
        strokeRoundedDashed(g, r, radius, edge, Sf((float)tk::kOutline));
    else
        strokeRounded(g, r, radius, edge, Sf((float)tk::kOutline));

    // A short tile is a row in a list: the check on the left, the words
    // beside it. A tall one is a tile: the check in the corner, the words
    // centred under it.
    const bool row = rIn.Height < Sf(100);

    // The round check: empty, or filled with a tick. The tick, not the
    // colour, is what says "selected".
    const float d = Sf(26);
    RectF check = row ? RectF(r.X + Sf(16), r.Y + (r.Height - d) / 2, d, d)
                      : RectF(r.X + Sf(12), r.Y + Sf(10), d, d);
    if (st.selected) {
        SolidBrush b(k.primary);
        g.FillEllipse(&b, check);
        RectF tick(check.X + Sf(4), check.Y + Sf(4), d - Sf(8), d - Sf(8));
        drawUiIcon(g, "check", tick, k.onPrimary);
    } else {
        SolidBrush b(k.surfaceRaised);
        g.FillEllipse(&b, check);
        Pen p(k.lineStrong, Sf(2));
        g.DrawEllipse(&p, RectF(check.X + Sf(1), check.Y + Sf(1), d - Sf(2),
                                d - Sf(2)));
    }

    const float titleH = Sf((float)tk::spec(titleStyle).lineHeight);
    if (row) {
        const float tx = check.GetRight() + Sf(16);
        const float tw = r.GetRight() - Sf(16) - tx;
        const float subH = sub.empty() ? 0 : Sf(20);
        const float block = titleH + (sub.empty() ? 0 : Sf(2) + subH);
        const float top = r.Y + (r.Height - block) / 2;
        drawStyled(g, title, RectF(tx, top, tw, titleH), titleStyle,
                   st.away ? k.inkMuted : k.ink, Align::Left);
        if (!sub.empty())
            drawText(g, sub, RectF(tx, top + titleH + Sf(2), tw, subH), 15,
                     st.away ? k.warningInk : k.inkMuted, Align::Left, st.away);
        return;
    }
    // The sub-line may run to a second or third line on a tall tile.
    const float subW = r.Width - Sf(32);
    float subH = 0;
    if (!sub.empty()) {
        const float lines = std::max(1.0f, std::min(3.0f, std::ceil(
            measureStyled(g, sub, tk::Text::Label).Width / std::max(1.0f, subW - Sf(8)))));
        subH = std::min(lines * Sf(20), r.Height - titleH - Sf(28));
        subH = std::max(subH, Sf(20));
    }
    const float block = titleH + (sub.empty() ? 0 : Sf(6) + subH);
    // Centred, but never up beside the check in the corner, which a long
    // title would run into.
    const float top =
        std::max(r.Y + (r.Height - block) / 2, check.GetBottom() + Sf(2));
    // The check sits in the top corner, clear of the centred title.
    RectF tr(r.X + Sf(16), top, r.Width - Sf(32), titleH);
    drawStyled(g, title, tr, titleStyle, st.away ? k.inkMuted : k.ink,
               Align::Center);
    if (!sub.empty()) {
        RectF sr(r.X + Sf(16), top + titleH + Sf(6), subW, subH + Sf(2));
        if (st.away)
            drawText(g, sub, sr, 15, k.warningInk, Align::Center, true, false, true);
        else
            drawStyled(g, sub, sr, tk::Text::Label, k.inkMuted, Align::Center,
                       false, true);
    }
}

// ------------------------------------------------------------------ icons
namespace {

struct U {  // maps unit coords (0..1) inside a box to device coords
    RectF b;
    PointF operator()(float x, float y) const {
        return PointF(b.X + x * b.Width, b.Y + y * b.Height);
    }
    RectF rect(float x, float y, float w, float h) const {
        return RectF(b.X + x * b.Width, b.Y + y * b.Height, w * b.Width,
                     h * b.Height);
    }
    float sx(float v) const { return v * b.Width; }
};

void lineTo(Graphics& g, Pen& p, const U& u, float x1, float y1, float x2,
            float y2) {
    g.DrawLine(&p, u(x1, y1), u(x2, y2));
}

void windowFrame(Graphics& g, Pen& p, const U& u, const Color& c) {
    RectF r = u.rect(0.10f, 0.16f, 0.80f, 0.68f);
    g.DrawRectangle(&p, r);
    SolidBrush b(c);
    g.FillRectangle(&b, RectF(r.X, r.Y, r.Width, u.sx(0.14f)));
}

}  // namespace

void drawUiIcon(Graphics& g, const std::string& id, const RectF& box,
                const Color& c) {
    // Drawn on the design system's 24-unit grid with a 2-unit stroke, round
    // caps and joins.
    const float s = std::min(box.Width, box.Height) / 24.0f;
    const float ox = box.X + (box.Width - s * 24) / 2;
    const float oy = box.Y + (box.Height - s * 24) / 2;
    auto P = [&](float x, float y) { return PointF(ox + x * s, oy + y * s); };
    Pen p(c, std::max(1.0f, 2.0f * s));
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    p.SetLineJoin(LineJoinRound);
    SolidBrush b(c);
    auto dot = [&](float x, float y) {
        const float r = 1.3f * s;
        g.FillEllipse(&b, RectF(ox + x * s - r, oy + y * s - r, r * 2, r * 2));
    };

    if (id == "alert") {
        PointF tri[3] = {P(12, 3.5f), P(21.5f, 20), P(2.5f, 20)};
        g.DrawPolygon(&p, tri, 3);
        g.DrawLine(&p, P(12, 9.5f), P(12, 13.5f));
        dot(12, 16.8f);
        return;
    }
    if (id == "info") {
        g.DrawEllipse(&p, RectF(ox + 2.5f * s, oy + 2.5f * s, 19 * s, 19 * s));
        g.DrawLine(&p, P(12, 11), P(12, 16.5f));
        dot(12, 7.8f);
        return;
    }
    if (id == "check") {
        PointF pts[3] = {P(5, 12.5f), P(10, 17.5f), P(19, 7)};
        g.DrawLines(&p, pts, 3);
        return;
    }
    if (id == "close") {
        g.DrawLine(&p, P(6, 6), P(18, 18));
        g.DrawLine(&p, P(18, 6), P(6, 18));
        return;
    }
    if (id == "chevron-down") {
        PointF pts[3] = {P(6, 9), P(12, 15), P(18, 9)};
        g.DrawLines(&p, pts, 3);
        return;
    }
    if (id == "plus") {
        g.DrawLine(&p, P(12, 5), P(12, 19));
        g.DrawLine(&p, P(5, 12), P(19, 12));
        return;
    }
    drawActionIcon(g, id, box, c);
}

void drawActionIcon(Graphics& g, const std::string& id, const RectF& box,
                    const Color& c) {
    U u{box};
    float w = std::max(1.4f, box.Width * 0.075f);
    Pen p(c, w);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    p.SetLineJoin(LineJoinRound);
    SolidBrush brush(c);

    auto dot = [&](float x, float y, float r) {
        g.FillEllipse(&brush, RectF(u(x, y).X - u.sx(r), u(x, y).Y - u.sx(r),
                                    u.sx(r * 2), u.sx(r * 2)));
    };

    if (id == "type") {
        // A capital T plus a text caret
        lineTo(g, p, u, 0.16f, 0.24f, 0.68f, 0.24f);
        lineTo(g, p, u, 0.42f, 0.24f, 0.42f, 0.78f);
        Pen thin(c, std::max(1.0f, w * 0.7f));
        lineTo(g, thin, u, 0.82f, 0.30f, 0.82f, 0.74f);
        return;
    }
    if (id == "keys") {
        RectF r = u.rect(0.08f, 0.26f, 0.84f, 0.48f);
        g.DrawRectangle(&p, r);
        for (int i = 0; i < 4; ++i) dot(0.20f + i * 0.20f, 0.42f, 0.035f);
        lineTo(g, p, u, 0.28f, 0.62f, 0.72f, 0.62f);
        return;
    }
    if (id == "click" || id == "scroll") {
        RectF body = u.rect(0.28f, 0.16f, 0.44f, 0.68f);
        GraphicsPath* rp = roundedPath(body, body.Width * 0.5f);
        g.DrawPath(&p, rp);
        delete rp;
        lineTo(g, p, u, 0.50f, 0.16f, 0.50f, 0.42f);
        if (id == "scroll") {
            lineTo(g, p, u, 0.86f, 0.34f, 0.94f, 0.26f);
            lineTo(g, p, u, 0.94f, 0.26f, 1.02f, 0.34f);
        }
        return;
    }
    if (id == "move_mouse") {
        lineTo(g, p, u, 0.5f, 0.12f, 0.5f, 0.88f);
        lineTo(g, p, u, 0.12f, 0.5f, 0.88f, 0.5f);
        lineTo(g, p, u, 0.5f, 0.12f, 0.40f, 0.24f);
        lineTo(g, p, u, 0.5f, 0.12f, 0.60f, 0.24f);
        lineTo(g, p, u, 0.88f, 0.5f, 0.76f, 0.40f);
        lineTo(g, p, u, 0.88f, 0.5f, 0.76f, 0.60f);
        return;
    }
    if (id == "click_image" || id == "wait_image") {
        RectF r = u.rect(0.10f, 0.18f, 0.68f, 0.56f);
        g.DrawRectangle(&p, r);
        dot(0.28f, 0.34f, 0.05f);
        // "mountain" inside the frame
        PointF pts[3] = {u(0.16f, 0.68f), u(0.36f, 0.44f), u(0.62f, 0.68f)};
        g.DrawLines(&p, pts, 3);
        if (id == "click_image") {
            // pointer arrow at the corner
            PointF arrow[4] = {u(0.62f, 0.56f), u(0.62f, 0.96f), u(0.72f, 0.84f),
                               u(0.86f, 0.84f)};
            g.FillPolygon(&brush, arrow, 4);
        } else {
            g.DrawEllipse(&p, u.rect(0.60f, 0.56f, 0.34f, 0.34f));
            lineTo(g, p, u, 0.77f, 0.66f, 0.77f, 0.74f);
            lineTo(g, p, u, 0.77f, 0.74f, 0.85f, 0.74f);
        }
        return;
    }
    if (id == "run") {
        RectF r = u.rect(0.10f, 0.18f, 0.80f, 0.64f);
        GraphicsPath* rp = roundedPath(r, u.sx(0.08f));
        g.DrawPath(&p, rp);
        delete rp;
        PointF chev[3] = {u(0.26f, 0.38f), u(0.40f, 0.50f), u(0.26f, 0.62f)};
        g.DrawLines(&p, chev, 3);
        lineTo(g, p, u, 0.50f, 0.64f, 0.72f, 0.64f);
        return;
    }
    if (id == "open") {
        PointF folder[6] = {u(0.10f, 0.76f), u(0.10f, 0.26f), u(0.40f, 0.26f),
                            u(0.48f, 0.38f), u(0.90f, 0.38f), u(0.90f, 0.76f)};
        g.DrawPolygon(&p, folder, 6);
        return;
    }
    if (id == "sleep") {
        g.DrawEllipse(&p, u.rect(0.14f, 0.14f, 0.72f, 0.72f));
        lineTo(g, p, u, 0.50f, 0.32f, 0.50f, 0.52f);
        lineTo(g, p, u, 0.50f, 0.52f, 0.66f, 0.60f);
        return;
    }
    if (id == "window_focus") {
        windowFrame(g, p, u, c);
        return;
    }
    if (id == "window_move") {
        windowFrame(g, p, u, c);
        lineTo(g, p, u, 0.32f, 0.56f, 0.68f, 0.56f);
        lineTo(g, p, u, 0.32f, 0.56f, 0.40f, 0.48f);
        lineTo(g, p, u, 0.32f, 0.56f, 0.40f, 0.64f);
        lineTo(g, p, u, 0.68f, 0.56f, 0.60f, 0.48f);
        lineTo(g, p, u, 0.68f, 0.56f, 0.60f, 0.64f);
        return;
    }
    if (id == "window_minimize") {
        windowFrame(g, p, u, c);
        lineTo(g, p, u, 0.30f, 0.66f, 0.70f, 0.66f);
        return;
    }
    if (id == "window_maximize") {
        windowFrame(g, p, u, c);
        g.DrawRectangle(&p, u.rect(0.26f, 0.44f, 0.48f, 0.28f));
        return;
    }
    if (id == "window_save") {
        windowFrame(g, p, u, c);
        lineTo(g, p, u, 0.50f, 0.42f, 0.50f, 0.70f);
        lineTo(g, p, u, 0.50f, 0.70f, 0.40f, 0.60f);
        lineTo(g, p, u, 0.50f, 0.70f, 0.60f, 0.60f);
        return;
    }
    if (id == "window_restore") {
        windowFrame(g, p, u, c);
        lineTo(g, p, u, 0.50f, 0.70f, 0.50f, 0.42f);
        lineTo(g, p, u, 0.50f, 0.42f, 0.40f, 0.52f);
        lineTo(g, p, u, 0.50f, 0.42f, 0.60f, 0.52f);
        return;
    }

    if (id == "page_goto") {
        // Two stacked cards with an arrow across the front one: the page you
        // are on, another behind it, and the move between them.
        Pen thin(c, std::max(1.0f, w * 0.75f));
        g.DrawRectangle(&thin, u.rect(0.34f, 0.16f, 0.50f, 0.40f));
        g.DrawRectangle(&p, u.rect(0.16f, 0.40f, 0.50f, 0.42f));
        lineTo(g, p, u, 0.28f, 0.61f, 0.54f, 0.61f);
        lineTo(g, p, u, 0.44f, 0.51f, 0.54f, 0.61f);
        lineTo(g, p, u, 0.44f, 0.71f, 0.54f, 0.61f);
        return;
    }

    if (id == "page_hold") {
        // The same two cards, with a finger pressing the front one and
        // staying there - the layer lasts exactly as long as the press.
        Pen thin(c, std::max(1.0f, w * 0.75f));
        g.DrawRectangle(&thin, u.rect(0.34f, 0.14f, 0.50f, 0.38f));
        g.DrawRectangle(&p, u.rect(0.16f, 0.38f, 0.50f, 0.42f));
        lineTo(g, p, u, 0.41f, 0.46f, 0.41f, 0.60f);
        dot(0.41f, 0.67f, 0.075f);
        return;
    }

    // Unknown action: a neutral dot so the tile still reads as "configured".
    dot(0.5f, 0.5f, 0.16f);
}

void drawGlyphPlus(Graphics& g, const RectF& box, const Color& c,
                   float thickness) {
    Pen p(c, thickness);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    float cx = box.X + box.Width / 2, cy = box.Y + box.Height / 2;
    float r = std::min(box.Width, box.Height) / 2;
    g.DrawLine(&p, PointF(cx - r, cy), PointF(cx + r, cy));
    g.DrawLine(&p, PointF(cx, cy - r), PointF(cx, cy + r));
}

void drawGlyphDial(Graphics& g, const RectF& box, const Color& c) {
    // Two small chevrons back to back: the wheel turns both ways. Deliberately
    // tiny - it marks a tile rather than decorating one.
    Pen p(c, std::max(1.0f, box.Width * 0.16f));
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    p.SetLineJoin(LineJoinRound);
    const float cx = box.X + box.Width / 2;
    const float w = box.Width * 0.34f;
    const float top = box.Y + box.Height * 0.08f;
    const float bot = box.GetBottom() - box.Height * 0.08f;
    const float h = box.Height * 0.30f;
    g.DrawLine(&p, cx - w, top + h, cx, top);
    g.DrawLine(&p, cx, top, cx + w, top + h);
    g.DrawLine(&p, cx - w, bot - h, cx, bot);
    g.DrawLine(&p, cx, bot, cx + w, bot - h);
}

void drawGlyphChevron(Graphics& g, const RectF& box, const Color& c, bool down) {
    Pen p(c, std::max(1.5f, box.Height * 0.14f));
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    p.SetLineJoin(LineJoinRound);
    float x0 = box.X, x1 = box.X + box.Width / 2, x2 = box.GetRight();
    float yA = down ? box.Y : box.GetBottom();
    float yB = down ? box.GetBottom() : box.Y;
    PointF pts[3] = {PointF(x0, yA), PointF(x1, yB), PointF(x2, yA)};
    g.DrawLines(&p, pts, 3);
}

void drawGlyphPlay(Graphics& g, const RectF& box, const Color& c) {
    SolidBrush b(c);
    PointF pts[3] = {PointF(box.X, box.Y), PointF(box.GetRight(),
                                                  box.Y + box.Height / 2),
                     PointF(box.X, box.GetBottom())};
    g.FillPolygon(&b, pts, 3);
}

void drawGlyphStop(Graphics& g, const RectF& box, const Color& c) {
    SolidBrush b(c);
    GraphicsPath* p = roundedPath(box, box.Width * 0.15f);
    g.FillPath(&b, p);
    delete p;
}

void drawGlyphGear(Graphics& g, const RectF& box, const Color& c) {
    Pen p(c, std::max(1.4f, box.Width * 0.10f));
    float cx = box.X + box.Width / 2, cy = box.Y + box.Height / 2;
    float rOut = std::min(box.Width, box.Height) / 2;
    float rIn = rOut * 0.42f;
    g.DrawEllipse(&p, RectF(cx - rIn, cy - rIn, rIn * 2, rIn * 2));
    for (int i = 0; i < 6; ++i) {
        double a = i * 3.14159265 / 3.0;
        g.DrawLine(&p,
                   PointF(cx + (REAL)(std::cos(a) * rIn * 1.35),
                          cy + (REAL)(std::sin(a) * rIn * 1.35)),
                   PointF(cx + (REAL)(std::cos(a) * rOut),
                          cy + (REAL)(std::sin(a) * rOut)));
    }
}

void drawGlyphKeyboard(Graphics& g, const RectF& box, const Color& c) {
    drawActionIcon(g, "keys", box, c);
}

void drawAppIcon(Bitmap& bmp, int size) {
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(Color(0, 0, 0, 0));

        // A denim plate with four cream keys, the first one in the light
        // blue wash - the same four-key mark as before, in the SMEdevelop
        // colours.
        REAL r = size * 0.22f;
        GraphicsPath plate;
        plate.AddArc(0.0f, 0.0f, r * 2, r * 2, 180, 90);
        plate.AddArc(size - r * 2, 0.0f, r * 2, r * 2, 270, 90);
        plate.AddArc(size - r * 2, size - r * 2, r * 2, r * 2, 0, 90);
        plate.AddArc(0.0f, size - r * 2, r * 2, r * 2, 90, 90);
        plate.CloseFigure();
        SolidBrush plateBrush(gc(tk::colors().primary));
        g.FillPath(&plateBrush, &plate);

        // a 2x2 deck of keys with the first one lit
        REAL m = size * 0.18f, gap = size * 0.07f;
        REAL cell = (size - m * 2 - gap) / 2;
        for (int i = 0; i < 4; ++i) {
            REAL x = m + (i % 2) * (cell + gap);
            REAL y = m + (i / 2) * (cell + gap);
            SolidBrush b(i == 0 ? gc(tk::colors().sky300)
                                : gc(tk::colors().surfaceRaised));
            REAL kr = cell * 0.28f;
            GraphicsPath key;
            key.AddArc(x, y, kr * 2, kr * 2, 180, 90);
            key.AddArc(x + cell - kr * 2, y, kr * 2, kr * 2, 270, 90);
            key.AddArc(x + cell - kr * 2, y + cell - kr * 2, kr * 2, kr * 2, 0, 90);
            key.AddArc(x, y + cell - kr * 2, kr * 2, kr * 2, 90, 90);
            key.CloseFigure();
            g.FillPath(&b, &key);
        }
    }
}

HICON makeAppIcon(int size) {
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    drawAppIcon(bmp, size);
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

}  // namespace ui
