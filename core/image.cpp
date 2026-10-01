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
        ComPtr<IWICBitmap> bitmap;  // WIC copies the pixels (WICBitmapCacheOnLoad)
        check(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, UINT(pixels.size()),
                                          const_cast<BYTE*>(pixels.data()), &bitmap),
              "reading " + images[i].string());
        const double scale = std::min(double(tile) / w, double(tile) / h);
        const UINT sw = std::max(1u, UINT(std::lround(w * scale))), sh = std::max(1u, UINT(std::lround(h * scale)));
        ComPtr<IWICBitmapScaler> scaler;  // output keeps the input's 32-bit BGRA
        check(wic->CreateBitmapScaler(&scaler), "CreateBitmapScaler");
        check(scaler->Initialize(bitmap.Get(), sw, sh, WICBitmapInterpolationModeFant), "scaling " + images[i].string());

        const unsigned x = (i % cols) * tile + (tile - sw) / 2, y = (i / cols) * tile + (tile - sh) / 2;
        std::uint8_t* dst = canvas.data() + (size_t(y) * width + x) * 4;
        const UINT stride = width * 4, bytes = (sh - 1) * stride + sw * 4;
        const WICRect all{0, 0, INT(sw), INT(sh)};
        check(scaler->CopyPixels(&all, stride, bytes, dst), "copying " + images[i].string());
    }
    encode_png(wic.Get(), out, width, height, canvas.data());
}

}  // namespace remod
