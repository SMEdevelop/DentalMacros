#include "vision.h"

#include "gdiplus_inc.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

using namespace Gdiplus;

namespace vis {
namespace {

// ------------------------------------------------------------ gray images
struct Gray {
    int w = 0, h = 0;
    std::vector<float> px;
    float at(int x, int y) const { return px[(size_t)y * w + x]; }
};

Gray toGray(const uint32_t* bgra, int w, int h) {
    Gray g;
    g.w = w;
    g.h = h;
    g.px.resize((size_t)w * h);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        uint32_t p = bgra[i];
        float b = (float)(p & 0xFF);
        float gr = (float)((p >> 8) & 0xFF);
        float r = (float)((p >> 16) & 0xFF);
        g.px[i] = 0.114f * b + 0.587f * gr + 0.299f * r;
    }
    return g;
}

Gray downsample(const Gray& s) {
    Gray d;
    d.w = s.w / 2;
    d.h = s.h / 2;
    if (d.w < 1 || d.h < 1) return d;
    d.px.resize((size_t)d.w * d.h);
    for (int y = 0; y < d.h; ++y) {
        const float* r0 = &s.px[(size_t)(y * 2) * s.w];
        const float* r1 = &s.px[(size_t)(y * 2 + 1) * s.w];
        float* out = &d.px[(size_t)y * d.w];
        for (int x = 0; x < d.w; ++x) {
            int i = x * 2;
            out[x] = 0.25f * (r0[i] + r0[i + 1] + r1[i] + r1[i + 1]);
        }
    }
    return d;
}

struct TplStats {
    double mean = 0;
    double norm = 0;   // sqrt(sum(T^2) - sum(T)^2/n)
};

TplStats statsOf(const Gray& t) {
    TplStats s;
    double sum = 0, sum2 = 0;
    for (float v : t.px) { sum += v; sum2 += (double)v * v; }
    double n = (double)t.px.size();
    s.mean = sum / n;
    double var = sum2 - sum * sum / n;
    s.norm = var > 0 ? std::sqrt(var) : 0;
    return s;
}

// Normalised cross-correlation of `tpl` against `img` at (x, y).
// Identical metric to cv2.TM_CCOEFF_NORMED.
double nccAt(const Gray& img, const Gray& tpl, const TplStats& ts, int x,
             int y) {
    double sumI = 0, sumI2 = 0, cross = 0;
    for (int r = 0; r < tpl.h; ++r) {
        const float* ip = &img.px[(size_t)(y + r) * img.w + x];
        const float* tp = &tpl.px[(size_t)r * tpl.w];
        for (int c = 0; c < tpl.w; ++c) {
            double iv = ip[c];
            sumI += iv;
            sumI2 += iv * iv;
            cross += iv * tp[c];
        }
    }
    double n = (double)tpl.px.size();
    double varI = sumI2 - sumI * sumI / n;
    if (varI <= 1e-9 || ts.norm <= 1e-9) return 0.0;
    return (cross - sumI * ts.mean) / (std::sqrt(varI) * ts.norm);
}

struct Cand {
    int x = 0, y = 0;
    double score = 0;
};

// ------------------------------------------------------------- GDI+ helper
std::once_flag g_gdiOnce;
ULONG_PTR g_gdiToken = 0;

void ensureGdiPlus() {
    std::call_once(g_gdiOnce, [] {
        GdiplusStartupInput in;
        GdiplusStartup(&g_gdiToken, &in, nullptr);
    });
}

bool getEncoderClsid(const wchar_t* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

// -------------------------------------------------------- template caching
struct CachedTpl {
    Gray gray;
    FILETIME stamp{};
    int w = 0, h = 0;
};

std::mutex g_cacheMu;
std::map<std::string, CachedTpl> g_cache;

bool fileStamp(const std::string& path, FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &fad))
        return false;
    out = fad.ftLastWriteTime;
    return true;
}

