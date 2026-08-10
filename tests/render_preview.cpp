// Renders MacroDeck's custom-drawn UI pieces to a PNG.
//
// The app itself imports the low-level keyboard hook, which Windows Smart App
// Control blocks for unsigned binaries - so this tool exists to exercise and
// eyeball the drawing code without launching the app.

#include "app.h"
#include "config.h"
#include "gdiplus_inc.h"
#include "keys.h"
#include "theme.h"
#include "util.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Gdiplus;
using namespace ui;

static bool encoderClsid(const wchar_t* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (!size) return false;
    std::vector<BYTE> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i)
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *clsid = codecs[i].Clsid;
            return true;
        }
    return false;
}

// A stand-in for the app's tile painter, using the same primitives.
static void drawTile(Graphics& g, RectF r, const std::string& keyName,
                     const std::string& label, const std::string& action,
                     const std::string& colorHex, bool selected, bool empty) {
    const float radius = 10.0f;
    if (empty) {
        fillRounded(g, r, radius, theme().tileEmpty);
        strokeRoundedDashed(g, r, radius, theme().border, 1.4f);
        RectF plus(r.X + r.Width / 2 - 9, r.Y + r.Height / 2 - 9, 18, 18);
        drawGlyphPlus(g, plus, theme().textFaint, 1.8f);
        if (!keyName.empty())
            drawText(g, widen(keys::displayName(keyName)),
                     RectF(r.X, r.GetBottom() - 22, r.Width, 16), 10,
                     theme().textFaint, Align::Center);
        return;
    }

    Color base = readableTileColor(colorHex, theme().tile);
    fillRounded(g, r, radius, base);
    strokeRounded(g, r, radius,
                  selected ? theme().accent : withAlpha(theme().border, 140),
                  selected ? 2.0f : 1.0f);

    std::wstring kl = widen(keys::displayName(keyName));
    SizeF ks = measureText(g, kl, 9, true);
    RectF badge(r.X + 7, r.Y + 6, ks.Width + 10, 15);
    fillRounded(g, badge, 4.0f, withAlpha(Color(255, 0, 0, 0), 70));
    drawText(g, kl, badge, 9, withAlpha(theme().text, 220), Align::Center, true);

    float iconSize = (std::min)(r.Width, r.Height) * 0.36f;
    RectF iconBox(r.X + (r.Width - iconSize) / 2, r.Y + r.Height * 0.24f,
                  iconSize, iconSize);
    drawActionIcon(g, action, iconBox, withAlpha(Color(255, 255, 255, 255), 235));

    drawText(g, widen(label), RectF(r.X + 5, r.GetBottom() - 30, r.Width - 10, 26),
             11, Color(255, 255, 255, 255), Align::Center, false, true, true);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    ULONG_PTR token = 0;
    GdiplusStartupInput in;
    if (GdiplusStartup(&token, &in, nullptr) != Ok) {
        std::printf("GDI+ failed to start\n");
        return 1;
    }

    const int W = 1180, H = 1000;
    int rc = 0;
    {
        Bitmap bmp(W, H, PixelFormat32bppARGB);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        SolidBrush bg(theme().bg);
        g.FillRectangle(&bg, 0, 0, W, H);

        drawText(g, L"MacroDeck - drawing preview", RectF(24, 18, 600, 30), 20,
                 theme().text, Align::Left, true);
        drawText(g,
                 L"Every icon below is drawn with vector primitives - no icon "
                 L"fonts, no image assets.",
                 RectF(24, 48, 900, 20), 12, theme().textDim, Align::Left);

        // ---- app icon at several sizes
        drawText(g, L"APP ICON", RectF(24, 84, 300, 18), 11, theme().textFaint,
                 Align::Left, true);
        int ax = 24;
        for (int size : {64, 48, 32, 16}) {
            HICON ic = makeAppIcon(size);
            if (ic) {
                Bitmap* ib = Bitmap::FromHICON(ic);
                if (ib) {
                    g.DrawImage(ib, ax, 108, size, size);
                    delete ib;
                }
                DestroyIcon(ic);
            }
            ax += size + 18;
        }

        // ---- every action icon
        drawText(g, L"ACTION ICONS", RectF(24, 196, 300, 18), 11,
                 theme().textFaint, Align::Left, true);
        int col = 0, row = 0;
        for (const auto& a : cfg::actionDefs()) {
            float x = 24.0f + col * 282.0f;
            float y = 222.0f + row * 46.0f;
            RectF cell(x, y, 270, 40);
            fillRounded(g, cell, 6.0f, theme().panel);
            RectF ib(x + 10, y + 8, 24, 24);
            drawActionIcon(g, a.id, ib, theme().accent);
            drawText(g, widen(a.label), RectF(x + 44, y, 216, 40), 12,
                     theme().text, Align::Left);
            if (++col == 4) { col = 0; ++row; }
        }

        // ---- deck tiles
        float tilesTop = 222.0f + (row + (col ? 1 : 0)) * 46.0f + 28.0f;
        drawText(g, L"DECK TILES",
                 RectF(24, tilesTop, 300, 18), 11, theme().textFaint,
                 Align::Left, true);
        float ty = tilesTop + 26;
        struct Sample {
            const char* key;
            const char* label;
            const char* action;
            const char* color;
            bool selected;
            bool empty;
        };
        const Sample samples[] = {
            {"num1", "Open Slack", "run", "", false, false},
            {"num2", "Mute mic", "keys", "#3b82f6", false, false},
            {"num3", "Build & test", "run", "#8b5cf6", true, false},
            {"num4", "Click Submit", "click_image", "#14b8a6", false, false},
            {"num5", "Tidy windows", "window_restore", "#f59e0b", false, false},
            {"num6", "", "", "", false, true},
            {"numadd", "", "", "", false, true},
        };
        float tx = 24;
        for (const auto& s : samples) {
            drawTile(g, RectF(tx, ty, 128, 128), s.key, s.label, s.action,
                     s.color, s.selected, s.empty);
            tx += 144;
        }

        drawText(g,
                 L"Left to right: default colour, custom colours, selected "
                 L"(accent ring), and two unassigned keys.",
                 RectF(24, ty + 140, 1000, 20), 11, theme().textDim,
                 Align::Left);

        // ---- numpad preset shape
        drawText(g, L"NUMPAD PRESET LAYOUT", RectF(24, ty + 172, 400, 18), 11,
                 theme().textFaint, Align::Left, true);
        const cfg::DeckPreset* np = cfg::findPreset("numpad");
        const float side = 52, gap = 7;
        for (const auto& slot : np->slots) {
            RectF r(24 + slot.col * (side + gap), ty + 198 + slot.row * (side + gap),
                    side * slot.colSpan + gap * (slot.colSpan - 1),
                    side * slot.rowSpan + gap * (slot.rowSpan - 1));
            fillRounded(g, r, 6.0f, theme().tileEmpty);
            strokeRoundedDashed(g, r, 6.0f, theme().border, 1.2f);
            drawText(g, widen(keys::displayName(slot.key)), r, 10,
                     theme().textDim, Align::Center);
        }
        drawText(g,
                 L"The numpad preset knows its own key names, so clicking a "
                 L"square binds that exact key -\nno key-pressing needed. "
                 L"Note the tall Num + and Enter keys and the double-width 0.",
                 RectF(300, ty + 210, 700, 60), 12, theme().textDim, Align::Left,
                 false, false, true);

        CLSID png;
        if (encoderClsid(L"image/png", &png)) {
            std::string out = appPath("ui-preview.png");
            if (bmp.Save(widen(out).c_str(), &png, nullptr) == Ok)
                std::printf("wrote %s\n", out.c_str());
            else {
                std::printf("could not save the preview\n");
                rc = 1;
            }
        } else {
            std::printf("no PNG encoder\n");
            rc = 1;
        }
    }

    GdiplusShutdown(token);
    return rc;
}
