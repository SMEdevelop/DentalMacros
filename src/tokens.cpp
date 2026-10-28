#include "tokens.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tk {
namespace {

Rgb hx(unsigned v, uint8_t a = 255) {
    return Rgb{(uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 8) & 0xFF),
               (uint8_t)(v & 0xFF), a};
}

double channel(uint8_t v) {
    const double c = v / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const Rgb& c) {
    return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b);
}

}  // namespace

const Colors& colors() {
    static const Colors c = [] {
        Colors t;
        t.surface = hx(0xf7f3ec);
        t.surfaceRaised = hx(0xfffdf9);
        t.surfaceSunken = hx(0xefe8dc);
        t.surfaceHover = hx(0xe6dccb);
        t.line = hx(0xe3d9c8);
        t.lineStrong = hx(0x9a8a74);
        t.ink = hx(0x3a3027);
        t.inkMuted = hx(0x6e5f4e);
        t.sky100 = hx(0xe3eef6);
        t.sky300 = hx(0xb9d3e6);
        t.sand100 = hx(0xf1e6d8);
        t.sand300 = hx(0xdcc6a8);
        t.clay = hx(0xa0704a);
        t.clayInk = hx(0x7a5235);
        t.primary = hx(0x3d6b8e);
        t.primaryHover = hx(0x335c7b);
        t.onPrimary = hx(0xffffff);
        t.primaryTint = t.sky100;
        t.primaryInk = hx(0x2f5878);
        t.focusRing = hx(0x3d6b8e);
        t.success = hx(0x4f7a52);
        t.onSuccess = hx(0xffffff);
        t.successTint = hx(0xe5efe2);
        t.warning = hx(0xc98a2b);
        t.warningTint = hx(0xfaf0d9);
        t.warningInk = hx(0x8a5a12);
        t.danger = hx(0xb4483a);
        t.onDanger = hx(0xffffff);
        t.dangerTint = hx(0xf8e5e0);
        t.dangerInk = hx(0xa03f32);
        // rgba(58, 48, 39, 0.40)
        t.overlay = Rgb{58, 48, 39, 102};
        return t;
    }();
    return c;
}

const std::vector<KeyColor>& keyColors() {
    static const std::vector<KeyColor> v = {
        {"red", "Terracotta", "#b4483a", hx(0xb4483a)},
        {"green", "Sage", "#4f7a52", hx(0x4f7a52)},
        {"blue", "Denim", "#3d6b8e", hx(0x3d6b8e)},
        {"ochre", "Ochre", "#9a6a2e", hx(0x9a6a2e)},
        {"plum", "Plum", "#7a5a7e", hx(0x7a5a7e)},
        {"spruce", "Spruce", "#3f7a74", hx(0x3f7a74)},
        {"walnut", "Walnut", "#6f5a48", hx(0x6f5a48)},
    };
    return v;
}

const KeyColor& defaultKeyColor() { return keyColors().back(); }

const KeyColor& nearestKeyColor(const std::string& hex) {
    Rgb c;
    if (!parseHex(hex, c)) return defaultKeyColor();
    // Nearest by hue first: a bright red from before the redesign should come
    // out terracotta, not walnut, even though walnut is closer in plain RGB
    // distance to many dark reds. Greys and near-greys have no hue to speak
    // of, so they go to walnut, the neutral one.
    auto hsv = [](const Rgb& x, double& h, double& s, double& v) {
        const double r = x.r / 255.0, g = x.g / 255.0, b = x.b / 255.0;
        const double mx = std::fmax(r, std::fmax(g, b));
        const double mn = std::fmin(r, std::fmin(g, b));
        const double d = mx - mn;
        v = mx;
        s = mx <= 0 ? 0 : d / mx;
        if (d <= 1e-9) { h = 0; return; }
        if (mx == r) h = std::fmod((g - b) / d, 6.0);
        else if (mx == g) h = (b - r) / d + 2.0;
        else h = (r - g) / d + 4.0;
        h *= 60.0;
        if (h < 0) h += 360.0;
    };
    double h, s, v;
    hsv(c, h, s, v);
    if (s < 0.18 || v < 0.12) return defaultKeyColor();

    const KeyColor* best = &defaultKeyColor();
    double bestScore = 1e18;
    for (const KeyColor& k : keyColors()) {
        double kh, ks, kv;
        hsv(k.rgb, kh, ks, kv);
        if (ks < 0.18) continue;   // walnut is only for the hueless
        double dh = std::fabs(h - kh);
        if (dh > 180) dh = 360 - dh;
        const double score = dh * dh + 400.0 * (s - ks) * (s - ks) +
                             400.0 * (v - kv) * (v - kv);
        if (score < bestScore) {
            bestScore = score;
            best = &k;
        }
    }
    return *best;
}

bool parseHex(const std::string& hexIn, Rgb& out) {
    size_t a = 0, b = hexIn.size();
    while (a < b && std::isspace((unsigned char)hexIn[a])) ++a;
    while (b > a && std::isspace((unsigned char)hexIn[b - 1])) --b;
    std::string h = hexIn.substr(a, b - a);
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() == 3) {
        std::string e;
        for (char ch : h) { e += ch; e += ch; }
        h = e;
    }
    if (h.size() != 6) return false;
    for (char ch : h)
        if (!std::isxdigit((unsigned char)ch)) return false;
    out = hx((unsigned)std::strtoul(h.c_str(), nullptr, 16));
    return true;
}

std::string toHex(const Rgb& c) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

double contrast(const Rgb& a, const Rgb& b) {
    const double la = luminance(a), lb = luminance(b);
    const double hi = std::fmax(la, lb), lo = std::fmin(la, lb);
    return (hi + 0.05) / (lo + 0.05);
}

TextSpec spec(Text t) {
    switch (t) {
        case Text::Display: return {44, 50, 800, 0};
        case Text::Title: return {26, 32, 700, 0};
        case Text::TileLabel: return {24, 30, 700, 0};
        case Text::KeyLabel: return {22, 28, 700, 0};
        case Text::Button: return {18, 24, 600, 0};
        case Text::ButtonBig: return {20, 26, 700, 0};
        case Text::Body: return {17, 26, 400, 0};
        case Text::Label: return {15, 20, 600, 0};
        case Text::Overline: return {13, 18, 700, 0.05f};
        case Text::Compact: return {15, 20, 400, 0};
        case Text::CompactBold: return {15, 20, 600, 0};
    }
    return {17, 26, 400, 0};
}

}  // namespace tk
