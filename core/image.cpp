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