bool loadTemplate(const std::string& path, Gray& out, std::string* error) {
    const std::string full = resolveDataPath(path);
    if (!fileExists(full)) {
        if (error) *error = "template image not found: " + path;
        return false;
    }

    FILETIME stamp{};
    fileStamp(full, stamp);
    {
        std::lock_guard<std::mutex> lk(g_cacheMu);
        auto it = g_cache.find(full);
        if (it != g_cache.end() &&
            CompareFileTime(&it->second.stamp, &stamp) == 0) {
            out = it->second.gray;
            return true;
        }
    }

    ensureGdiPlus();
    Bitmap bmp(widen(full).c_str());
    if (bmp.GetLastStatus() != Ok) {
        if (error) *error = "could not decode image: " + path;
        return false;
    }
    int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) {
        if (error) *error = "image has no pixels: " + path;
        return false;
    }
    BitmapData bd{};
    Rect rc(0, 0, w, h);
    if (bmp.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) {
        if (error) *error = "could not read pixels from: " + path;
        return false;
    }
    std::vector<uint32_t> px((size_t)w * h);
    for (int y = 0; y < h; ++y)
        memcpy(&px[(size_t)y * w],
               (BYTE*)bd.Scan0 + (ptrdiff_t)y * bd.Stride,
               (size_t)w * 4);
    bmp.UnlockBits(&bd);

    out = toGray(px.data(), w, h);
    {
        std::lock_guard<std::mutex> lk(g_cacheMu);
        CachedTpl c;
        c.gray = out;
        c.stamp = stamp;
        c.w = w;
        c.h = h;
        g_cache[full] = std::move(c);
    }
    return true;
}

}  // namespace

void clearTemplateCache() {
    std::lock_guard<std::mutex> lk(g_cacheMu);
    g_cache.clear();
}

// ---------------------------------------------------------------- capture
bool captureRegion(int x, int y, int w, int h, Capture& out,
                   std::string* error) {
    if (w <= 0 || h <= 0) {
        if (error) *error = "capture region is empty";
        return false;
    }
    HDC screen = GetDC(nullptr);
    if (!screen) {
        if (error) *error = "could not access the screen";
        return false;
    }
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        // CAPTUREBLT so layered/transparent windows are included
        ok = BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT) != 0;
        if (ok) {
            out.originX = x;
            out.originY = y;
            out.width = w;
            out.height = h;
            out.bgra.resize((size_t)w * h);
            memcpy(out.bgra.data(), bits, (size_t)w * h * 4);
        }
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (!ok && error) *error = "screen capture failed: " + lastErrorMessage();
    return ok;
}

bool captureScreen(Capture& out, std::string* error) {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return captureRegion(x, y, w, h, out, error);
}

namespace {
struct MonEnum {
    std::vector<RECT> rects;
};
BOOL CALLBACK monProc(HMONITOR mon, HDC, LPRECT, LPARAM data) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi))
        ((MonEnum*)data)->rects.push_back(mi.rcMonitor);
    return TRUE;
}
std::vector<RECT> monitors() {
    MonEnum e;
    EnumDisplayMonitors(nullptr, nullptr, monProc, (LPARAM)&e);
    return e.rects;
}
}  // namespace

int monitorCount() { return (int)monitors().size(); }

bool monitorRect(int index, RECT& out) {
    auto m = monitors();
    if (index < 0 || index >= (int)m.size()) return false;
    out = m[(size_t)index];
    return true;
}

bool captureMonitor(int index, Capture& out, std::string* error) {
    RECT r;
    if (!monitorRect(index, r)) {
        if (error) *error = "no such monitor";
        return false;
    }
    return captureRegion(r.left, r.top, r.right - r.left, r.bottom - r.top, out,
                         error);
}

