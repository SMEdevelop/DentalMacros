// Exports src\app.ico from the same drawing code the app uses at runtime.
//
// Run tools\make_icon.bat after changing drawAppIcon() in theme.cpp. The
// result is committed, so an ordinary build never needs this tool.
//
// The file holds every size Windows asks for - Explorer's views, the taskbar,
// Alt+Tab, the Start menu and the installer all pick different ones. Sizes up
// to 64 are stored as plain 32-bit DIBs, which every version of Windows and
// every icon-reading tool understands; 128 and 256 are stored as PNG, which
// Vista onwards supports and which keeps the file (and therefore the exe)
// about ten times smaller than the uncompressed alternative.

#include "gdiplus_inc.h"
#include "theme.h"
#include "util.h"

#include <objidl.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace Gdiplus;

namespace {

std::vector<uint8_t> g_scratch;

bool encoderClsid(const wchar_t* mime, CLSID* out) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (!size) return false;
    std::vector<uint8_t> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i)
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *out = codecs[i].Clsid;
            return true;
        }
    return false;
}

// PNG bytes for one rendered size.
bool encodePng(Bitmap& bmp, std::vector<uint8_t>& out) {
    CLSID png;
    if (!encoderClsid(L"image/png", &png)) return false;

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    bool ok = bmp.Save(stream, &png, nullptr) == Ok;
    if (ok) {
        // The stream position after Save is exactly how many bytes it wrote.
        // GlobalSize would over-report: the block is rounded up.
        LARGE_INTEGER zero{};
        ULARGE_INTEGER end{};
        stream->Seek(zero, STREAM_SEEK_CUR, &end);
        out.resize((size_t)end.QuadPart);
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG got = 0;
        ok = out.empty() ||
             (SUCCEEDED(stream->Read(out.data(), (ULONG)out.size(), &got)) &&
              got == out.size());
    }
    stream->Release();
    return ok;
}

// A 32-bit bottom-up DIB plus the 1bpp AND mask an .ico entry must carry.
bool encodeDib(Bitmap& bmp, int size, std::vector<uint8_t>& out) {
    BitmapData bd{};
    Rect rc(0, 0, size, size);
    if (bmp.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok)
        return false;

    const size_t maskRow = (size_t)(((size + 31) / 32) * 4);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = size;
    bih.biHeight = size * 2;   // colour rows followed by mask rows
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    out.clear();
    out.reserve(sizeof(bih) + (size_t)size * size * 4 + maskRow * size);
    const auto* h = (const uint8_t*)&bih;
    out.insert(out.end(), h, h + sizeof(bih));

    for (int y = size - 1; y >= 0; --y) {   // DIBs run bottom-up
        const auto* row = (const uint8_t*)bd.Scan0 + (ptrdiff_t)y * bd.Stride;
        out.insert(out.end(), row, row + (size_t)size * 4);
    }
    // Transparency comes from the alpha channel; the mask just has to be
    // there and the right size, so leave it all-opaque.
    out.insert(out.end(), maskRow * (size_t)size, 0);

    bmp.UnlockBits(&bd);
    return true;
}

#pragma pack(push, 1)
struct IconDir {
    uint16_t reserved, type, count;
};
struct IconDirEntry {
    uint8_t width, height, colours, reserved;
    uint16_t planes, bitCount;
    uint32_t bytes, offset;
};
#pragma pack(pop)

}  // namespace

int main() {
    ULONG_PTR token = 0;
    GdiplusStartupInput input;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) {
        std::printf("could not start GDI+\n");
        return 1;
    }

    const int sizes[] = {16, 20, 24, 32, 48, 64, 128, 256};
    const size_t n = sizeof(sizes) / sizeof(sizes[0]);

    std::vector<std::vector<uint8_t>> blobs;
    for (size_t i = 0; i < n; ++i) {
        const int size = sizes[i];
        Bitmap bmp(size, size, PixelFormat32bppARGB);
        ui::drawAppIcon(bmp, size);

        std::vector<uint8_t> blob;
        const bool asPng = size >= 128;
        const bool ok = asPng ? encodePng(bmp, blob) : encodeDib(bmp, size, blob);
        if (!ok) {
            std::printf("failed to encode the %dx%d image\n", size, size);
            GdiplusShutdown(token);
            return 1;
        }
        std::printf("  %3dx%-3d  %-4s %7zu bytes\n", size, size,
                    asPng ? "png" : "dib", blob.size());
        blobs.push_back(std::move(blob));
    }

    std::string file;
    IconDir dir{0, 1, (uint16_t)n};
    file.append((const char*)&dir, sizeof(dir));

    uint32_t offset =
        (uint32_t)(sizeof(IconDir) + sizeof(IconDirEntry) * n);
    for (size_t i = 0; i < n; ++i) {
        IconDirEntry e{};
        // 256 is written as 0: the field is one byte and 256 does not fit.
        e.width = (uint8_t)(sizes[i] >= 256 ? 0 : sizes[i]);
        e.height = e.width;
        e.planes = 1;
        e.bitCount = 32;
        e.bytes = (uint32_t)blobs[i].size();
        e.offset = offset;
        offset += e.bytes;
        file.append((const char*)&e, sizeof(e));
    }
    for (const auto& b : blobs)
        file.append((const char*)b.data(), b.size());

    const std::string out = "src\\app.ico";
    if (!writeFileAtomic(out, file)) {
        std::printf("could not write %s\n", out.c_str());
        GdiplusShutdown(token);
        return 1;
    }
    std::printf("\nwrote %s (%zu images, %zu bytes)\n", out.c_str(), n,
                file.size());

    GdiplusShutdown(token);
    return 0;
}
