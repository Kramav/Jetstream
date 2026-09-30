#include "image.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

}  // namespace

void save_png_bgra(const fs::path& out, unsigned width, unsigned height, const std::vector<std::uint8_t>& bgra) {
    if (bgra.size() != size_t(width) * height * 4) throw std::runtime_error("save_png_bgra: wrong pixel count");
    ComScope com;
    encode_png(factory().Get(), out, width, height, bgra.data());
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
        ComPtr<IWICBitmapDecoder> decoder;
        check(wic->CreateDecoderFromFilename(images[i].c_str(), nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnDemand, &decoder),
              "reading " + images[i].string());
        ComPtr<IWICBitmapFrameDecode> frame;
        check(decoder->GetFrame(0, &frame), "reading " + images[i].string());
        UINT w = 0, h = 0;
        check(frame->GetSize(&w, &h), "GetSize");
        const double scale = std::min(double(tile) / w, double(tile) / h);
        const UINT sw = std::max(1u, UINT(std::lround(w * scale))), sh = std::max(1u, UINT(std::lround(h * scale)));

        ComPtr<IWICBitmapScaler> scaler;
        check(wic->CreateBitmapScaler(&scaler), "CreateBitmapScaler");
        check(scaler->Initialize(frame.Get(), sw, sh, WICBitmapInterpolationModeFant), "scaling " + images[i].string());
        ComPtr<IWICFormatConverter> bgra;
        check(wic->CreateFormatConverter(&bgra), "CreateFormatConverter");
        check(bgra->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                               WICBitmapPaletteTypeCustom),
              "converting " + images[i].string());

        const unsigned x = (i % cols) * tile + (tile - sw) / 2, y = (i / cols) * tile + (tile - sh) / 2;
        std::uint8_t* dst = canvas.data() + (size_t(y) * width + x) * 4;
        const UINT stride = width * 4, bytes = (sh - 1) * stride + sw * 4;
        const WICRect all{0, 0, INT(sw), INT(sh)};
        check(bgra->CopyPixels(&all, stride, bytes, dst), "copying " + images[i].string());
    }
    encode_png(wic.Get(), out, width, height, canvas.data());
}

}  // namespace remod
