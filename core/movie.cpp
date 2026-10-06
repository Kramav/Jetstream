#include "movie.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace remod {

namespace {

// Big-endian reads at an offset.
struct Reader {
    std::ifstream in;
    std::uint64_t size = 0;
    std::uint64_t be(std::uint64_t at, int bytes) {
        std::array<unsigned char, 8> b{};
        in.clear();
        in.seekg(std::streamoff(at));
        in.read(reinterpret_cast<char*>(b.data()), bytes);
        if (in.gcount() != bytes) throw std::runtime_error("the file ends inside a box");
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v = v << 8 | b[size_t(i)];
        return v;
    }
    std::string fourcc(std::uint64_t at) {
        const auto v = std::uint32_t(be(at, 4));
        return {char(v >> 24), char(v >> 16 & 0xff), char(v >> 8 & 0xff), char(v & 0xff)};
    }
};

struct Box {
    std::string type;
    std::uint64_t start, payload, end;  // payload: after the header
};

// The boxes directly inside [from, to).
std::vector<Box> boxes(Reader& r, std::uint64_t from, std::uint64_t to) {
    std::vector<Box> out;
    for (std::uint64_t at = from; at + 8 <= to;) {
        std::uint64_t size = r.be(at, 4), header = 8;
        if (size == 1) {
            size = r.be(at + 8, 8);
            header = 16;
        } else if (size == 0) {
            size = to - at;  // to the end
        }
        if (size < header || at + size > to) throw std::runtime_error("a box runs past its container");
        out.push_back({r.fourcc(at + 4), at, at + header, at + size});
        at += size;
    }
    return out;
}

void check(HRESULT hr, const std::string& what) {
    if (FAILED(hr)) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%08lX", static_cast<unsigned long>(hr));
        throw std::runtime_error(what + " failed (" + hex + ")");
    }
}

// COM and Media Foundation for the calling thread, for one call (as image.cpp's ComScope; MFStartup counts).
struct MfScope {
    bool com = false;
    MfScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr != RPC_E_CHANGED_MODE) check(hr, "CoInitializeEx");
        com = SUCCEEDED(hr);
        check(MFStartup(MF_VERSION), "starting Media Foundation");
    }
    ~MfScope() {
        MFShutdown();
        if (com) CoUninitialize();
    }
};

const Box* find(const std::vector<Box>& list, const char* type) {
    for (const Box& b : list)
        if (b.type == type) return &b;
    return nullptr;
}

}  // namespace

MovieInfo read_mp4_info(const std::filesystem::path& file) {
    Reader r{std::ifstream(file, std::ios::binary), std::filesystem::file_size(file)};
    if (!r.in) throw std::runtime_error("can't read " + file.string());
    if (r.size < 8 || r.fourcc(4) != "ftyp")  // e.g. the game's 38-byte REMV stub beside a streaming movie
        throw std::runtime_error(file.filename().string() + " isn't an MP4");
    const auto top = boxes(r, 0, r.size);
    const Box* moov = find(top, "moov");
    if (!moov) throw std::runtime_error(file.filename().string() + " has no track index (moov box)");

    MovieInfo info;
    for (const Box& trak : boxes(r, moov->payload, moov->end)) {
        if (trak.type != "trak") continue;
        const auto in_trak = boxes(r, trak.payload, trak.end);
        const Box* tkhd = find(in_trak, "tkhd");
        const Box* mdia = find(in_trak, "mdia");
        if (!tkhd || !mdia) continue;
        const auto in_mdia = boxes(r, mdia->payload, mdia->end);
        const Box *mdhd = find(in_mdia, "mdhd"), *hdlr = find(in_mdia, "hdlr"), *minf = find(in_mdia, "minf");
        if (!mdhd || !hdlr || !minf) continue;
        const auto in_minf = boxes(r, minf->payload, minf->end);
        const Box* stbl = find(in_minf, "stbl");
        if (!stbl) continue;
        const auto in_stbl = boxes(r, stbl->payload, stbl->end);
        const Box *stsd = find(in_stbl, "stsd"), *stts = find(in_stbl, "stts");
        const std::string handler = r.fourcc(hdlr->payload + 8);
        const std::string codec = stsd && r.be(stsd->payload + 4, 4) > 0 ? r.fourcc(stsd->payload + 12) : "";
        if (handler == "soun" && info.audio.empty()) info.audio = codec;
        if (handler != "vide" || !info.video.empty()) continue;
        info.video = codec;
        const bool v1 = r.be(tkhd->payload, 1) == 1;  // 64-bit times
        info.width = unsigned(r.be(tkhd->payload + (v1 ? 88 : 76), 4) >> 16);  // 16.16 fixed point
        info.height = unsigned(r.be(tkhd->payload + (v1 ? 92 : 80), 4) >> 16);
        const bool m1 = r.be(mdhd->payload, 1) == 1;
        const auto timescale = r.be(mdhd->payload + (m1 ? 20 : 12), 4);
        const auto duration = r.be(mdhd->payload + (m1 ? 24 : 16), m1 ? 8 : 4);
        if (timescale) info.seconds = double(duration) / double(timescale);
        if (stts && info.seconds > 0) {
            std::uint64_t frames = 0;
            const auto entries = r.be(stts->payload + 4, 4);
            for (std::uint64_t i = 0; i < entries; ++i) frames += r.be(stts->payload + 8 + i * 8, 4);
            info.fps = double(frames) / info.seconds;
        }
    }
    if (info.seconds > 0) info.bitrate = std::uint64_t(double(r.size) * 8 / info.seconds);
    return info;
}

