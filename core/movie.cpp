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
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
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
        if (handler == "soun" && info.audio.empty()) {
            info.audio = codec;
            // AudioSampleEntry: 8 reserved and reference bytes, 8 reserved, channels (16 bit), sample size, 4,
            // rate (16.16).
            if (const auto entry = stsd->payload + 8; !codec.empty() && r.be(entry, 4) >= 36) {
                info.audio_channels = unsigned(r.be(entry + 24, 2));
                info.audio_rate = unsigned(r.be(entry + 32, 4) >> 16);
            }
        }
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

namespace {

// A source reader decoding `video`'s first video (or audio) track only, with the video processor (scaling, colour
// conversion). Windows picks the reader by the file's extension, so a game movie (an MP4 named .mov.1.x64) is told
// what it is.
ComPtr<IMFSourceReader> open_reader(const std::filesystem::path& video,
                                    DWORD track = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM)) {
    std::string ext = video.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool known =
        ext == ".mp4" || ext == ".m4v" || ext == ".mov" || ext == ".wmv" || ext == ".avi" || ext == ".mkv";
    if (!known) try {
            read_mp4_info(video);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string(e.what()) + " (a game movie's playable copy is the one under "
                                     "natives\\STM\\streaming\\)");
        }
    ComPtr<IMFByteStream> stream;
    if (FAILED(MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, video.c_str(),
                            &stream)))
        throw std::runtime_error("can't open " + video.string());
    if (ComPtr<IMFAttributes> about; !known && SUCCEEDED(stream.As(&about)))
        about->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"video/mp4");
    ComPtr<IMFAttributes> processing;
    check(MFCreateAttributes(&processing, 1), "MFCreateAttributes");
    processing->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromByteStream(stream.Get(), processing.Get(), &reader)))
        throw std::runtime_error("Windows can't open " + video.filename().string() + " as a video");
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(reader->SetStreamSelection(track, TRUE)))
        throw std::runtime_error(video.filename().string() + " has no " +
                                 (track == DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM) ? "video" : "sound") + " track");
    return reader;
}

}  // namespace