// ---------------------------------------------------------------- matching
std::vector<Match> findAll(const std::string& templatePath, double confidence,
                           const RECT* region, int limit, std::string* error,
                           bool* fatal) {
    std::vector<Match> result;
    if (limit <= 0) limit = 1;
    if (fatal) *fatal = false;

    Gray tpl;
    if (!loadTemplate(templatePath, tpl, error)) {
        // Missing, undecodable or empty file: waiting cannot fix any of these.
        if (fatal) *fatal = true;
        return result;
    }

    TplStats full = statsOf(tpl);
    if (full.norm <= 1e-6) {
        // A flat template correlates perfectly with any flat area, which would
        // report a confident match at a meaningless location. Refuse instead.
        if (error)
            *error = "template '" + baseName(templatePath) +
                     "' is a single flat colour - it has no pattern to match. "
                     "Re-snip it around something distinctive.";
        if (fatal) *fatal = true;
        return result;
    }

    Capture cap;
    bool ok = region ? captureRegion(region->left, region->top,
                                     region->right - region->left,
                                     region->bottom - region->top, cap, error)
                     : captureScreen(cap, error);
    if (!ok) return result;

    Gray img = toGray(cap.bgra.data(), cap.width, cap.height);
    if (img.w < tpl.w || img.h < tpl.h) {
        if (error)
            *error = "template is larger than the area being searched";
        if (fatal) *fatal = true;
        return result;
    }

    // Pyramid: search coarse, then refine. Keeps a full-screen search fast
    // without giving up sub-pixel-accurate placement.
    std::vector<Gray> imgPyr{img}, tplPyr{tpl};
    while ((int)imgPyr.size() < 5) {
        const Gray& t = tplPyr.back();
        const Gray& i = imgPyr.back();
        if (t.w / 2 < 8 || t.h / 2 < 8) break;
        if (i.w / 2 < t.w || i.h / 2 < t.h) break;
        Gray ti = downsample(i), tt = downsample(t);
        if (ti.w < 1 || tt.w < 1) break;
        imgPyr.push_back(std::move(ti));
        tplPyr.push_back(std::move(tt));
    }

    const int top = (int)imgPyr.size() - 1;
    const double coarseThresh = std::max(0.25, confidence - 0.25);

    // --- coarse pass over the whole (downsampled) image
    std::vector<Cand> cands;
    {
        const Gray& I = imgPyr[(size_t)top];
        const Gray& T = tplPyr[(size_t)top];
        TplStats ts = statsOf(T);
        const int maxX = I.w - T.w, maxY = I.h - T.h;
        if (maxX < 0 || maxY < 0) return result;
        for (int y = 0; y <= maxY; ++y)
            for (int x = 0; x <= maxX; ++x) {
                double s = nccAt(I, T, ts, x, y);
                if (s >= coarseThresh) cands.push_back({x, y, s});
            }
        std::sort(cands.begin(), cands.end(),
                  [](const Cand& a, const Cand& b) { return a.score > b.score; });
        // Thin out the candidate list, but only just enough to keep it small.
        // Suppressing a wide neighbourhood here would be a mistake: when two
        // near-identical things sit close together, downsampling can score the
        // wrong one fractionally higher, and discarding its neighbour would
        // throw away the position that is actually a perfect match at full
        // resolution. Keep both and let the refinement pass decide.
        std::vector<Cand> kept;
        const int sepX = std::max(1, T.w / 8);
        const int sepY = std::max(1, T.h / 8);
        for (const Cand& c : cands) {
            bool tooClose = false;
            for (const Cand& k : kept)
                if (std::abs(k.x - c.x) < sepX && std::abs(k.y - c.y) < sepY) {
                    tooClose = true;
                    break;
                }
            if (!tooClose) kept.push_back(c);
            if ((int)kept.size() >= limit * 8 + 24) break;
        }
        cands.swap(kept);
    }
    if (cands.empty()) return result;

    // --- refine each candidate down the pyramid
    for (int level = top - 1; level >= 0; --level) {
        const Gray& I = imgPyr[(size_t)level];
        const Gray& T = tplPyr[(size_t)level];
        TplStats ts = statsOf(T);
        const int maxX = I.w - T.w, maxY = I.h - T.h;
        for (Cand& c : cands) {
            int bx = c.x * 2, by = c.y * 2;
            double best = -2;
            int bestX = bx, bestY = by;
            const int radius = 3;
            for (int dy = -radius; dy <= radius; ++dy)
                for (int dx = -radius; dx <= radius; ++dx) {
                    int x = bx + dx, y = by + dy;
                    if (x < 0 || y < 0 || x > maxX || y > maxY) continue;
                    double s = nccAt(I, T, ts, x, y);
                    if (s > best) { best = s; bestX = x; bestY = y; }
                }
            c.x = bestX;
            c.y = bestY;
            c.score = best;
        }
    }

    // --- final filter, suppress overlaps, sort
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.score > b.score; });
    std::vector<Cand> kept;
    for (const Cand& c : cands) {
        if (c.score < confidence) continue;
        bool tooClose = false;
        for (const Cand& k : kept)
            if (std::abs(k.x - c.x) < std::max(1, tpl.w / 2) &&
                std::abs(k.y - c.y) < std::max(1, tpl.h / 2)) {
                tooClose = true;
                break;
            }
        if (tooClose) continue;
        kept.push_back(c);
        if ((int)kept.size() >= limit) break;
    }

    for (const Cand& c : kept) {
        Match m;
        m.x = cap.originX + c.x + tpl.w / 2;
        m.y = cap.originY + c.y + tpl.h / 2;
        m.confidence = c.score;
        m.w = tpl.w;
        m.h = tpl.h;
        result.push_back(m);
    }
    return result;
}

