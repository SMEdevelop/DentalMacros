#include "theme.h"

#include "util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace Gdiplus;

namespace ui {
namespace {
float g_dpi = 1.0f;

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

}  // namespace

const Theme& theme() {
    static Theme t;
    return t;
}

void setDpiScale(float s) { g_dpi = (s > 0.1f && s < 8.0f) ? s : 1.0f; }
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
    Color c = fromHex(hex, fallback);
    // Tile labels are white; darken anything too bright to keep them legible.
    double lum = (0.299 * c.GetR() + 0.587 * c.GetG() + 0.114 * c.GetB()) / 255.0;
    if (lum > 0.62) return mix(c, Color(255, 0, 0, 0), (float)(lum - 0.5));
    return c;
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

        // rounded dark plate
        REAL r = size * 0.22f;
        GraphicsPath plate;
        plate.AddArc(0.0f, 0.0f, r * 2, r * 2, 180, 90);
        plate.AddArc(size - r * 2, 0.0f, r * 2, r * 2, 270, 90);
        plate.AddArc(size - r * 2, size - r * 2, r * 2, r * 2, 0, 90);
        plate.AddArc(0.0f, size - r * 2, r * 2, r * 2, 90, 90);
        plate.CloseFigure();
        SolidBrush dark(Color(255, 30, 33, 39));
        g.FillPath(&dark, &plate);

        // a 2x2 deck of keys with the first one lit
        REAL m = size * 0.18f, gap = size * 0.07f;
        REAL cell = (size - m * 2 - gap) / 2;
        for (int i = 0; i < 4; ++i) {
            REAL x = m + (i % 2) * (cell + gap);
            REAL y = m + (i / 2) * (cell + gap);
            SolidBrush b(i == 0 ? Color(255, 79, 140, 255)
                                : Color(255, 78, 85, 97));
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