std::string encode_movie(const std::filesystem::path& video, const MovieInfo& like, const std::string& title,
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

    // Sound, only where the original has it in the file (the logos, mva300 / 301). AAC as Windows' encoder takes it:
    // 44.1 or 48 kHz, 1, 2 or 6 channels, 16-bit PCM in. ponytail: 192 kbps always (its highest).
    const bool sound = !like.audio.empty();
    const UINT32 hz = like.audio_rate == 44100 ? 44100 : 48000;
    const UINT32 channels = like.audio_channels == 1 || like.audio_channels == 6 ? like.audio_channels : 2;
    auto audio_type = [&](const GUID& subtype) {
        ComPtr<IMFMediaType> t;
        check(MFCreateMediaType(&t), "MFCreateMediaType");
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE, subtype);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, hz);
        t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        if (subtype == MFAudioFormat_PCM) {
            t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
            t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, hz * channels * 2);
        } else {
            t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
        }
        return t;
    };
    DWORD audio = 0;
    if (sound) {
        check(writer->AddStream(audio_type(MFAudioFormat_AAC).Get(), &audio), "setting up the AAC encoder");
        check(writer->SetInputMediaType(audio, audio_type(MFAudioFormat_PCM).Get(), nullptr),
              "setting up the AAC encoder for " + std::to_string(hz) + " Hz, " + std::to_string(channels) +
                  " channels");
    }
    // Where the sound comes from: fills up to `frames` sample frames, returns how many (fewer: it has ended).
    std::function<size_t(std::int16_t*, size_t)> pcm;
    ComPtr<IMFSourceReader> sound_reader;
    std::vector<std::uint8_t> decoded;  // the sound reader's last sample, from `used` on
    size_t used = 0;
    bool sound_ended = false;
    std::string what_sound = sound ? "silence (your video has no sound)" : "";
    if (sound && video.empty()) {
        what_sound = "a beep each second";
        pcm = [&, n = std::uint64_t(0)](std::int16_t* to, size_t frames) mutable {
            for (size_t i = 0; i < frames; ++i, ++n) {
                const double beep = n % hz < hz / 10 ? 8000 * std::sin(6.283185307 * 1000 * double(n) / hz) : 0;
                for (UINT32 c = 0; c < channels; ++c) to[i * channels + c] = std::int16_t(beep);
            }
            return frames;
        };
    } else if (sound) {
        try {
            sound_reader = open_reader(video, DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM));
        } catch (const std::runtime_error&) {  // no sound track: silence
        }
        if (sound_reader) {
            if (FAILED(sound_reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr,
                                                         audio_type(MFAudioFormat_PCM).Get())))
                throw std::runtime_error("Windows can't convert " + video.filename().string() + "'s sound to " +
                                         std::to_string(hz) + " Hz, " + std::to_string(channels) + " channels");
            what_sound = "your video's sound";
            pcm = [&](std::int16_t* to, size_t frames) {
                const size_t want = frames * channels * 2;
                size_t got = 0;
                while (got < want && !sound_ended) {
                    if (used == decoded.size()) {
                        DWORD flags = 0;
                        LONGLONG at = 0;
                        ComPtr<IMFSample> sample;
                        check(sound_reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags,
                                                       &at, &sample),
                              "decoding " + video.filename().string() + "'s sound");
                        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) sound_ended = true;
                        if (!sample) continue;
                        ComPtr<IMFMediaBuffer> buffer;
                        check(sample->ConvertToContiguousBuffer(&buffer), "reading sound");
                        BYTE* data = nullptr;
                        DWORD length = 0;
                        check(buffer->Lock(&data, nullptr, &length), "reading sound");
                        decoded.assign(data, data + length);
                        buffer->Unlock();
                        used = 0;
                    }
                    const size_t take = std::min(want - got, decoded.size() - used);
                    std::memcpy(reinterpret_cast<std::uint8_t*>(to) + got, decoded.data() + used, take);
                    got += take;
                    used += take;
                }
                return got / (channels * 2);
            };
        }
    }
    check(writer->BeginWriting(), "starting to write " + out.string());

    // The sound up to `until` (100 ns units), in tenths of a second: the source's, then silence.
    std::uint64_t sound_written = 0;
    auto write_sound = [&](LONGLONG until) {
        if (!sound) return;
        const std::uint64_t target = std::uint64_t(until) * hz / 10'000'000;
        while (sound_written < target) {
            const size_t n = size_t(std::min<std::uint64_t>(target - sound_written, hz / 10));
            const DWORD bytes = DWORD(n * channels * 2);
            ComPtr<IMFMediaBuffer> buffer;
            check(MFCreateMemoryBuffer(bytes, &buffer), "MFCreateMemoryBuffer");
            BYTE* data = nullptr;
            check(buffer->Lock(&data, nullptr, nullptr), "locking sound");
            const size_t got = pcm ? pcm(reinterpret_cast<std::int16_t*>(data), n) : 0;
            std::memset(data + got * channels * 2, 0, bytes - got * channels * 2);
            buffer->Unlock();
            buffer->SetCurrentLength(bytes);
            ComPtr<IMFSample> sample;
            check(MFCreateSample(&sample), "MFCreateSample");
            sample->AddBuffer(buffer.Get());
            sample->SetSampleTime(LONGLONG(sound_written * 10'000'000 / hz));
            sample->SetSampleDuration(LONGLONG(n * 10'000'000 / hz));
            check(writer->WriteSample(audio, sample.Get()), "encoding sound");
            sound_written += n;
        }
    };

    auto write = [&](IMFMediaBuffer* buffer, LONGLONG frame) {
        ComPtr<IMFSample> sample;
        check(MFCreateSample(&sample), "MFCreateSample");
        sample->AddBuffer(buffer);
        sample->SetSampleTime(frame * period);
        sample->SetSampleDuration(period);
        check(writer->WriteSample(stream, sample.Get()), "encoding a frame");
        write_sound((frame + 1) * period);  // the sound alongside, up to this frame's end
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
        const ComPtr<IMFSourceReader> reader = open_reader(video);
        constexpr DWORD first = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
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
    return what_sound;
}

