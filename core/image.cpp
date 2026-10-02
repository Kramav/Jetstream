#include "image.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace remod {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {

void check(HRESULT hr, const std::string& what) {
    if (FAILED(hr)) {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(hr));
        throw std::runtime_error(what + " failed (" + hex + ")");
    }
}

// COM for the calling thread, for the duration of one call. A thread already in another apartment mode
// (e.g. an STA UI thread) is fine: WIC works in both.
struct ComScope {
    bool owned = false;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr != RPC_E_CHANGED_MODE) check(hr, "CoInitializeEx");
        owned = SUCCEEDED(hr);
    }
    ~ComScope() {
        if (owned) CoUninitialize();
    }
};

ComPtr<IWICImagingFactory> factory() {
    ComPtr<IWICImagingFactory> f;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)),
          "creating the WIC factory");
    return f;
}

void encode_png(IWICImagingFactory* wic, const fs::path& out, unsigned w, unsigned h, const std::uint8_t* bgra) {
    ComPtr<IWICStream> stream;
    check(wic->CreateStream(&stream), "CreateStream");
    check(stream->InitializeFromFilename(out.c_str(), GENERIC_WRITE), "opening " + out.string() + " for writing");
    ComPtr<IWICBitmapEncoder> encoder;
    check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "CreateEncoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "encoder Initialize");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    check(encoder->CreateNewFrame(&frame, &props), "CreateNewFrame");
    check(frame->Initialize(props.Get()), "frame Initialize");
    check(frame->SetSize(w, h), "SetSize");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format), "SetPixelFormat");
    if (format != GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("PNG encoder refused 32-bit BGRA");
    check(frame->WritePixels(h, w * 4, w * h * 4, const_cast<BYTE*>(bgra)), "WritePixels");
    check(frame->Commit(), "frame Commit");
    check(encoder->Commit(), "encoder Commit");
}

// TGA -> 32-bit BGRA, top-down. WIC has no TGA codec. Handles types 2/3 (true-colour/grey) and 10/11 (their RLE
// forms) at 24/32 and 8 bits, which is what editors and Noesis write. 32-bit pixels keep their alpha even when the
// header declares no alpha bits (Noesis writes it that way). Origin: descriptor bit 5 set = top-left, else bottom-left.
std::vector<std::uint8_t> read_tga(const fs::path& file, unsigned& w, unsigned& h) {
    std::ifstream in(file, std::ios::binary);
    const std::string b(std::istreambuf_iterator<char>(in), {});
    auto u8 = [&](size_t i) -> unsigned {
        if (i >= b.size()) throw std::runtime_error(file.string() + " is not a complete TGA file");
        return std::uint8_t(b[i]);
    };
    const unsigned type = u8(2), bits = u8(16), desc = u8(17);
    const bool grey = type == 3 || type == 11, rle = type == 10 || type == 11;
    if (!(grey ? bits == 8 : (type == 2 || type == 10) && (bits == 24 || bits == 32)))
        throw std::runtime_error(file.string() + ": unsupported TGA (type " + std::to_string(type) + ", " +
                                 std::to_string(bits) + "-bit)");
    w = u8(12) | u8(13) << 8;
    h = u8(14) | u8(15) << 8;
    const unsigned px = bits / 8;
    size_t at = 18 + u8(0) + size_t(u8(5) | u8(6) << 8) * ((u8(7) + 7) / 8);  // skip image id and any colour map

    std::vector<std::uint8_t> out(size_t(w) * h * 4);
    auto put = [&](size_t i, size_t src) {  // the i-th pixel in file order, from bytes at src
        const size_t row = i / w, y = desc & 0x20 ? row : h - 1 - row;
        std::uint8_t* d = out.data() + (y * w + i % w) * 4;
        d[0] = std::uint8_t(u8(src));
        d[1] = std::uint8_t(grey ? d[0] : u8(src + 1));
        d[2] = std::uint8_t(grey ? d[0] : u8(src + 2));
        d[3] = std::uint8_t(px == 4 ? u8(src + 3) : 255);
    };
    for (size_t i = 0, n = size_t(w) * h; i < n;) {
        if (!rle) {
            put(i++, at);
            at += px;
            continue;
        }
        const unsigned head = u8(at++), count = (head & 0x7F) + 1;
        for (unsigned k = 0; k < count && i < n; ++k) {
            put(i++, at);
            if (!(head & 0x80)) at += px;  // raw packet: a new pixel each time; run packet: the same one
        }
        if (head & 0x80) at += px;
    }
    return out;
}

// `pixels` (w x h, BGRA) scaled to sw x sh (WIC's Fant) and the `part` of that copied to `dst` (rows `stride` apart).
// `what` names the image in errors.
void scale_into(IWICImagingFactory* wic, const std::uint8_t* pixels, unsigned w, unsigned h, unsigned sw, unsigned sh,
                const WICRect& part, std::uint8_t* dst, unsigned stride, const std::string& what) {
    ComPtr<IWICBitmap> bitmap;  // WIC copies the pixels (WICBitmapCacheOnLoad)
    check(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, w * h * 4, const_cast<BYTE*>(pixels),
                                      &bitmap),
          "reading " + what);
    ComPtr<IWICBitmapScaler> scaler;  // output keeps the input's 32-bit BGRA
    check(wic->CreateBitmapScaler(&scaler), "CreateBitmapScaler");
    check(scaler->Initialize(bitmap.Get(), sw, sh, WICBitmapInterpolationModeFant), "scaling " + what);
    const UINT bytes = UINT(part.Height - 1) * stride + UINT(part.Width) * 4;
    check(scaler->CopyPixels(&part, stride, bytes, dst), "copying " + what);
}

}  // namespace