bool findImage(const std::string& templatePath, double confidence,
               const RECT* region, Match& out, std::string* error,
               bool* fatal) {
    auto all = findAll(templatePath, confidence, region, 1, error, fatal);
    if (all.empty()) return false;
    out = all[0];
    return true;
}

bool waitForImage(const std::string& templatePath, double timeoutSec,
                  double confidence, const RECT* region, Match& out,
                  std::string* error) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::duration_cast<clock::duration>(
                                       std::chrono::duration<double>(timeoutSec));
    std::string localErr;
    while (true) {
        localErr.clear();
        bool fatal = false;
        if (findImage(templatePath, confidence, region, out, &localErr, &fatal))
            return true;
        // An unusable template will never start working - stop now instead of
        // burning the whole timeout on it.
        if (fatal) {
            if (error) *error = localErr;
            return false;
        }
        if (clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    if (error && error->empty()) *error = localErr;
    return false;
}

// ------------------------------------------------------------------- files
bool savePng(const std::string& path, const Capture& cap, RECT crop,
             std::string* error) {
    if (!cap.valid()) {
        if (error) *error = "nothing captured";
        return false;
    }
    int x = std::max(0L, crop.left);
    int y = std::max(0L, crop.top);
    int w = std::min((LONG)cap.width - x, crop.right - crop.left);
    int h = std::min((LONG)cap.height - y, crop.bottom - crop.top);
    if (w <= 0 || h <= 0) {
        if (error) *error = "selection is empty";
        return false;
    }

    ensureGdiPlus();
    Bitmap bmp(w, h, PixelFormat32bppARGB);
    BitmapData bd{};
    Rect rc(0, 0, w, h);
    if (bmp.LockBits(&rc, ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Ok) {
        if (error) *error = "could not build the image";
        return false;
    }
    for (int r = 0; r < h; ++r) {
        auto* dst = (uint32_t*)((BYTE*)bd.Scan0 + (ptrdiff_t)r * bd.Stride);
        const uint32_t* src = &cap.bgra[(size_t)(y + r) * cap.width + x];
        for (int c = 0; c < w; ++c) dst[c] = src[c] | 0xFF000000u;  // opaque
    }
    bmp.UnlockBits(&bd);

    CLSID png;
    if (!getEncoderClsid(L"image/png", &png)) {
        if (error) *error = "no PNG encoder available";
        return false;
    }
    std::string fullPath = dataPath(path);
    size_t slash = fullPath.find_last_of("\\/");
    if (slash != std::string::npos) ensureDir(fullPath.substr(0, slash));
    Status st = bmp.Save(widen(fullPath).c_str(), &png, nullptr);
    if (st != Ok) {
        if (error) *error = "could not save " + path;
        return false;
    }
    clearTemplateCache();
    return true;
}

bool imageSize(const std::string& path, int& w, int& h) {
    const std::string full = resolveDataPath(path);
    if (!fileExists(full)) return false;
    ensureGdiPlus();
    Bitmap bmp(widen(full).c_str());
    if (bmp.GetLastStatus() != Ok) return false;
    w = (int)bmp.GetWidth();
    h = (int)bmp.GetHeight();
    return true;
}

}  // namespace vis