// ---- MoviePlayer ----

struct MoviePlayer::State {
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false, playing = false, ended = false, fresh = false;
    double seek_to = -1;  // a seek asked for, seconds; -1 none
    double position = 0;
    std::chrono::steady_clock::time_point start;  // when the movie's time 0 would have been shown, while playing
    Frame frame;
    MovieInfo info;
    std::string error;
};

namespace {

void play_movie(MoviePlayer::State& s, const std::filesystem::path& video, unsigned max_side) {
    using clock = std::chrono::steady_clock;
    MfScope mf;
    const ComPtr<IMFSourceReader> reader = open_reader(video);
    constexpr DWORD first = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFMediaType> native;
    check(reader->GetNativeMediaType(first, 0, &native), "reading " + video.filename().string() + "'s video");
    UINT32 w = 0, h = 0, num = 0, den = 1;
    MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &num, &den);
    if (!w || !h) throw std::runtime_error(video.filename().string() + " has no picture size");
    PROPVARIANT length;
    PropVariantInit(&length);
    reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &length);
    const double scale = std::min(1.0, double(max_side) / std::max(w, h));
    const UINT32 ow = std::max(2u, UINT32(w * scale) & ~1u), oh = std::max(2u, UINT32(h * scale) & ~1u);
    ComPtr<IMFMediaType> rgb;
    check(MFCreateMediaType(&rgb), "MFCreateMediaType");
    rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(rgb.Get(), MF_MT_FRAME_SIZE, ow, oh);
    if (FAILED(reader->SetCurrentMediaType(first, nullptr, rgb.Get())))
        throw std::runtime_error("Windows can't decode " + video.filename().string() + "'s video (codec missing?)");
    ComPtr<IMFMediaType> got;
    reader->GetCurrentMediaType(first, &got);
    LONG stride = LONG(MFGetAttributeUINT32(got.Get(), MF_MT_DEFAULT_STRIDE, 0));
    if (!stride) MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, ow, &stride);
    {
        std::lock_guard lock(s.mutex);
        s.info = {w, h, length.vt == VT_UI8 ? double(length.uhVal.QuadPart) / 1e7 : 0, den ? double(num) / den : 0};
    }
    PropVariantClear(&length);

    // The next frame, or nullptr at the end.
    auto read = [&](double& time) {
        for (ComPtr<IMFSample> sample;;) {
            DWORD flags = 0;
            LONGLONG at = 0;
            check(reader->ReadSample(first, 0, nullptr, &flags, &at, &sample), "decoding " + video.filename().string());
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return ComPtr<IMFSample>();
            if (!sample) continue;
            time = double(at) / 1e7;
            return sample;
        }
    };
    auto show = [&](IMFSample* sample, double time) {
        MoviePlayer::Frame f{ow, oh, std::vector<std::uint8_t>(size_t(ow) * oh * 4), time};
        ComPtr<IMFMediaBuffer> buffer;
        check(sample->ConvertToContiguousBuffer(&buffer), "reading a frame");
        ComPtr<IMF2DBuffer> two_d;
        BYTE* line = nullptr;
        LONG pitch = 0;
        const bool locked_2d = SUCCEEDED(buffer.As(&two_d)) && SUCCEEDED(two_d->Lock2D(&line, &pitch));
        if (!locked_2d) {
            BYTE* data = nullptr;
            check(buffer->Lock(&data, nullptr, nullptr), "reading a frame");
            pitch = stride;  // negative: bottom-up, the first row last
            line = stride < 0 ? data + size_t(oh - 1) * size_t(-stride) : data;
        }
        for (UINT32 y = 0; y < oh; ++y) {
            std::uint8_t* row = f.bgra.data() + size_t(y) * ow * 4;
            std::memcpy(row, line + std::ptrdiff_t(y) * pitch, size_t(ow) * 4);
            for (UINT32 x = 0; x < ow; ++x) row[x * 4 + 3] = 255;  // RGB32's fourth byte is padding
        }
        if (locked_2d) two_d->Unlock2D();
        else buffer->Unlock();
        std::lock_guard lock(s.mutex);
        s.frame = std::move(f);
        s.fresh = true;
        s.position = time;
    };

    double time = 0;
    if (const auto sample = read(time)) show(sample.Get(), time);
    for (;;) {
        std::unique_lock lock(s.mutex);
        s.wake.wait(lock, [&] { return s.stop || s.seek_to >= 0 || s.playing; });
        if (s.stop) return;
        if (s.seek_to >= 0) {
            const double to = s.seek_to;
            s.seek_to = -1;
            s.ended = false;
            lock.unlock();
            PROPVARIANT at;
            PropVariantInit(&at);
            at.vt = VT_I8;
            at.hVal.QuadPart = LONGLONG(to * 1e7);
            reader->SetCurrentPosition(GUID_NULL, at);  // lands on the key frame before: decode on to the time
            const double frame = den && num ? double(den) / num : 1.0 / 30;
            ComPtr<IMFSample> shown;
            double shown_time = 0;
            while (const auto sample = read(time)) {
                shown = sample;
                shown_time = time;
                if (time + frame > to) break;
            }
            if (shown) show(shown.Get(), shown_time);
            lock.lock();
            s.start = clock::now() -
                      std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(s.position));
            continue;
        }
        lock.unlock();
        const auto sample = read(time);
        lock.lock();
        if (!sample) {  // the end: paused on the last frame
            s.playing = false;
            s.ended = true;
            continue;
        }
        // Shown at its time. ponytail: a video decoding slower than it plays (4K in software) falls behind the clock
        // and plays at decoding speed; frames aren't dropped.
        const auto due = s.start + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(time));
        if (s.wake.wait_until(lock, due, [&] { return s.stop || s.seek_to >= 0 || !s.playing; })) {
            if (s.stop) return;
            if (s.seek_to >= 0) continue;  // the seek decides what shows
        }
        lock.unlock();
        show(sample.Get(), time);
    }
}

}  // namespace