void save_png_bgra(const fs::path& out, unsigned width, unsigned height, const std::vector<std::uint8_t>& bgra) {
    if (bgra.size() != size_t(width) * height * 4) throw std::runtime_error("save_png_bgra: wrong pixel count");
    ComScope com;
    encode_png(factory().Get(), out, width, height, bgra.data());
}

std::vector<std::uint8_t> read_image_bgra(const fs::path& file, unsigned& width, unsigned& height) {
    std::string ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".tga") return read_tga(file, width, height);
    ComScope com;
    const auto wic = factory();
    ComPtr<IWICBitmapDecoder> decoder;
    check(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder),
          "reading " + file.string());
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame), "reading " + file.string());
    ComPtr<IWICFormatConverter> bgra;
    check(wic->CreateFormatConverter(&bgra), "CreateFormatConverter");
    check(bgra->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                           WICBitmapPaletteTypeCustom),
          "converting " + file.string());
    check(bgra->GetSize(&width, &height), "GetSize");
    std::vector<std::uint8_t> pixels(size_t(width) * height * 4);
    check(bgra->CopyPixels(nullptr, width * 4, UINT(pixels.size()), pixels.data()), "reading " + file.string());
    return pixels;
}

void tile_images(const std::vector<fs::path>& images, const fs::path& out, unsigned tile) {
    if (images.empty()) throw std::runtime_error("no images to combine");
    ComScope com;
    const auto wic = factory();

    const unsigned n = unsigned(images.size());
    const unsigned cols = unsigned(std::ceil(std::sqrt(double(n))));
    const unsigned rows = (n + cols - 1) / cols;
    const unsigned width = cols * tile, height = rows * tile;
    std::vector<std::uint8_t> canvas(size_t(width) * height * 4);
    for (size_t i = 0; i < canvas.size(); i += 4) {  // opaque dark grey (B, G, R, A)
        canvas[i] = canvas[i + 1] = canvas[i + 2] = 0x20;
        canvas[i + 3] = 0xFF;
    }

    for (unsigned i = 0; i < n; ++i) {
        unsigned w = 0, h = 0;
        const auto pixels = read_image_bgra(images[i], w, h);
        const double scale = std::min(double(tile) / w, double(tile) / h);
        const UINT sw = std::max(1u, UINT(std::lround(w * scale))), sh = std::max(1u, UINT(std::lround(h * scale)));
        const unsigned x = (i % cols) * tile + (tile - sw) / 2, y = (i / cols) * tile + (tile - sh) / 2;
        scale_into(wic.Get(), pixels.data(), w, h, sw, sh, WICRect{0, 0, INT(sw), INT(sh)},
                   canvas.data() + (size_t(y) * width + x) * 4, width * 4, images[i].string());
    }
    encode_png(wic.Get(), out, width, height, canvas.data());
}

Bgra load_image(const fs::path& file) {
    Bgra image;
    image.pixels = read_image_bgra(file, image.width, image.height);
    return image;
}

void save_png(const fs::path& file, const Bgra& image) { save_png_bgra(file, image.width, image.height, image.pixels); }

void adjust_colour(Bgra& image, float hue, float saturation, float brightness, float contrast) {
    // The hue rotation matrix that keeps luminance (as CSS's hue-rotate), on R, G, B.
    const float c = std::cos(hue * 3.14159265f / 180), s = std::sin(hue * 3.14159265f / 180);
    const float m[3][3] = {{0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f},
                           {0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f},
                           {0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f}};
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        std::uint8_t* p = &image.pixels[i];  // B, G, R, A
        const float r0 = p[2] / 255.0f, g0 = p[1] / 255.0f, b0 = p[0] / 255.0f;
        float rgb[3] = {m[0][0] * r0 + m[0][1] * g0 + m[0][2] * b0, m[1][0] * r0 + m[1][1] * g0 + m[1][2] * b0,
                        m[2][0] * r0 + m[2][1] * g0 + m[2][2] * b0};
        const float lum = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
        for (float& v : rgb) {
            v = lum + (v - lum) * (1 + saturation);
            v = (v - 0.5f) * (1 + contrast) + 0.5f + brightness;
        }
        for (int k = 0; k < 3; ++k) p[2 - k] = std::uint8_t(std::lround(std::clamp(rgb[k], 0.0f, 1.0f) * 255));
    }
}

Bgra resize_image(const Bgra& image, unsigned width, unsigned height, Fit fit) {
    if (width == 0 || height == 0 || image.width == 0 || image.height == 0) throw std::runtime_error("can't resize to or from a zero size");
    Bgra out{width, height, std::vector<std::uint8_t>(size_t(width) * height * 4, 0)};  // transparent
    const double sx = double(width) / image.width, sy = double(height) / image.height;
    const double scale = fit == Fit::Fit ? std::min(sx, sy) : std::max(sx, sy);
    UINT sw = width, sh = height;
    if (fit != Fit::Stretch) {
        sw = std::max(1u, UINT(std::lround(image.width * scale)));
        sh = std::max(1u, UINT(std::lround(image.height * scale)));
    }
    ComScope com;
    const auto wic = factory();
    if (fit == Fit::Fit) {  // the whole image, centred
        const unsigned x = (width - std::min(sw, width)) / 2, y = (height - std::min(sh, height)) / 2;
        sw = std::min(sw, width);
        sh = std::min(sh, height);
        scale_into(wic.Get(), image.pixels.data(), image.width, image.height, sw, sh, WICRect{0, 0, INT(sw), INT(sh)},
                   out.pixels.data() + (size_t(y) * width + x) * 4, width * 4, "the image");
    } else {  // Stretch: all of it; Fill: its centre
        const WICRect part{INT((std::max(sw, width) - width) / 2), INT((std::max(sh, height) - height) / 2), INT(width), INT(height)};
        scale_into(wic.Get(), image.pixels.data(), image.width, image.height, std::max(sw, width), std::max(sh, height), part,
                   out.pixels.data(), width * 4, "the image");
    }
    return out;
}