using Microsoft::WRL::ComPtr;

void encode_movie(const std::filesystem::path& video, const MovieInfo& like, const std::string& title,
                  const std::filesystem::path& out, bool same_length) {
    if (!like.width || !like.height || like.fps <= 0) throw std::runtime_error("the movie to match has no video track");
    MfScope mf;
    // The frame rate as a ratio: whole, or NTSC's x/1.001 (the game's 29.97).
    // ponytail: those two kinds only; other fractional rates round to whole frames.
    UINT32 num = UINT32(std::lround(like.fps)), den = 1;
    if (const double ntsc = like.fps * 1.001;
        std::abs(like.fps - num) > 0.005 && std::abs(ntsc - std::round(ntsc)) < 0.005)
        num = UINT32(std::lround(ntsc)) * 1000, den = 1001;
    const LONGLONG period = LONGLONG(10'000'000.0 * den / num);  // 100 ns units
    const UINT32 bitrate = UINT32(std::clamp<std::uint64_t>(like.bitrate, 1'000'000, 200'000'000));
    const GUID input = video.empty() ? MFVideoFormat_RGB32 : MFVideoFormat_NV12;
    const LONGLONG frames = std::llround(like.seconds * num / den);  // the original's

    ComPtr<IMFAttributes> options;
    check(MFCreateAttributes(&options, 2), "MFCreateAttributes");
    options->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);  // whatever the file's name
    options->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    ComPtr<IMFSinkWriter> writer;
    check(MFCreateSinkWriterFromURL(out.c_str(), nullptr, options.Get(), &writer), "creating " + out.string());
    auto video_type = [&](const GUID& subtype) {
        ComPtr<IMFMediaType> t;
        check(MFCreateMediaType(&t), "MFCreateMediaType");
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, subtype);
        t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, like.width, like.height);
        MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, num, den);
        MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        return t;
    };
    const ComPtr<IMFMediaType> h264 = video_type(MFVideoFormat_H264);
    h264->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    h264->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);  // as the game's movies
    DWORD stream = 0;
    check(writer->AddStream(h264.Get(), &stream), "setting up the H.264 encoder");
    ComPtr<IMFAttributes> rate;  // variable bit rate around the original's: a still test card stays small
    check(MFCreateAttributes(&rate, 2), "MFCreateAttributes");
    rate->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_UnconstrainedVBR);
    rate->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, bitrate);
    check(writer->SetInputMediaType(stream, video_type(input).Get(), rate.Get()),
          "setting up the H.264 encoder for " + std::to_string(like.width) + "x" + std::to_string(like.height));
    check(writer->BeginWriting(), "starting to write " + out.string());

    auto write = [&](IMFMediaBuffer* buffer, LONGLONG frame) {
        ComPtr<IMFSample> sample;
        check(MFCreateSample(&sample), "MFCreateSample");
        sample->AddBuffer(buffer);
        sample->SetSampleTime(frame * period);
        sample->SetSampleDuration(period);
        check(writer->WriteSample(stream, sample.Get()), "encoding a frame");
    };

    if (video.empty()) {
        // The card: drawn with GDI into a bottom-up DIB (MF's RGB32 is bottom-up too), again each second.
        struct Gdi {  // released however this ends
            HDC dc = CreateCompatibleDC(nullptr);
            HBITMAP bmp = nullptr;
            HFONT title_font = nullptr, time_font = nullptr;
            ~Gdi() { DeleteObject(title_font), DeleteObject(time_font), DeleteObject(bmp), DeleteDC(dc); }
        } gdi;
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), LONG(like.width), LONG(like.height), 1, 32, BI_RGB};
        void* bits = nullptr;
        gdi.bmp = CreateDIBSection(gdi.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        auto font = [](int px) {
            return CreateFontW(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        };
        gdi.title_font = font(int(like.height / 5));
        gdi.time_font = font(int(like.height / 14));
        if (!gdi.bmp || !gdi.title_font || !gdi.time_font) throw std::runtime_error("can't draw the test card");
        const HDC dc = gdi.dc;
        SelectObject(dc, gdi.bmp);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        const std::wstring name(title.begin(), title.end());  // ponytail: ASCII (movie ids)
        const DWORD bytes = like.width * like.height * 4;
        const int total = int(std::lround(like.seconds));
        ComPtr<IMFMediaBuffer> buffer;
        for (LONGLONG k = 0, shown = -1; k < frames; ++k) {
            if (const LONGLONG second = k * den / num; second != shown) {
                shown = second;
                RECT all{0, 0, LONG(like.width), LONG(like.height)};
                const HBRUSH back = CreateSolidBrush(RGB(20, 40, 90));
                FillRect(dc, &all, back);
                DeleteObject(back);
                RECT top = all, bottom = all;
                top.bottom = bottom.top = LONG(like.height * 55 / 100);
                SelectObject(dc, gdi.title_font);
                DrawTextW(dc, name.c_str(), -1, &top, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
                SelectObject(dc, gdi.time_font);
                const std::wstring time = std::to_wstring(second) + L" / " + std::to_wstring(total) + L" s";
                DrawTextW(dc, time.c_str(), -1, &bottom, DT_CENTER | DT_TOP | DT_SINGLELINE);
                GdiFlush();
                check(MFCreateMemoryBuffer(bytes, &buffer), "MFCreateMemoryBuffer");
                BYTE* data = nullptr;
                check(buffer->Lock(&data, nullptr, nullptr), "locking a frame");
                std::memcpy(data, bits, bytes);
                buffer->Unlock();
                buffer->SetCurrentLength(bytes);
            }
            write(buffer.Get(), k);  // a frame within the same second shares its buffer
        }
    } else {
        // The video, decoded and scaled by the source reader's video processor (it keeps the shape: black bars).
        ComPtr<IMFAttributes> processing;
        check(MFCreateAttributes(&processing, 1), "MFCreateAttributes");
        processing->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        ComPtr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(video.c_str(), processing.Get(), &reader)))
            throw std::runtime_error("Windows can't open " + video.filename().string() + " as a video");
        constexpr DWORD first = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        if (FAILED(reader->SetStreamSelection(first, TRUE)))
            throw std::runtime_error(video.filename().string() + " has no video track");
        if (FAILED(reader->SetCurrentMediaType(first, nullptr, video_type(MFVideoFormat_NV12).Get())))
            throw std::runtime_error("Windows can't decode " + video.filename().string() + "'s video (codec missing?)");
        // Frame k shows the last frame starting at or before k's time: repeated or dropped to the frame rate. After
        // the video's end (same_length) its last frame stays.
        ComPtr<IMFSample> shown, ahead;
        LONGLONG ahead_time = 0, end = 0;
        auto pull = [&] {
            for (ahead.Reset();;) {
                DWORD flags = 0;
                LONGLONG time = 0;
                check(reader->ReadSample(first, 0, nullptr, &flags, &time, &ahead),
                      "decoding " + video.filename().string());
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return;
                if (!ahead) continue;  // a gap
                LONGLONG length = 0;
                ahead->GetSampleDuration(&length);
                ahead_time = time;
                end = std::max(end, time + std::max(length, period));
                return;
            }
        };
        pull();
        if (!ahead) throw std::runtime_error(video.filename().string() + " has no frames");
        const LONGLONG start = ahead_time;
        for (LONGLONG k = 0; same_length ? k < frames : ahead || start + k * period + period / 2 < end; ++k) {
            while (ahead && ahead_time <= start + k * period) {
                shown = ahead;
                pull();
            }
            ComPtr<IMFMediaBuffer> buffer;
            check(shown->ConvertToContiguousBuffer(&buffer), "reading a frame");
            write(buffer.Get(), k);
        }
    }
    check(writer->Finalize(), "finishing " + out.string());
}

}  // namespace remod
