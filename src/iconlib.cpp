#include "iconlib.h"

#include "theme.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <windowsx.h>

using namespace Gdiplus;

namespace ui {
namespace {

constexpr char kPrefix[] = "lib:";

// Unit coordinates inside the icon box, so every icon below is drawn on a
// 0..1 grid and scales to any tile size. Deliberately a local copy of the
// same helper theme.cpp uses for the action glyphs - they are two independent
// sets of drawings and neither should be able to break the other.
struct U {
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

void ln(Graphics& g, Pen& p, const U& u, float x1, float y1, float x2,
        float y2) {
    g.DrawLine(&p, u(x1, y1), u(x2, y2));
}

// Points come in as a flat {x,y, x,y, ...} brace list. MSVC has no compound
// literals in C++, so an initializer_list is how a shape stays written out
// inline at the point it is drawn instead of as a named array above it.
std::vector<PointF> unitPts(const U& u, std::initializer_list<float> xy) {
    std::vector<PointF> pts;
    pts.reserve(xy.size() / 2);
    const float* v = xy.begin();
    for (size_t i = 0; i + 1 < xy.size(); i += 2)
        pts.push_back(u(v[i], v[i + 1]));
    return pts;
}

void poly(Graphics& g, Pen& p, const U& u, std::initializer_list<float> xy) {
    std::vector<PointF> pts = unitPts(u, xy);
    if (pts.size() >= 2) g.DrawLines(&p, pts.data(), (INT)pts.size());
}

void fillPoly(Graphics& g, SolidBrush& b, const U& u,
              std::initializer_list<float> xy) {
    std::vector<PointF> pts = unitPts(u, xy);
    if (pts.size() >= 3) g.FillPolygon(&b, pts.data(), (INT)pts.size());
}

GraphicsPath* rounded(const RectF& r, float radius) {
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

void roundRect(Graphics& g, Pen& p, const U& u, float x, float y, float w,
               float h, float rad) {
    GraphicsPath* path = rounded(u.rect(x, y, w, h), u.sx(rad));
    g.DrawPath(&p, path);
    delete path;
}

// ------------------------------------------------------------- the library
// Order here is the order in the picker.
const std::vector<LibIcon>& lib() {
    static const std::vector<LibIcon> v = {
        // --- Office
        {"printer", "Printer", "Office"},
        {"save", "Save", "Office"},
        {"folder", "Folder", "Office"},
        {"file", "Document", "Office"},
        {"copy", "Copy", "Office"},
        {"clipboard", "Clipboard", "Office"},
        {"calendar", "Calendar", "Office"},
        {"clock", "Clock", "Office"},
        {"mail", "Mail", "Office"},
        {"phone", "Phone", "Office"},
        {"chat", "Message", "Office"},
        {"search", "Search", "Office"},
        {"calculator", "Calculator", "Office"},
        {"tag", "Label", "Office"},
        // --- Media
        {"play", "Play", "Media"},
        {"pause", "Pause", "Media"},
        {"stop", "Stop", "Media"},
        {"record", "Record", "Media"},
        {"next", "Next", "Media"},
        {"previous", "Previous", "Media"},
        {"volume", "Volume", "Media"},
        {"mute", "Mute", "Media"},
        {"camera", "Camera", "Media"},
        {"mic", "Microphone", "Media"},
        {"screen", "Screen", "Media"},
        // --- Actions
        {"check", "Tick", "Actions"},
        {"cross", "Cross", "Actions"},
        {"plus", "Plus", "Actions"},
        {"minus", "Minus", "Actions"},
        {"star", "Star", "Actions"},
        {"heart", "Heart", "Actions"},
        {"flag", "Flag", "Actions"},
        {"bell", "Bell", "Actions"},
        {"lock", "Lock", "Actions"},
        {"unlock", "Unlock", "Actions"},
        {"refresh", "Refresh", "Actions"},
        {"undo", "Undo", "Actions"},
        {"redo", "Redo", "Actions"},
        {"power", "Power", "Actions"},
        {"gear", "Settings", "Actions"},
        {"trash", "Delete", "Actions"},
        {"warning", "Warning", "Actions"},
        // --- Arrows
        {"arrow_up", "Up", "Arrows"},
        {"arrow_down", "Down", "Arrows"},
        {"arrow_left", "Left", "Arrows"},
        {"arrow_right", "Right", "Arrows"},
        {"home", "Home", "Arrows"},
        {"external", "Open out", "Arrows"},
        // --- Practice
        {"tooth", "Tooth", "Practice"},
        {"firstaid", "First aid", "Practice"},
        {"pill", "Medication", "Practice"},
        {"user", "Person", "Practice"},
        {"users", "People", "Practice"},
        {"chart", "Chart", "Practice"},
        {"xray", "X-ray", "Practice"},
    };
    return v;
}

}  // namespace

const std::vector<LibIcon>& iconLibrary() { return lib(); }

const std::vector<std::string>& iconCategories() {
    static std::vector<std::string> cats;
    if (cats.empty())
        for (const LibIcon& i : lib()) {
            if (std::find(cats.begin(), cats.end(), i.category) == cats.end())
                cats.push_back(i.category);
        }
    return cats;
}

bool isLibraryIcon(const std::string& f) {
    return f.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0;
}

std::string libraryIconId(const std::string& f) {
    return isLibraryIcon(f) ? f.substr(sizeof(kPrefix) - 1) : std::string();
}

std::string libraryIconRef(const std::string& id) { return kPrefix + id; }

bool libraryHasIcon(const std::string& id) {
    for (const LibIcon& i : lib())
        if (id == i.id) return true;
    return false;
}

// ---------------------------------------------------------------- drawing
bool drawLibraryIcon(Graphics& g, const std::string& id, const RectF& box,
                     const Color& c) {
    if (!libraryHasIcon(id)) return false;

    U u{box};
    const float w = std::max(1.3f, box.Width * 0.08f);
    Pen p(c, w);
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    p.SetLineJoin(LineJoinRound);
    Pen thin(c, std::max(1.0f, w * 0.72f));
    thin.SetStartCap(LineCapRound);
    thin.SetEndCap(LineCapRound);
    thin.SetLineJoin(LineJoinRound);
    SolidBrush br(c);

    auto dot = [&](float x, float y, float r) {
        PointF at = u(x, y);
        g.FillEllipse(&br, RectF(at.X - u.sx(r), at.Y - u.sx(r), u.sx(r * 2),
                                 u.sx(r * 2)));
    };
    auto circle = [&](float x, float y, float r, Pen& pen) {
        PointF at = u(x, y);
        g.DrawEllipse(&pen, RectF(at.X - u.sx(r), at.Y - u.sx(r), u.sx(r * 2),
                                  u.sx(r * 2)));
    };

    // ------------------------------------------------------------- Office
    if (id == "printer") {
        roundRect(g, p, u, 0.16f, 0.40f, 0.68f, 0.30f, 0.06f);
        poly(g, p, u, {0.30f, 0.40f, 0.30f, 0.16f, 0.70f, 0.16f,
                                      0.70f, 0.40f});
        g.FillRectangle(&br, u.rect(0.30f, 0.66f, 0.40f, 0.20f));
        dot(0.74f, 0.50f, 0.035f);
        return true;
    }
    if (id == "save") {
        roundRect(g, p, u, 0.16f, 0.16f, 0.68f, 0.68f, 0.06f);
        g.DrawRectangle(&p, u.rect(0.34f, 0.16f, 0.32f, 0.22f));
        g.DrawRectangle(&thin, u.rect(0.30f, 0.54f, 0.40f, 0.30f));
        return true;
    }
    if (id == "folder") {
        poly(g, p, u, {0.12f, 0.76f, 0.12f, 0.26f, 0.40f, 0.26f,
                                      0.48f, 0.38f, 0.88f, 0.38f, 0.88f, 0.76f,
                                      0.12f, 0.76f});
        return true;
    }
    if (id == "file") {
        poly(g, p, u, {0.24f, 0.86f, 0.24f, 0.14f, 0.60f, 0.14f,
                                      0.76f, 0.32f, 0.76f, 0.86f, 0.24f,
                                      0.86f});
        ln(g, thin, u, 0.60f, 0.14f, 0.60f, 0.32f);
        ln(g, thin, u, 0.60f, 0.32f, 0.76f, 0.32f);
        ln(g, thin, u, 0.36f, 0.52f, 0.64f, 0.52f);
        ln(g, thin, u, 0.36f, 0.66f, 0.64f, 0.66f);
        return true;
    }
    if (id == "copy") {
        g.DrawRectangle(&p, u.rect(0.14f, 0.14f, 0.46f, 0.46f));
        g.DrawRectangle(&p, u.rect(0.40f, 0.40f, 0.46f, 0.46f));
        return true;
    }
    if (id == "clipboard") {
        roundRect(g, p, u, 0.22f, 0.20f, 0.56f, 0.66f, 0.06f);
        roundRect(g, thin, u, 0.38f, 0.10f, 0.24f, 0.16f, 0.04f);
        ln(g, thin, u, 0.34f, 0.48f, 0.66f, 0.48f);
        ln(g, thin, u, 0.34f, 0.64f, 0.66f, 0.64f);
        return true;
    }
    if (id == "calendar") {
        roundRect(g, p, u, 0.14f, 0.22f, 0.72f, 0.62f, 0.06f);
        ln(g, p, u, 0.14f, 0.40f, 0.86f, 0.40f);
        ln(g, thin, u, 0.32f, 0.12f, 0.32f, 0.28f);
        ln(g, thin, u, 0.68f, 0.12f, 0.68f, 0.28f);
        dot(0.34f, 0.56f, 0.045f);
        dot(0.50f, 0.56f, 0.045f);
        dot(0.66f, 0.56f, 0.045f);
        return true;
    }
    if (id == "clock") {
        circle(0.50f, 0.50f, 0.36f, p);
        ln(g, p, u, 0.50f, 0.50f, 0.50f, 0.28f);
        ln(g, p, u, 0.50f, 0.50f, 0.66f, 0.58f);
        return true;
    }
    if (id == "mail") {
        roundRect(g, p, u, 0.12f, 0.26f, 0.76f, 0.48f, 0.05f);
        poly(g, thin, u, {0.12f, 0.30f, 0.50f, 0.56f, 0.88f,
                                         0.30f});
        return true;
    }
    if (id == "phone") {
        // A handset with a cord reads as a lightning bolt at 30 pixels.
        // A mobile does not.
        roundRect(g, p, u, 0.30f, 0.10f, 0.40f, 0.80f, 0.08f);
        ln(g, thin, u, 0.43f, 0.20f, 0.57f, 0.20f);
        dot(0.50f, 0.79f, 0.045f);
        return true;
    }
    if (id == "chat") {
        roundRect(g, p, u, 0.12f, 0.20f, 0.76f, 0.48f, 0.10f);
        poly(g, p, u, {0.30f, 0.68f, 0.30f, 0.86f, 0.48f,
                                      0.68f});
        return true;
    }
    if (id == "search") {
        circle(0.44f, 0.44f, 0.26f, p);
        ln(g, p, u, 0.63f, 0.63f, 0.84f, 0.84f);
        return true;
    }
    if (id == "calculator") {
        roundRect(g, p, u, 0.20f, 0.12f, 0.60f, 0.76f, 0.07f);
        g.DrawRectangle(&thin, u.rect(0.30f, 0.22f, 0.40f, 0.16f));
        for (int r = 0; r < 2; ++r)
            for (int c2 = 0; c2 < 3; ++c2)
                dot(0.34f + c2 * 0.16f, 0.52f + r * 0.16f, 0.04f);
        return true;
    }
    if (id == "tag") {
        poly(g, p, u, {0.50f, 0.12f, 0.88f, 0.12f, 0.88f, 0.50f,
                                      0.50f, 0.88f, 0.12f, 0.50f, 0.50f,
                                      0.12f});
        dot(0.72f, 0.28f, 0.05f);
        return true;
    }

    // -------------------------------------------------------------- Media
    if (id == "play") {
        fillPoly(g, br, u, {0.30f, 0.18f, 0.82f, 0.50f, 0.30f,
                                           0.82f});
        return true;
    }
    if (id == "pause") {
        g.FillRectangle(&br, u.rect(0.28f, 0.20f, 0.16f, 0.60f));
        g.FillRectangle(&br, u.rect(0.56f, 0.20f, 0.16f, 0.60f));
        return true;
    }
    if (id == "stop") {
        GraphicsPath* path = rounded(u.rect(0.24f, 0.24f, 0.52f, 0.52f),
                                     u.sx(0.05f));
        g.FillPath(&br, path);
        delete path;
        return true;
    }
    if (id == "record") {
        PointF at = u(0.50f, 0.50f);
        g.FillEllipse(&br, RectF(at.X - u.sx(0.28f), at.Y - u.sx(0.28f),
                                 u.sx(0.56f), u.sx(0.56f)));
        return true;
    }
    if (id == "next" || id == "previous") {
        const bool fwd = id == "next";
        const float a = fwd ? 0.24f : 0.76f, b2 = fwd ? 0.62f : 0.38f;
        fillPoly(g, br, u, {a, 0.22f, b2, 0.50f, a, 0.78f});
        g.FillRectangle(&br, u.rect(fwd ? 0.66f : 0.24f, 0.22f, 0.10f, 0.56f));
        return true;
    }
    if (id == "volume" || id == "mute") {
        fillPoly(g, br, u, {0.14f, 0.38f, 0.30f, 0.38f, 0.48f,
                                           0.20f, 0.48f, 0.80f, 0.30f, 0.62f,
                                           0.14f, 0.62f});
        if (id == "volume") {
            g.DrawArc(&thin, u.rect(0.42f, 0.28f, 0.28f, 0.44f), -60.0f, 120.0f);
            g.DrawArc(&thin, u.rect(0.48f, 0.18f, 0.40f, 0.64f), -60.0f, 120.0f);
        } else {
            ln(g, p, u, 0.62f, 0.38f, 0.86f, 0.62f);
            ln(g, p, u, 0.86f, 0.38f, 0.62f, 0.62f);
        }
        return true;
    }
    if (id == "camera") {
        roundRect(g, p, u, 0.10f, 0.28f, 0.80f, 0.50f, 0.07f);
        poly(g, thin, u, {0.34f, 0.28f, 0.40f, 0.18f, 0.60f,
                                         0.18f, 0.66f, 0.28f});
        circle(0.50f, 0.54f, 0.15f, p);
        return true;
    }
    if (id == "mic") {
        roundRect(g, p, u, 0.38f, 0.12f, 0.24f, 0.42f, 0.12f);
        g.DrawArc(&p, u.rect(0.26f, 0.36f, 0.48f, 0.40f), 0.0f, 180.0f);
        ln(g, p, u, 0.50f, 0.76f, 0.50f, 0.88f);
        return true;
    }
    if (id == "screen") {
        roundRect(g, p, u, 0.10f, 0.20f, 0.80f, 0.50f, 0.05f);
        ln(g, p, u, 0.34f, 0.84f, 0.66f, 0.84f);
        ln(g, thin, u, 0.50f, 0.70f, 0.50f, 0.84f);
        return true;
    }

    // ------------------------------------------------------------ Actions
    if (id == "check") {
        poly(g, p, u, {0.18f, 0.52f, 0.42f, 0.74f, 0.82f,
                                      0.26f});
        return true;
    }
    if (id == "cross") {
        ln(g, p, u, 0.24f, 0.24f, 0.76f, 0.76f);
        ln(g, p, u, 0.76f, 0.24f, 0.24f, 0.76f);
        return true;
    }
    if (id == "plus") {
        ln(g, p, u, 0.50f, 0.18f, 0.50f, 0.82f);
        ln(g, p, u, 0.18f, 0.50f, 0.82f, 0.50f);
        return true;
    }
    if (id == "minus") {
        ln(g, p, u, 0.18f, 0.50f, 0.82f, 0.50f);
        return true;
    }
    if (id == "star") {
        fillPoly(g, br, u, {0.50f, 0.12f, 0.61f, 0.38f, 0.88f, 0.40f,
                            0.67f, 0.58f, 0.74f, 0.86f, 0.50f, 0.70f,
                            0.26f, 0.86f, 0.33f, 0.58f, 0.12f, 0.40f,
                            0.39f, 0.38f});
        return true;
    }
    if (id == "heart") {
        GraphicsPath path;
        PointF c1 = u(0.50f, 0.84f);
        path.AddArc(u.rect(0.12f, 0.20f, 0.40f, 0.38f), 150.0f, 210.0f);
        path.AddArc(u.rect(0.48f, 0.20f, 0.40f, 0.38f), 180.0f, 210.0f);
        path.AddLine(u(0.86f, 0.46f), c1);
        path.CloseFigure();
        g.FillPath(&br, &path);
        return true;
    }
    if (id == "flag") {
        ln(g, p, u, 0.26f, 0.12f, 0.26f, 0.88f);
        poly(g, p, u, {0.26f, 0.16f, 0.80f, 0.28f, 0.26f,
                                      0.50f});
        return true;
    }
    if (id == "bell") {
        g.DrawArc(&p, u.rect(0.22f, 0.14f, 0.56f, 0.56f), 180.0f, 180.0f);
        ln(g, p, u, 0.22f, 0.42f, 0.22f, 0.66f);
        ln(g, p, u, 0.78f, 0.42f, 0.78f, 0.66f);
        ln(g, p, u, 0.14f, 0.68f, 0.86f, 0.68f);
        g.DrawArc(&thin, u.rect(0.42f, 0.70f, 0.16f, 0.16f), 0.0f, 180.0f);
        return true;
    }
    if (id == "lock" || id == "unlock") {
        roundRect(g, p, u, 0.22f, 0.44f, 0.56f, 0.42f, 0.06f);
        if (id == "lock")
            g.DrawArc(&p, u.rect(0.32f, 0.16f, 0.36f, 0.40f), 180.0f, 180.0f);
        else
            g.DrawArc(&p, u.rect(0.46f, 0.14f, 0.36f, 0.40f), 180.0f, 140.0f);
        dot(0.50f, 0.64f, 0.05f);
        return true;
    }
    if (id == "refresh") {
        g.DrawArc(&p, u.rect(0.18f, 0.18f, 0.64f, 0.64f), 40.0f, 280.0f);
        fillPoly(g, br, u, {0.78f, 0.10f, 0.90f, 0.42f, 0.60f,
                                           0.34f});
        return true;
    }
    if (id == "undo" || id == "redo") {
        const bool back = id == "undo";
        g.DrawArc(&p, u.rect(0.18f, 0.30f, 0.64f, 0.50f), 180.0f, 180.0f);
        // Filled heads: two thin strokes read as a tick at tile size.
        if (back) {
            ln(g, p, u, 0.18f, 0.58f, 0.18f, 0.34f);
            fillPoly(g, br, u, {0.04f, 0.40f, 0.32f, 0.40f, 0.18f, 0.14f});
        } else {
            ln(g, p, u, 0.82f, 0.58f, 0.82f, 0.34f);
            fillPoly(g, br, u, {0.68f, 0.40f, 0.96f, 0.40f, 0.82f, 0.14f});
        }
        return true;
    }
    if (id == "power") {
        g.DrawArc(&p, u.rect(0.20f, 0.22f, 0.60f, 0.60f), -60.0f, 300.0f);
        ln(g, p, u, 0.50f, 0.12f, 0.50f, 0.46f);
        return true;
    }
    if (id == "gear") {
        circle(0.50f, 0.50f, 0.18f, p);
        for (int i = 0; i < 8; ++i) {
            const double a = i * 3.14159265 / 4.0;
            const float cx = 0.50f + (float)std::cos(a) * 0.30f;
            const float cy = 0.50f + (float)std::sin(a) * 0.30f;
            const float ix = 0.50f + (float)std::cos(a) * 0.21f;
            const float iy = 0.50f + (float)std::sin(a) * 0.21f;
            ln(g, p, u, ix, iy, cx, cy);
        }
        return true;
    }
    if (id == "trash") {
        ln(g, p, u, 0.14f, 0.26f, 0.86f, 0.26f);
        poly(g, p, u, {0.24f, 0.26f, 0.30f, 0.86f, 0.70f, 0.86f,
                                      0.76f, 0.26f});
        poly(g, thin, u, {0.38f, 0.26f, 0.38f, 0.16f, 0.62f,
                                         0.16f, 0.62f, 0.26f});
        ln(g, thin, u, 0.42f, 0.40f, 0.44f, 0.72f);
        ln(g, thin, u, 0.58f, 0.40f, 0.56f, 0.72f);
        return true;
    }
    if (id == "warning") {
        poly(g, p, u, {0.50f, 0.14f, 0.90f, 0.82f, 0.10f, 0.82f,
                                      0.50f, 0.14f});
        ln(g, p, u, 0.50f, 0.40f, 0.50f, 0.60f);
        dot(0.50f, 0.71f, 0.045f);
        return true;
    }

    // ------------------------------------------------------------- Arrows
    if (id == "arrow_up" || id == "arrow_down" || id == "arrow_left" ||
        id == "arrow_right") {
        if (id == "arrow_up") {
            ln(g, p, u, 0.50f, 0.84f, 0.50f, 0.20f);
            poly(g, p, u, {0.28f, 0.42f, 0.50f, 0.18f, 0.72f,
                                          0.42f});
        } else if (id == "arrow_down") {
            ln(g, p, u, 0.50f, 0.16f, 0.50f, 0.80f);
            poly(g, p, u, {0.28f, 0.58f, 0.50f, 0.82f, 0.72f,
                                          0.58f});
        } else if (id == "arrow_left") {
            ln(g, p, u, 0.84f, 0.50f, 0.20f, 0.50f);
            poly(g, p, u, {0.42f, 0.28f, 0.18f, 0.50f, 0.42f,
                                          0.72f});
        } else {
            ln(g, p, u, 0.16f, 0.50f, 0.80f, 0.50f);
            poly(g, p, u, {0.58f, 0.28f, 0.82f, 0.50f, 0.58f,
                                          0.72f});
        }
        return true;
    }
    if (id == "home") {
        poly(g, p, u, {0.12f, 0.48f, 0.50f, 0.14f, 0.88f,
                                      0.48f});
        poly(g, p, u, {0.22f, 0.44f, 0.22f, 0.86f, 0.78f, 0.86f,
                                      0.78f, 0.44f});
        g.DrawRectangle(&thin, u.rect(0.42f, 0.60f, 0.16f, 0.26f));
        return true;
    }
    if (id == "external") {
        poly(g, p, u, {0.56f, 0.18f, 0.20f, 0.18f, 0.20f, 0.80f,
                                      0.82f, 0.80f, 0.82f, 0.46f});
        ln(g, p, u, 0.50f, 0.50f, 0.86f, 0.14f);
        poly(g, p, u, {0.62f, 0.14f, 0.88f, 0.14f, 0.88f,
                                      0.40f});
        return true;
    }

    // ----------------------------------------------------------- Practice
    if (id == "tooth") {
        // Two crowns meeting at the top, tapering to two roots.
        GraphicsPath path;
        // Shallower crown, longer roots: the first attempt read as an arch.
        path.AddArc(u.rect(0.14f, 0.10f, 0.72f, 0.48f), 180.0f, 180.0f);
        path.AddLine(u(0.86f, 0.34f), u(0.69f, 0.90f));
        path.AddLine(u(0.69f, 0.90f), u(0.55f, 0.46f));
        path.AddLine(u(0.55f, 0.46f), u(0.45f, 0.46f));
        path.AddLine(u(0.45f, 0.46f), u(0.31f, 0.90f));
        path.AddLine(u(0.31f, 0.90f), u(0.14f, 0.34f));
        path.CloseFigure();
        g.DrawPath(&p, &path);
        return true;
    }
    if (id == "firstaid") {
        roundRect(g, p, u, 0.12f, 0.24f, 0.76f, 0.56f, 0.08f);
        ln(g, p, u, 0.50f, 0.36f, 0.50f, 0.68f);
        ln(g, p, u, 0.34f, 0.52f, 0.66f, 0.52f);
        return true;
    }
    if (id == "pill") {
        // Flat, not rotated: on the diagonal the outline plus its seam read
        // as a paperclip.
        roundRect(g, p, u, 0.10f, 0.34f, 0.80f, 0.32f, 0.16f);
        ln(g, p, u, 0.50f, 0.34f, 0.50f, 0.66f);
        // One half filled is what separates a capsule from a rounded box.
        GraphicsPath* half = rounded(u.rect(0.10f, 0.34f, 0.80f, 0.32f),
                                     u.sx(0.16f));
        Region clip(u.rect(0.10f, 0.34f, 0.40f, 0.32f));
        g.SetClip(&clip);
        g.FillPath(&br, half);
        g.ResetClip();
        delete half;
        return true;
    }
    if (id == "user") {
        circle(0.50f, 0.32f, 0.18f, p);
        g.DrawArc(&p, u.rect(0.20f, 0.54f, 0.60f, 0.56f), 180.0f, 180.0f);
        return true;
    }
    if (id == "users") {
        circle(0.38f, 0.32f, 0.16f, p);
        g.DrawArc(&p, u.rect(0.10f, 0.52f, 0.56f, 0.52f), 180.0f, 180.0f);
        circle(0.74f, 0.36f, 0.12f, thin);
        g.DrawArc(&thin, u.rect(0.56f, 0.54f, 0.40f, 0.40f), 200.0f, 140.0f);
        return true;
    }
    if (id == "chart") {
        ln(g, p, u, 0.16f, 0.14f, 0.16f, 0.84f);
        ln(g, p, u, 0.16f, 0.84f, 0.88f, 0.84f);
        g.FillRectangle(&br, u.rect(0.28f, 0.54f, 0.13f, 0.30f));
        g.FillRectangle(&br, u.rect(0.46f, 0.36f, 0.13f, 0.48f));
        g.FillRectangle(&br, u.rect(0.64f, 0.22f, 0.13f, 0.62f));
        return true;
    }
    if (id == "xray") {
        roundRect(g, p, u, 0.18f, 0.12f, 0.64f, 0.76f, 0.06f);
        // a row of teeth on the film
        for (int i = 0; i < 3; ++i) {
            const float x = 0.30f + i * 0.18f;
            g.DrawArc(&thin, u.rect(x - 0.07f, 0.34f, 0.14f, 0.20f), 180.0f,
                      180.0f);
            ln(g, thin, u, x - 0.05f, 0.44f, x - 0.03f, 0.62f);
            ln(g, thin, u, x + 0.05f, 0.44f, x + 0.03f, 0.62f);
        }
        return true;
    }

    return false;
}

}  // namespace ui