// ---- Replace photo ----

namespace {

using Plane = std::vector<float>;  // one value per pixel, rows top-down

Plane luma(const Bgra& img) {  // brightness, 0-1
    Plane p(size_t(img.width) * img.height);
    for (size_t i = 0; i < p.size(); ++i) {
        const std::uint8_t* c = &img.pixels[i * 4];
        p[i] = (0.114f * c[0] + 0.587f * c[1] + 0.299f * c[2]) / 255;
    }
    return p;
}

// A box blur `r` pixels each way (rows then columns, the window shrinking at the edges), `passes` times: three are
// close to a Gaussian.
Plane box_blur(Plane p, unsigned w, unsigned h, int r, int passes = 3) {
    if (r <= 0 || w == 0 || h == 0) return p;
    Plane tmp(p.size());
    std::vector<double> sum(size_t(std::max(w, h)) + 1);
    auto pass = [&](const Plane& in, Plane& out, unsigned n, unsigned lines, size_t step, size_t line_step) {
        for (unsigned l = 0; l < lines; ++l) {
            const size_t base = l * line_step;
            sum[0] = 0;
            for (unsigned i = 0; i < n; ++i) sum[i + 1] = sum[i] + in[base + i * step];
            for (unsigned i = 0; i < n; ++i) {
                const int a = std::max(0, int(i) - r), b = std::min(int(n) - 1, int(i) + r);
                out[base + i * step] = float((sum[size_t(b) + 1] - sum[size_t(a)]) / double(b - a + 1));
            }
        }
    };
    for (int k = 0; k < passes; ++k) {
        pass(p, tmp, w, h, 1, w);  // rows
        pass(tmp, p, h, w, w, 1);  // columns
    }
    return p;
}

// The blur of `v` counting only where `weight` is (blur(v * weight) / blur(weight)): the old photo without the frame.
Plane weighted_blur(const Plane& v, const Plane& weight, unsigned w, unsigned h, int r) {
    Plane vw(v.size());
    for (size_t i = 0; i < v.size(); ++i) vw[i] = v[i] * weight[i];
    const Plane a = box_blur(std::move(vw), w, h, r), b = box_blur(weight, w, h, r);
    Plane out(v.size());
    for (size_t i = 0; i < v.size(); ++i) out[i] = b[i] > 1e-4f ? a[i] / b[i] : v[i];
    return out;
}

// The median of `v` over a square `r` pixels each way, counting only `inside` pixels, for the inside pixels of the box
// x0-x1, y0-y1 (others keep their value). A median keeps step edges and drops what's thinner than its window, so
// `v - median` is specks and scratches without a picture's outlines. Sliding 256-level histogram along each row.
Plane median_inside(const Plane& v, const std::vector<bool>& inside, unsigned w, unsigned x0, unsigned y0, unsigned x1,
                    unsigned y1, int r) {
    Plane out(v);
    for (unsigned y = y0; y <= y1; ++y) {
        int hist[256] = {}, n = 0, m = 0, below = 0;  // m: the median's level; below: values under it
        auto column = [&](int x, int d) {
            if (x < int(x0) || x > int(x1)) return;
            for (int yy = std::max(int(y0), int(y) - r); yy <= std::min(int(y1), int(y) + r); ++yy)
                if (const size_t i = size_t(yy) * w + size_t(x); inside[i]) {
                    const int q = std::clamp(int(v[i] * 255 + 0.5f), 0, 255);
                    hist[q] += d;
                    n += d;
                    if (q < m) below += d;
                }
        };
        for (int x = int(x0) - r; x < int(x0) + r; ++x) column(x, 1);
        for (unsigned x = x0; x <= x1; ++x) {
            column(int(x) + r, 1);
            column(int(x) - r - 1, -1);
            const size_t i = size_t(y) * w + x;
            if (!inside[i] || n == 0) continue;
            const int half = (n - 1) / 2;
            while (below > half) below -= hist[--m];
            while (below + hist[m] <= half) below += hist[m++];
            out[i] = float(m) / 255;
        }
    }
    return out;
}

// Squared distances along one line (Felzenszwalb & Huttenlocher): `f` is 0 at the sources, huge elsewhere.
void distance_line(const std::vector<double>& f, std::vector<double>& d, int n, std::vector<int>& v,
                   std::vector<double>& z) {
    constexpr double inf = 1e30;
    int k = 0;
    v[0] = 0;
    z[0] = -inf;
    z[1] = inf;
    for (int q = 1; q < n; ++q) {
        auto meet = [&](int p) { return ((f[q] + double(q) * q) - (f[p] + double(p) * p)) / (2.0 * q - 2.0 * p); };
        double s = meet(v[k]);
        while (s <= z[k]) s = meet(v[--k]);
        v[++k] = q;
        z[k] = s;
        z[k + 1] = inf;
    }
    for (int q = 0, j = 0; q < n; ++q) {
        while (z[j + 1] < q) ++j;
        d[q] = double(q - v[j]) * (q - v[j]) + f[v[j]];
    }
}

// Each inside pixel's distance (pixels) to the nearest outside one or the image's edge (0 outside).
Plane distance_inside(const std::vector<bool>& inside, unsigned width, unsigned height) {
    const int w = int(width) + 2, h = int(height) + 2;  // outside all round
    std::vector<double> g(size_t(w) * h, 0.0);
    for (int y = 1; y + 1 < h; ++y)
        for (int x = 1; x + 1 < w; ++x)
            if (inside[size_t(y - 1) * width + size_t(x - 1)]) g[size_t(y) * w + x] = 1e20;
    const int n = std::max(w, h);
    std::vector<double> f(size_t(n) + 1), d(size_t(n) + 1), z(size_t(n) + 2);
    std::vector<int> v(size_t(n) + 1);
    for (int x = 0; x < w; ++x) {  // columns
        for (int y = 0; y < h; ++y) f[y] = g[size_t(y) * w + x];
        distance_line(f, d, h, v, z);
        for (int y = 0; y < h; ++y) g[size_t(y) * w + x] = d[y];
    }
    for (int y = 0; y < h; ++y) {  // rows
        for (int x = 0; x < w; ++x) f[x] = g[size_t(y) * w + x];
        distance_line(f, d, w, v, z);
        for (int x = 0; x < w; ++x) g[size_t(y) * w + x] = d[x];
    }
    Plane out(size_t(width) * height);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) out[size_t(y) * width + x] = float(std::sqrt(g[size_t(y + 1) * w + x + 1]));
    return out;
}