MoviePlayer::MoviePlayer(const std::filesystem::path& video, unsigned max_side) : state_(std::make_unique<State>()) {
    thread_ = std::thread([s = state_.get(), video, max_side] {
        try {
            play_movie(*s, video, max_side);
        } catch (const std::exception& e) {
            std::lock_guard lock(s->mutex);
            s->error = e.what();
        }
    });
}

MoviePlayer::~MoviePlayer() {
    {
        std::lock_guard lock(state_->mutex);
        state_->stop = true;
    }
    state_->wake.notify_all();
    thread_.join();
}

bool MoviePlayer::take(Frame& frame) {
    std::lock_guard lock(state_->mutex);
    if (!state_->fresh) return false;
    state_->fresh = false;
    std::swap(frame, state_->frame);
    return true;
}

void MoviePlayer::play() {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->ended) state_->seek_to = 0;
        state_->playing = true;
        state_->start = std::chrono::steady_clock::now() -
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(state_->ended ? 0 : state_->position));
    }
    state_->wake.notify_all();
}

void MoviePlayer::pause() {
    {
        std::lock_guard lock(state_->mutex);
        state_->playing = false;
    }
    state_->wake.notify_all();
}

void MoviePlayer::seek(double seconds) {
    {
        std::lock_guard lock(state_->mutex);
        state_->seek_to = std::max(0.0, seconds);
    }
    state_->wake.notify_all();
}

bool MoviePlayer::playing() const {
    std::lock_guard lock(state_->mutex);
    return state_->playing;
}

double MoviePlayer::position() const {
    std::lock_guard lock(state_->mutex);
    return state_->position;
}

MovieInfo MoviePlayer::info() const {
    std::lock_guard lock(state_->mutex);
    return state_->info;
}

std::string MoviePlayer::error() const {
    std::lock_guard lock(state_->mutex);
    return state_->error;
}

}  // namespace remod
