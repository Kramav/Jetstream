#include "movie.hpp"

#include <array>
#include <cstdint>
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
    return info;
}

}  // namespace remod