// How far into the frame each pixel is: its distance to the nearest transparent one (alpha < 128) or the image's edge.
Plane distance_to_outline(const Bgra& img) {
    std::vector<bool> opaque(size_t(img.width) * img.height);
    for (size_t i = 0; i < opaque.size(); ++i) opaque[i] = img.pixels[i * 4 + 3] >= 128;
    return distance_inside(opaque, img.width, img.height);
}

float sample(const Plane& p, unsigned w, unsigned h, float x, float y) {  // bilinear, clamped
    x = std::clamp(x, 0.0f, float(w) - 1.001f);
    y = std::clamp(y, 0.0f, float(h) - 1.001f);
    const unsigned x0 = unsigned(x), y0 = unsigned(y);
    const float fx = x - float(x0), fy = y - float(y0);
    const float* r0 = &p[size_t(y0) * w + x0];
    const float* r1 = r0 + w;
    return (r0[0] * (1 - fx) + r0[1] * fx) * (1 - fy) + (r1[0] * (1 - fx) + r1[1] * fx) * fy;
}

}  // namespace

PhotoArea photo_area(const Bgra& frame, float frame_width, float grow, float feather, float scale) {
    const unsigned w = frame.width, h = frame.height;
    if (w < 8 || h < 8) throw std::runtime_error("the frame image is too small");
    const Plane dist = distance_to_outline(frame);
    // The brightness change across the frame: the Sobel gradient of a slightly blurred brightness, projected onto the
    // outline's normal (the way the distance from the outline grows). A ring's edges all lie along the outline and
    // step the same way all round it; the photo's content (faces, grime, specks) points every way, with either sign.
    const Plane l = box_blur(luma(frame), w, h, 1, 2);
    Plane across(l.size(), 0);
    auto at = [&](const Plane& p, unsigned x, unsigned y) { return p[size_t(y) * w + x]; };
    for (unsigned y = 1; y + 1 < h; ++y)
        for (unsigned x = 1; x + 1 < w; ++x) {
            const float nx = at(dist, x + 1, y) - at(dist, x - 1, y), ny = at(dist, x, y + 1) - at(dist, x, y - 1);
            const float len = std::sqrt(nx * nx + ny * ny);
            if (len < 1e-3f) continue;
            const float gx = at(l, x + 1, y - 1) + 2 * at(l, x + 1, y) + at(l, x + 1, y + 1) - at(l, x - 1, y - 1) -
                             2 * at(l, x - 1, y) - at(l, x - 1, y + 1);
            const float gy = at(l, x - 1, y + 1) + 2 * at(l, x, y + 1) + at(l, x + 1, y + 1) - at(l, x - 1, y - 1) -
                             2 * at(l, x, y - 1) - at(l, x + 1, y - 1);
            across[size_t(y) * w + x] = (gx * nx + gy * ny) / len;
        }
    std::vector<float> inside;
    for (size_t i = 0; i < across.size(); ++i)
        if (dist[i] > 1.5f) inside.push_back(std::abs(across[i]));
    if (inside.empty()) throw std::runtime_error("the frame image is all transparent");

    // The brightness change across the outline, averaged (with its sign) at each whole distance from it: the frame's
    // rings, the photo's edge among them, add up; the photo's content cancels out.
    const int deepest = std::max(4, int(*std::max_element(dist.begin(), dist.end()) * 0.8f));
    std::vector<double> total(size_t(deepest) + 2), count(size_t(deepest) + 2);
    for (size_t i = 0; i < dist.size(); ++i)
        if (const int d = int(std::lround(dist[i])); d >= 1 && d <= deepest) {
            total[size_t(d)] += across[i];
            count[size_t(d)] += 1;
        }
    PhotoArea out;
    out.frame_width = frame_width;
    if (frame_width <= 0) {  // the innermost clear peak is the photo's edge
        std::vector<float> m(size_t(deepest) + 2, 0), smooth(size_t(deepest) + 2, 0);
        for (int d = 1; d <= deepest; ++d)
            m[size_t(d)] = count[size_t(d)] ? float(std::abs(total[size_t(d)] / count[size_t(d)])) : 0;
        for (int d = 2; d < deepest; ++d) smooth[size_t(d)] = (m[size_t(d) - 1] + m[size_t(d)] + m[size_t(d) + 1]) / 3;
        const float best = *std::max_element(smooth.begin(), smooth.end());
        int pick = 0;
        for (int d = 3; d + 1 < deepest; ++d)
            if (smooth[size_t(d)] >= 0.5f * best && smooth[size_t(d)] >= smooth[size_t(d) - 1] &&
                smooth[size_t(d)] >= smooth[size_t(d) + 1])
                pick = d;
        if (!pick) throw std::runtime_error("no frame edge found: set Frame width");
        out.frame_width = float(pick);
    }
    const float fw = out.frame_width;

    // For snapping: the edge strength along each ray (either way: a photo can be lighter than its frame in one place
    // and darker in another; a signed score failed on such a photo), capped at the 95th percentile, so a ramp's
    // steepest point wins. Capped at half of it (before 2026-10-02), every clear edge scored the same and the path
    // wandered (user: the picture covered the frame's edge on some sides).
    const auto p95 = inside.begin() + std::ptrdiff_t(double(inside.size() - 1) * 0.95);
    std::nth_element(inside.begin(), p95, inside.end());
    const float cap = std::max(1e-4f, *p95);
    Plane s(across.size());
    for (size_t i = 0; i < across.size(); ++i) s[i] = std::min(1.0f, std::abs(across[i]) / cap);

    // The middle of the area inside that ring; rays from it find the ring at every angle.
    double cx = 0, cy = 0, n = 0;
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
            if (dist[size_t(y) * w + x] >= fw) cx += x, cy += y, n += 1;
    if (n == 0) {  // in real pixels (a thumbnail's are fewer)
        const float middle = *std::max_element(dist.begin(), dist.end());
        throw std::runtime_error("Frame width " + std::to_string(std::lround(fw / scale)) +
                                 " px reaches past the frame's middle: at most " +
                                 std::to_string(std::max(0L, std::lround(std::floor(middle - 1) / scale))) +
                                 " px here (0 = auto)");
    }
    cx /= n;
    cy /= n;
    constexpr int A = 720;  // rays, half a degree apart
    const float band = std::max(3.0f, fw * 0.15f);
    std::vector<float> ring(A);
    float reach = 0;
    for (int a = 0; a < A; ++a) {
        const double t = a * 6.283185307 / A;
        const float dx = float(std::cos(t)), dy = float(std::sin(t));
        float r = 0;
        for (;; r += 0.5f) {
            const float x = float(cx) + r * dx, y = float(cy) + r * dy;
            if (x < 0 || y < 0 || x > float(w - 1) || y > float(h - 1) || sample(dist, w, h, x, y) < fw) break;
        }
        ring[size_t(a)] = r;
        reach = std::max(reach, r + band);
    }
    const int R = int(reach) + 2;
    // Within the band around the ring, the edge strength along each ray, pulled towards the ring: harder from outside
    // it, so of two equal edges the inner one (the photo's, not the frame's) wins.
    constexpr float none = -1e9f;
    std::vector<float> score(size_t(A) * R, none);
    for (int a = 0; a < A; ++a) {
        const double t = a * 6.283185307 / A;
        const int lo = std::max(1, int(ring[size_t(a)] - band)), hi = std::min(R - 1, int(ring[size_t(a)] + band));
        for (int r = lo; r <= hi; ++r)
            score[size_t(a) * R + r] =
                sample(s, w, h, float(cx + r * std::cos(t)), float(cy + r * std::sin(t))) -
                (float(r) > ring[size_t(a)] ? 0.15f : 0.05f) * std::abs(float(r) - ring[size_t(a)]) / band;
    }
    // The closed path of radii with the most edge along it, changing by at most K per ray: two turns, the second
    // (settled) kept.
    const int K = 2 + int(0.012f * reach);
    std::vector<float> dp(score.begin(), score.begin() + R), next(dp.size());
    std::vector<std::int32_t> from(size_t(2 * A) * R);
    for (int t = 1; t < 2 * A; ++t) {
        const float* row = &score[size_t(t % A) * R];
        for (int r = 0; r < R; ++r) {
            next[size_t(r)] = none;
            from[size_t(t) * R + r] = r;
            if (row[r] <= none / 2) continue;
            float best = none;
            for (int q = std::max(0, r - K); q <= std::min(R - 1, r + K); ++q)
                if (const float v = dp[size_t(q)] - 0.01f * float(std::abs(q - r)); v > best) {
                    best = v;
                    from[size_t(t) * R + r] = q;
                }
            if (best > none / 2) next[size_t(r)] = best + row[r];
        }
        std::swap(dp, next);
    }
    int r = int(std::max_element(dp.begin(), dp.end()) - dp.begin());
    std::vector<int> radius(size_t(2 * A));
    for (int t = 2 * A - 1; t >= 0; --t) {
        radius[size_t(t)] = r;
        if (t) r = from[size_t(t) * R + r];
    }
    for (int a = 0; a < A; ++a) {
        const double t = a * 6.283185307 / A;
        const float rr = std::max(1.0f, float(radius[size_t(A + a)]) + grow);
        out.outline.push_back({float(cx + rr * std::cos(t)), float(cy + rr * std::sin(t))});
    }

    // The mask: inside the outline (pixel centres), softened by `feather`.
    Plane m(size_t(w) * h, 0);
    std::vector<float> xs;
    for (unsigned y = 0; y < h; ++y) {
        const float py = float(y) + 0.5f;
        xs.clear();
        for (size_t i = 0; i < out.outline.size(); ++i) {
            const auto& p = out.outline[i];
            const auto& q = out.outline[(i + 1) % out.outline.size()];
            if ((p[1] <= py) != (q[1] <= py)) xs.push_back(p[0] + (py - p[1]) * (q[0] - p[0]) / (q[1] - p[1]));
        }
        std::ranges::sort(xs);
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const int x0 = std::max(0, int(std::ceil(xs[k] - 0.5f))), x1 = std::min(int(w) - 1, int(std::floor(xs[k + 1] - 0.5f)));
            for (int x = x0; x <= x1; ++x) m[size_t(y) * w + size_t(x)] = 1;
        }
    }
    if (feather > 0) {  // inwards only: the frame stays untouched (user, 2026-10-02: no picture on the frame)
        const Plane hard = m;
        m = box_blur(std::move(m), w, h, std::max(1, int(std::lround(feather))), 2);
        for (size_t i = 0; i < m.size(); ++i) m[i] *= hard[i];
    }
    out.mask.resize(m.size());
    for (size_t i = 0; i < m.size(); ++i) out.mask[i] = std::uint8_t(std::lround(std::clamp(m[i], 0.0f, 1.0f) * 255));
    return out;
}

Bgra replace_photo(const Bgra& frame, const Bgra& picture, const std::vector<std::uint8_t>& mask, const Ageing& ageing,
                   float scale) {
    const unsigned w = frame.width, h = frame.height;
    if (mask.size() != size_t(w) * h) throw std::runtime_error("the mask doesn't match the frame image");
    unsigned x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
            if (mask[size_t(y) * w + x]) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    if (x0 > x1) return frame;  // no photo area
    const unsigned bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    const Bgra pic = resize_image(picture, bw, bh, Fit::Fill);  // end to end over the area
    Plane m(mask.size());
    for (size_t i = 0; i < m.size(); ++i) m[i] = mask[i] / 255.0f;
    const Plane old_l = luma(frame);

    // The old photo and the picture over the area: brightness mean and spread, colour cast, how colourful.
    struct Stats {
        double weight = 0, l = 0, l2 = 0, colourful = 0;
    } old_s, new_s;
    auto add = [](Stats& st, const std::uint8_t* p, float wt) {
        const double r = p[2] / 255.0, g = p[1] / 255.0, b = p[0] / 255.0, l = 0.299 * r + 0.587 * g + 0.114 * b;
        st.weight += wt;
        st.l += wt * l;
        st.l2 += wt * l * l;
        st.colourful += wt * (std::abs(r - l) + std::abs(g - l) + std::abs(b - l)) / 3;
    };
    for (unsigned y = y0; y <= y1; ++y)
        for (unsigned x = x0; x <= x1; ++x)
            if (const float wt = m[size_t(y) * w + x]; wt > 0) {
                add(old_s, &frame.pixels[(size_t(y) * w + x) * 4], wt);
                add(new_s, &pic.pixels[(size_t(y - y0) * bw + (x - x0)) * 4], wt);
            }
    auto mean = [](const Stats& st) { return float(st.l / st.weight); };
    auto spread = [&](const Stats& st) { return float(std::sqrt(std::max(0.0, st.l2 / st.weight - mean(st) * mean(st)))); };
    const float mu_old = mean(old_s), mu_new = mean(new_s);
    const float k_tone = spread(new_s) > 1e-4f ? spread(old_s) / spread(new_s) : 1;
    const float colourful_new = float(new_s.colourful / new_s.weight), colourful_old = float(old_s.colourful / old_s.weight);
    const float k_colour = colourful_new > 1e-4f ? std::min(1.0f, colourful_old / colourful_new) : 0;

    // The old photo's toning: its colour away from grey as a straight line over brightness (sepia is browner in the
    // shadows), fitted twice, the second time without the pixels far off the first line (stains), so stains don't
    // bend it and the toning that follows the old picture's content isn't taken for stains.
    float tone_a[3] = {0, 0, 0}, tone_b[3] = {0, 0, 0};
    auto cast = [&](int k, float l) { return tone_a[k] + tone_b[k] * l; };
    auto off_line = [&](size_t i) {
        const std::uint8_t* p = &frame.pixels[i * 4];
        float e = 0;
        for (int k = 0; k < 3; ++k) e += std::abs(p[2 - k] / 255.0f - old_l[i] - cast(k, old_l[i]));
        return e;
    };
    float limit = 1e9f;
    for (int pass = 0; pass < 2; ++pass) {
        double sw = 0, sl = 0, sll = 0, sc[3] = {0, 0, 0}, slc[3] = {0, 0, 0};
        for (unsigned y = y0; y <= y1; ++y)
            for (unsigned x = x0; x <= x1; ++x)
                if (const size_t i = size_t(y) * w + x; m[i] > 0 && off_line(i) <= limit) {
                    const double wt = m[i], l = old_l[i];
                    sw += wt, sl += wt * l, sll += wt * l * l;
                    for (int k = 0; k < 3; ++k) {
                        const double c = frame.pixels[i * 4 + 2 - k] / 255.0 - l;
                        sc[k] += wt * c, slc[k] += wt * l * c;
                    }
                }
        if (sw <= 0) break;
        const double var = sll / sw - (sl / sw) * (sl / sw);
        for (int k = 0; k < 3; ++k) {
            tone_b[k] = var > 1e-6 ? float((slc[k] / sw - (sl / sw) * (sc[k] / sw)) / var) : 0;
            tone_a[k] = float(sc[k] / sw - tone_b[k] * sl / sw);
        }
        std::vector<float> errors;
        for (unsigned y = y0; y <= y1; ++y)
            for (unsigned x = x0; x <= x1; ++x)
                if (const size_t i = size_t(y) * w + x; m[i] > 0) errors.push_back(off_line(i));
        const auto mid = errors.begin() + std::ptrdiff_t(errors.size() / 2);
        std::nth_element(errors.begin(), mid, errors.end());
        limit = 2.5f * *mid + 2.0f / 255;
    }

    // The old photo's shading: its brightness averaged by distance from the photo's edge (up to a quarter of its size)
    // and direction round it, relative to its mean. That keeps the frame's shadow along an edge and the fading at the
    // corners, while the old picture's own shapes (faces, clothes) average out.
    std::vector<bool> inside(mask.size());
    double mx = 0, my = 0, mn = 0;
    for (unsigned y = y0; y <= y1; ++y)
        for (unsigned x = x0; x <= x1; ++x)
            if (mask[size_t(y) * w + x] >= 128) inside[size_t(y) * w + x] = true, mx += x, my += y, mn += 1;
    const Plane from_edge = distance_inside(inside, w, h);
    constexpr int sectors = 36;
    const float reach = std::max(4.0f, 0.25f * float(std::min(bw, bh)));
    const int rings = 24;
    std::vector<double> sum(size_t(rings) * sectors), count(size_t(rings) * sectors);
    auto bin = [&](unsigned x, unsigned y, int& ring, int& sector) {
        ring = std::min(rings - 1, int(from_edge[size_t(y) * w + x] / reach * rings));
        const double angle = std::atan2(double(y) - my / mn, double(x) - mx / mn) + 3.141592653589793;
        sector = std::min(sectors - 1, int(angle / 6.283185307179586 * sectors));
    };
    for (unsigned y = y0; y <= y1; ++y)
        for (unsigned x = x0; x <= x1; ++x)
            if (const size_t i = size_t(y) * w + x; inside[i] && from_edge[i] < reach) {
                int ring = 0, sector = 0;
                bin(x, y, ring, sector);
                sum[size_t(ring) * sectors + sector] += old_l[i];
                count[size_t(ring) * sectors + sector] += 1;
            }
    // Smoothed over +-3 sectors (30 degrees) and +-1 ring, then relative to the innermost ring in the same direction:
    // only how the photo darkens towards its edge carries over, not the old picture's light and dark sides.
    std::vector<float> level(sum.size(), 0), ratio(sum.size(), 1);
    for (int ring = 0; ring < rings; ++ring)
        for (int sector = 0; sector < sectors; ++sector) {
            double total = 0, n = 0;
            for (int dr = -1; dr <= 1; ++dr)
                for (int ds = -3; ds <= 3; ++ds) {
                    const int r2 = ring + dr, s2 = (sector + ds + sectors) % sectors;
                    if (r2 < 0 || r2 >= rings) continue;
                    total += sum[size_t(r2) * sectors + s2];
                    n += count[size_t(r2) * sectors + s2];
                }
            if (n > 0) level[size_t(ring) * sectors + sector] = float(total / n);
        }
    for (int ring = 0; ring < rings; ++ring)
        for (int sector = 0; sector < sectors; ++sector)
            if (const float inner = level[size_t(rings - 1) * sectors + sector]; inner > 1e-3f)
                // Only darker: a frame casts shadow; a lighter edge is the old picture (a dark coat further in).
                ratio[size_t(ring) * sectors + sector] = std::min(1.0f, level[size_t(ring) * sectors + sector] / inner);
    // Scratches and specks: what a median removes (thinner than its window), minus the old picture's own fine texture
    // (residuals under twice their typical size are dropped), and only where the old picture is plain: eyes, buttons
    // and patterns sit among the picture's own edges (the median keeps those), specks on a plain area don't.
    Plane detail(mask.size(), 0);
    if (ageing.detail > 0) {
        const Plane med = median_inside(old_l, inside, w, x0, y0, x1, y1, std::max(1, int(std::lround(2 * scale))));
        auto typical = [](std::vector<float> v) {
            if (v.empty()) return 0.0f;
            const auto mid = v.begin() + std::ptrdiff_t(v.size() / 2);
            std::nth_element(v.begin(), mid, v.end());
            return *mid;
        };
        Plane busy(mask.size(), 0);  // the median's edge strength, spread a little
        std::vector<float> sizes, busies;
        for (unsigned y = y0; y <= y1; ++y)
            for (unsigned x = x0; x <= x1; ++x)
                if (const size_t i = size_t(y) * w + x; inside[i]) {
                    sizes.push_back(std::abs(detail[i] = old_l[i] - med[i]));
                    if (x > 0 && y > 0 && x + 1 < w && y + 1 < h)
                        busy[i] = std::abs(med[i + 1] - med[i - 1]) + std::abs(med[i + w] - med[i - w]);
                }
        busy = box_blur(std::move(busy), w, h, std::max(1, int(std::lround(3 * scale))));
        for (size_t i = 0; i < busy.size(); ++i)
            if (inside[i]) busies.push_back(busy[i]);
        const float core = std::max(1.5f / 255, 2 * typical(std::move(sizes)));
        const float plain = std::max(1e-3f, 2 * typical(std::move(busies)));
        for (size_t i = 0; i < detail.size(); ++i) {
            const float d = detail[i], b = busy[i] / plain;
            detail[i] = (d > 0 ? std::max(0.0f, d - core) : std::min(0.0f, d + core)) / (1 + b * b);
        }
    }
    // Stains: colour the toning doesn't explain, softened (blotches, not the old picture's fine colour), as a tint
    // that only takes light away (a stain is dye): (brightness + its colour) / its strongest channel.
    std::array<Plane, 3> stain;
    for (int k = 0; k < 3; ++k) {
        stain[size_t(k)].assign(mask.size(), 0);
        for (unsigned y = y0; y <= y1; ++y)
            for (unsigned x = x0; x <= x1; ++x)
                if (const size_t i = size_t(y) * w + x; m[i] > 0)
                    stain[size_t(k)][i] = frame.pixels[i * 4 + 2 - size_t(k)] / 255.0f - old_l[i] - cast(k, old_l[i]);
        stain[size_t(k)] = weighted_blur(stain[size_t(k)], m, w, h, std::max(1, int(std::lround(3 * scale))));
    }
    const Plane soft_l = weighted_blur(old_l, m, w, h, std::max(1, int(std::lround(3 * scale))));

    Bgra out = frame;
    for (unsigned y = y0; y <= y1; ++y)
        for (unsigned x = x0; x <= x1; ++x) {
            const size_t i = size_t(y) * w + x;
            const float a = m[i];
            if (a <= 0) continue;
            const std::uint8_t* np = &pic.pixels[(size_t(y - y0) * bw + (x - x0)) * 4];
            std::uint8_t* op = &out.pixels[i * 4];
            float rgb[3] = {np[2] / 255.0f, np[1] / 255.0f, np[0] / 255.0f};
            const float old[3] = {op[2] / 255.0f, op[1] / 255.0f, op[0] / 255.0f};
            const float l = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
            const float toned_l = (l - mu_new) * k_tone + mu_old;  // the old photo's brightness and contrast
            float shading = 1;  // fading back to 1 (no change) towards the reach
            if (from_edge[i] < reach) {
                // Between the four nearest bins (rings straight, sectors round the circle), so no band edges show.
                const float fr = std::clamp(from_edge[i] / reach * rings - 0.5f, 0.0f, float(rings - 1));
                const double angle = std::atan2(double(y) - my / mn, double(x) - mx / mn) + 3.141592653589793;
                const float fs = float(angle / 6.283185307179586 * sectors) - 0.5f;
                const int r0 = std::min(rings - 2, int(fr)), s0 = int(std::floor(fs));
                const float tr = fr - float(r0), ts = fs - float(s0);
                auto at = [&](int r, int sct) { return ratio[size_t(r) * sectors + size_t((sct % sectors + sectors) % sectors)]; };
                const float v = (at(r0, s0) * (1 - ts) + at(r0, s0 + 1) * ts) * (1 - tr) +
                                (at(r0 + 1, s0) * (1 - ts) + at(r0 + 1, s0 + 1) * ts) * tr;
                const float t = std::clamp(1 - from_edge[i] / reach, 0.0f, 1.0f);
                const float fade = t * t * (3 - 2 * t);  // eases out over the whole reach: no inner edge shows
                shading = 1 + (v - 1) * fade;
            }
            float tint[3];
            const float base = std::max(0.1f, soft_l[i]);
            for (int k = 0; k < 3; ++k) tint[k] = std::max(0.0f, 1 + stain[size_t(k)][i] / base);
            const float strongest = std::max({tint[0], tint[1], tint[2], 1e-4f});
            for (int k = 0; k < 3; ++k) {
                const float toned = toned_l + (rgb[k] - l) * k_colour + cast(k, toned_l);  // and its toning
                float c = rgb[k] + (toned - rgb[k]) * ageing.tone;
                c *= 1 + (shading - 1) * ageing.shading;
                c *= 1 + (tint[k] / strongest - 1) * ageing.stains;
                c += ageing.detail * detail[i];
                op[2 - k] = std::uint8_t(std::lround(std::clamp(old[k] + (c - old[k]) * a, 0.0f, 1.0f) * 255));
            }
        }
    return out;
}

void overlay_image(Bgra& base, const Bgra& top, int x, int y, float opacity) {
    opacity = std::clamp(opacity, 0.0f, 1.0f);
    for (unsigned ty = 0; ty < top.height; ++ty) {
        const long by = long(y) + long(ty);
        if (by < 0 || by >= long(base.height)) continue;
        for (unsigned tx = 0; tx < top.width; ++tx) {
            const long bx = long(x) + long(tx);
            if (bx < 0 || bx >= long(base.width)) continue;
            const std::uint8_t* t = &top.pixels[(size_t(ty) * top.width + tx) * 4];
            std::uint8_t* b = &base.pixels[(size_t(by) * base.width + size_t(bx)) * 4];
            const float a = t[3] / 255.0f * opacity;
            for (int k = 0; k < 3; ++k) b[k] = std::uint8_t(std::lround(b[k] * (1 - a) + t[k] * a));  // b[3] kept
        }
    }
}

}  // namespace remod
