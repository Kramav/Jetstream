#include "wwise.hpp"

#include "profile.hpp"

#include <opus/opus_multistream.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <utility>

namespace remod {

namespace {

namespace fs = std::filesystem;
using Bytes = std::string_view;

[[noreturn]] void fail(const std::string& why) { throw std::runtime_error(why); }

template <class T>
T get(Bytes b, size_t at) {
    if (at > b.size() || b.size() - at < sizeof(T)) fail("a sound file ends too soon");
    T v;
    std::memcpy(&v, b.data() + at, sizeof v);
    return v;
}
template <class T>
void put(std::string& b, T v) {
    b.append(reinterpret_cast<const char*>(&v), sizeof v);
}
template <class T>
void put_at(std::string& b, size_t at, T v) {
    std::memcpy(b.data() + at, &v, sizeof v);
}

// ---- RIFF (the WEM container) ----

struct Riff {
    Bytes fmt, seek, data;
};

Riff chunks(Bytes w) {
    if (w.substr(0, 4) != "RIFF" || w.substr(8, 4) != "WAVE") fail("not a WEM sound (RIFF WAVE)");
    const size_t end = std::min<size_t>(w.size(), size_t(get<std::uint32_t>(w, 4)) + 8);
    Riff r;
    for (size_t at = 12; at + 8 <= end;) {
        const Bytes tag = w.substr(at, 4);
        size_t size = get<std::uint32_t>(w, at + 4);
        if (size > end - at - 8) {
            if (tag != "data") fail("a WEM chunk runs past the file's end");
            size = end - at - 8;  // a streamed sound's first part (a bank's prefetch): its header is whole
        }
        const Bytes body = w.substr(at + 8, size);
        if (tag == "fmt ") r.fmt = body;
        if (tag == "seek") r.seek = body;
        if (tag == "data") r.data = body;
        at += 8 + size + (size & 1);
    }
    if (r.fmt.size() < 18 || r.data.empty()) fail("a WEM without its fmt or data chunk");
    return r;
}

// RIFF WAVE with these chunks, unpadded (as Wwise writes them: only the last can be odd-sized).
std::string riff(std::initializer_list<std::pair<const char*, const std::string*>> parts) {
    std::string body = "WAVE";
    for (const auto& [tag, data] : parts) {
        body += tag;
        put(body, std::uint32_t(data->size()));
        body += *data;
    }
    std::string out = "RIFF";
    put(out, std::uint32_t(body.size()));
    return out + body;
}

// ---- Bits, in Vorbis's order (least significant first) ----

struct BitIn {
    Bytes b;
    size_t pos = 0;
    std::uint32_t get(int n) {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i, ++pos) {
            if ((pos >> 3) >= b.size()) fail("a Vorbis setup or packet ends too soon");
            v |= std::uint32_t((std::uint8_t(b[pos >> 3]) >> (pos & 7)) & 1) << i;
        }
        return v;
    }
};

struct BitOut {
    std::string b;
    size_t pos = 0;
    void put(std::uint32_t v, int n) {
        for (int i = 0; i < n; ++i, ++pos) {
            if ((pos >> 3) >= b.size()) b.push_back('\0');
            if (v >> i & 1) b[pos >> 3] = char(std::uint8_t(b[pos >> 3]) | 1u << (pos & 7));
        }
    }
    std::uint32_t copy(BitIn& from, int n) {
        const std::uint32_t v = from.get(n);
        put(v, n);
        return v;
    }
    void copy_rest(BitIn& from) {
        for (const size_t end = from.b.size() * 8; from.pos < end;) {
            const int n = int(std::min<size_t>(32, end - from.pos));
            put(from.get(n), n);
        }
    }
};

int ilog(std::uint32_t x) {
    int n = 0;
    for (; x; x >>= 1) ++n;
    return n;
}

// libvorbis's _book_maptype1_quantvals: the largest v with v^dims <= entries.
std::uint32_t quantvals(std::uint32_t entries, std::uint32_t dims) {
    auto v = std::uint32_t(std::floor(std::pow(double(entries), 1.0 / dims)));
    for (;;) {
        std::uint64_t acc = 1, acc1 = 1;
        for (std::uint32_t i = 0; i < dims; ++i) acc *= v, acc1 *= v + 1;
        if (acc <= entries && acc1 > entries) return v;
        acc > entries ? --v : ++v;
    }
}

// ---- Wwise Vorbis: the codebook library and the setup ----

// Wwise's Vorbis files name their codebooks by id in this library (ww2ogg's packed_codebooks_aoTuV_603.bin, BSD):
// each codebook in a compact form, then a table of their offsets, the table's own offset last.
struct Codebooks {
    std::string data;
    std::vector<std::uint32_t> offsets;

    explicit Codebooks(const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) fail("can't read Wwise's codebook library " + file.string());
        data.assign(std::istreambuf_iterator<char>(in), {});
        const auto table = data.size() >= 4 ? get<std::uint32_t>(data, data.size() - 4) : std::uint32_t(-1);
        if (table >= data.size()) fail(file.string() + " isn't Wwise's codebook library");
        for (size_t at = table; at + 4 <= data.size(); at += 4) offsets.push_back(get<std::uint32_t>(data, at));
    }

    // Codebook `id` in Vorbis's own form.
    void rebuild(std::uint32_t id, BitOut& w) const {
        if (size_t(id) + 1 >= offsets.size() || offsets[id] > offsets[id + 1])
            fail("a codebook Wwise's library doesn't have (" + std::to_string(id) + ")");
        BitIn r{Bytes(data).substr(offsets[id], offsets[id + 1] - offsets[id])};
        const std::uint32_t dims = r.get(4), entries = r.get(14);
        w.put(0x564342, 24);  // "BCV"
        w.put(dims, 16);
        w.put(entries, 24);
        if (w.copy(r, 1)) {  // ordered: lengths as runs
            w.copy(r, 5);
            for (std::uint32_t cur = 0, guard = 0; cur < entries; ++guard) {
                if (guard > 64) fail("a damaged codebook in Wwise's library");
                cur += w.copy(r, ilog(entries - cur));
            }
        } else {
            const int length_bits = int(r.get(3));
            const std::uint32_t sparse = w.copy(r, 1);
            if (length_bits == 0 || length_bits > 5) fail("a damaged codebook in Wwise's library");
            for (std::uint32_t i = 0; i < entries; ++i)
                if (!sparse || w.copy(r, 1)) w.put(r.get(length_bits), 5);
        }
        const std::uint32_t lookup = r.get(1);
        w.put(lookup, 4);
        if (lookup) {  // type 1, the only one Wwise stores
            w.copy(r, 32), w.copy(r, 32);
            const int bits = int(w.copy(r, 4)) + 1;
            w.copy(r, 1);
            for (std::uint32_t i = 0, n = quantvals(entries, dims); i < n; ++i) w.copy(r, bits);
        }
    }
};

// A Wwise Vorbis setup block in Vorbis's own form (the setup header packet, "\5vorbis..."), as ww2ogg rebuilds it:
// Wwise names its codebooks by id and leaves out the time domain, the floor, mapping and mode types and the framing.
struct Setup {
    std::string packet;
    std::vector<bool> long_mode;  // each mode's block flag
    int mode_bits = 0;
};

Setup expand_setup(Bytes payload, unsigned channels, const Codebooks& books) {
    BitIn r{payload};
    BitOut w;
    w.put(5, 8);
    for (const char c : Bytes("vorbis")) w.put(std::uint8_t(c), 8);
    const std::uint32_t codebooks = w.copy(r, 8) + 1;
    for (std::uint32_t i = 0; i < codebooks; ++i) books.rebuild(r.get(10), w);
    w.put(0, 6), w.put(0, 16);  // one time domain transform, unused
    const std::uint32_t floors = w.copy(r, 6) + 1;
    for (std::uint32_t f = 0; f < floors; ++f) {
        w.put(1, 16);  // floor type 1, the only one Wwise uses
        const std::uint32_t parts = w.copy(r, 5);
        std::vector<std::uint32_t> classes(parts);
        std::uint32_t top = 0;
        for (auto& c : classes) top = std::max(top, c = w.copy(r, 4));
        std::vector<std::uint32_t> dims;
        for (std::uint32_t c = 0; parts && c <= top; ++c) {
            dims.push_back(w.copy(r, 3) + 1);
            const std::uint32_t sub = w.copy(r, 2);
            if (sub) w.copy(r, 8);
            for (std::uint32_t k = 0; k < (1u << sub); ++k) w.copy(r, 8);
        }
        w.copy(r, 2);
        const int range = int(w.copy(r, 4));
        for (const std::uint32_t c : classes)
            for (std::uint32_t k = 0; k < dims[c]; ++k) w.copy(r, range);
    }
    const std::uint32_t residues = w.copy(r, 6) + 1;
    for (std::uint32_t i = 0; i < residues; ++i) {
        w.put(r.get(2), 16);  // its type: 2 bits in Wwise's form
        w.copy(r, 24), w.copy(r, 24), w.copy(r, 24);
        const std::uint32_t classes = w.copy(r, 6) + 1;
        w.copy(r, 8);
        std::vector<std::uint32_t> cascade;
        for (std::uint32_t c = 0; c < classes; ++c) {
            const std::uint32_t low = w.copy(r, 3);
            const std::uint32_t high = w.copy(r, 1) ? w.copy(r, 5) : 0;
            cascade.push_back(high << 3 | low);
        }
        for (const std::uint32_t x : cascade)
            for (int k = 0; k < 8; ++k)
                if (x >> k & 1) w.copy(r, 8);
    }
    const std::uint32_t mappings = w.copy(r, 6) + 1;
    for (std::uint32_t i = 0; i < mappings; ++i) {
        w.put(0, 16);  // mapping type 0
        const std::uint32_t submaps = w.copy(r, 1) ? w.copy(r, 4) + 1 : 1;
        if (w.copy(r, 1)) {  // channel coupling
            const std::uint32_t steps = w.copy(r, 8) + 1;
            const int bits = ilog(channels - 1);
            for (std::uint32_t s = 0; s < steps; ++s) w.copy(r, bits), w.copy(r, bits);
        }
        w.copy(r, 2);  // reserved
        if (submaps > 1)
            for (unsigned c = 0; c < channels; ++c) w.copy(r, 4);
        for (std::uint32_t s = 0; s < submaps; ++s) w.copy(r, 8), w.copy(r, 8), w.copy(r, 8);
    }
    Setup s;
    const std::uint32_t modes = w.copy(r, 6) + 1;
    for (std::uint32_t i = 0; i < modes; ++i) {
        s.long_mode.push_back(w.copy(r, 1) != 0);
        w.put(0, 16), w.put(0, 16);  // window and transform types
        w.copy(r, 8);
    }
    w.put(1, 1);  // framing
    s.packet = std::move(w.b);
    s.mode_bits = ilog(modes - 1);
    return s;
}

std::uint32_t mode_of(Bytes wwise_packet, const Setup& s) {
    const std::uint32_t mode = BitIn{wwise_packet}.get(s.mode_bits);
    if (mode >= s.long_mode.size()) fail("a Vorbis packet with an unknown mode");
    return mode;
}

// libvorbis's audio packet in Wwise's form: without the packet-type bit, and a long block's two window bits.
// ponytail: libvorbis's packets end on a byte, so the true bit count is lost: a packet can come out one zero byte
// longer than Wwise's (decoders ignore it).
std::string to_wwise_packet(Bytes packet, const Setup& s) {
    BitIn r{packet};
    if (r.get(1)) fail("libvorbis gave a header where audio was expected");
    const std::uint32_t mode = r.get(s.mode_bits);
    if (mode >= s.long_mode.size()) fail("libvorbis gave a packet with an unknown mode");
    if (s.long_mode[mode]) r.get(2);
    BitOut w;
    w.put(mode, s.mode_bits);
    w.copy_rest(r);
    return std::move(w.b);
}

// Wwise's audio packet in Vorbis's form: the packet-type bit back, and a long block's window bits (the neighbours').
std::string from_wwise_packet(Bytes packet, const Setup& s, bool prev_long, bool next_long) {
    BitIn r{packet};
    const std::uint32_t mode = r.get(s.mode_bits);
    if (mode >= s.long_mode.size()) fail("a Vorbis packet with an unknown mode");
    BitOut w;
    w.put(0, 1);
    w.put(mode, s.mode_bits);
    if (s.long_mode[mode]) w.put(prev_long, 1), w.put(next_long, 1);
    w.copy_rest(r);
    return std::move(w.b);
}

struct VorbisWem {
    Bytes extra, payload;  // fmt's 48 extra bytes; the setup block's payload
    std::vector<Bytes> packets;
};

VorbisWem vorbis_parts(const Riff& r) {
    VorbisWem v{r.fmt.substr(18), {}, {}};
    if (v.extra.size() < 48) fail("a Wwise Vorbis header of an unknown size");
    const auto table = get<std::uint32_t>(v.extra, 22), audio = get<std::uint32_t>(v.extra, 26);
    v.payload = r.data.substr(std::min<size_t>(table + 2, r.data.size()), get<std::uint16_t>(r.data, table));
    for (size_t at = audio; at + 2 <= r.data.size();) {
        const size_t n = get<std::uint16_t>(r.data, at);
        if (n > r.data.size() - at - 2) fail("a Vorbis packet runs past the WEM's end");
        v.packets.push_back(r.data.substr(at + 2, n));
        at += 2 + n;
    }
    return v;
}

class VorbisEncoder {
public:
    VorbisEncoder(unsigned channels, unsigned rate, float quality) {
        vorbis_info_init(&vi_);
        if (vorbis_encode_init_vbr(&vi_, long(channels), long(rate), quality)) {
            vorbis_info_clear(&vi_);
            fail("libvorbis can't encode " + std::to_string(channels) + " channels at " + std::to_string(rate) + " Hz");
        }
        vorbis_comment_init(&vc_);
        vorbis_analysis_init(&vd_, &vi_);
        vorbis_block_init(&vd_, &vb_);
        ogg_packet id, comment, setup;
        vorbis_analysis_headerout(&vd_, &vc_, &id, &comment, &setup);
        setup_.assign(reinterpret_cast<const char*>(setup.packet), size_t(setup.bytes));
    }
    ~VorbisEncoder() {
        vorbis_block_clear(&vb_);
        vorbis_dsp_clear(&vd_);
        vorbis_comment_clear(&vc_);
        vorbis_info_clear(&vi_);
    }
    VorbisEncoder(const VorbisEncoder&) = delete;
    VorbisEncoder& operator=(const VorbisEncoder&) = delete;

    const std::string& setup() const { return setup_; }
    int blocksize(int which) { return vorbis_info_blocksize(&vi_, which); }

    std::vector<std::string> encode(const std::vector<std::int16_t>& pcm, unsigned channels) {
        std::vector<std::string> out;
        auto drain = [&] {
            ogg_packet op;
            while (vorbis_analysis_blockout(&vd_, &vb_) == 1) {
                vorbis_analysis(&vb_, nullptr);
                vorbis_bitrate_addblock(&vb_);
                while (vorbis_bitrate_flushpacket(&vd_, &op))
                    out.emplace_back(reinterpret_cast<const char*>(op.packet), size_t(op.bytes));
            }
        };
        const size_t frames = pcm.size() / channels;
        for (size_t at = 0; at < frames; at += 1024) {
            const int n = int(std::min<size_t>(1024, frames - at));
            float** buffer = vorbis_analysis_buffer(&vd_, n);
            for (int i = 0; i < n; ++i)
                for (unsigned c = 0; c < channels; ++c) buffer[c][i] = pcm[(at + size_t(i)) * channels + c] / 32768.0f;
            vorbis_analysis_wrote(&vd_, n);
            drain();
        }
        vorbis_analysis_wrote(&vd_, 0);
        drain();
        return out;
    }

private:
    vorbis_info vi_;
    vorbis_comment vc_;
    vorbis_dsp_state vd_;
    vorbis_block vb_;
    std::string setup_;
};

// Header layout: CLAUDE.md §10 "Wwise Vorbis WEM layout" (extra = the fmt chunk's 48 bytes after cbSize).
std::string encode_vorbis(const Riff& r, const WemInfo& info, const std::vector<std::int16_t>& pcm,
                          const fs::path& codebooks) {
    const VorbisWem like = vorbis_parts(r);
    const Setup setup = expand_setup(like.payload, info.channels, Codebooks(codebooks));
    // libvorbis's setup changes with each whole step of quality: find the step that makes like's (the game's music is
    // 7.x, its cutscene effects 8.x).
    std::unique_ptr<VorbisEncoder> encoder;
    for (int step = -1; step <= 9 && !encoder; ++step) {
        auto e = std::make_unique<VorbisEncoder>(info.channels, info.rate, (float(step) + 0.5f) / 10);
        if (e->setup() == setup.packet) encoder = std::move(e);
    }
    if (!encoder) fail("libvorbis makes no setup like this Wwise Vorbis sound's (made with a setting the tool doesn't know)");
    const int sizes[2] = {1 << std::uint8_t(like.extra[46]), 1 << std::uint8_t(like.extra[47])};
    if (encoder->blocksize(0) != sizes[0] || encoder->blocksize(1) != sizes[1])
        fail("libvorbis's block sizes aren't the sound's");

    const std::uint64_t frames = pcm.size() / info.channels;
    std::vector<std::string> packets;
    for (const std::string& p : encoder->encode(pcm, info.channels)) packets.push_back(to_wwise_packet(p, setup));
    // Each packet's end in decoded samples (it gives the previous block's quarter and its own), and its offset from
    // the setup block's start.
    const size_t setup_block = 2 + like.payload.size();
    std::vector<std::uint64_t> ends;
    std::vector<std::uint64_t> offsets;
    std::uint64_t end = 0, offset = setup_block;
    int previous = 0;
    for (const std::string& p : packets) {
        const int size = sizes[setup.long_mode[mode_of(p, setup)]];
        if (previous) end += std::uint64_t(previous / 4 + size / 4);
        previous = size;
        ends.push_back(end);
        offsets.push_back(offset);
        offset += 2 + p.size();
    }
    if (packets.empty() || ends.back() < frames || ends.back() - frames > 0xFFFF)
        fail("libvorbis's packets don't cover the sound");
    const auto padding = std::uint16_t(ends.back() - frames);

    // Seek table: the first packet ending at least `granularity` samples after the previous entry, as steps. The
    // granularity is a Wwise setting: like's smallest step after the first (the game's: 2048).
    std::uint32_t granularity = 0;
    const auto table = get<std::uint32_t>(like.extra, 22);
    for (size_t at = 4; at + 4 <= table; at += 4) {
        const auto a = get<std::uint16_t>(r.data, at);
        granularity = granularity ? std::min<std::uint32_t>(granularity, a) : a;
    }
    if (!granularity) granularity = 2048;
    std::string seek;
    for (size_t i = 0, last_end = 0, last_offset = 0; i < ends.size(); ++i) {
        if (ends[i] < last_end + granularity) continue;
        if (ends[i] - last_end > 0xFFFF || offsets[i] - last_offset > 0xFFFF) fail("a seek step too large for Wwise");
        put(seek, std::uint16_t(ends[i] - last_end));
        put(seek, std::uint16_t(offsets[i] - last_offset));
        last_end = ends[i], last_offset = offsets[i];
    }

    std::string audio;
    size_t largest = 0;
    for (const std::string& p : packets) {
        if (p.size() > 0xFFFF) fail("a Vorbis packet too large for Wwise");
        put(audio, std::uint16_t(p.size()));
        audio += p;
        largest = std::max(largest, p.size());
    }
    std::string data = seek;
    put(data, std::uint16_t(like.payload.size()));
    data += like.payload;  // the same setup, so its hash and decoder sizes (copied below) stay right
    data += audio;
    std::string extra(like.extra.substr(0, 48));  // keeps @0, the channel config, the decoder sizes, hash, block sizes
    put_at(extra, 6, std::uint32_t(frames));
    put_at(extra, 10, std::uint32_t(setup_block));                 // loop start: the first audio packet
    put_at(extra, 14, std::uint32_t(setup_block + audio.size()));  // loop end: the audio's end
    put_at(extra, 18, std::uint16_t(0));
    put_at(extra, 20, padding);
    put_at(extra, 22, std::uint32_t(seek.size()));
    put_at(extra, 26, std::uint32_t(seek.size() + setup_block));
    put_at(extra, 30, std::uint16_t(largest));
    put_at(extra, 32, padding);
    std::string fmt;
    put(fmt, kWemVorbis);
    put(fmt, std::uint16_t(info.channels));
    put(fmt, std::uint32_t(info.rate));
    put(fmt, std::uint32_t((data.size() - seek.size()) * info.rate / frames));
    put(fmt, std::uint32_t(0));  // block align, bits per sample
    put(fmt, std::uint16_t(48));
    fmt += extra;
    return riff({{"fmt ", &fmt}, {"data", &data}});
}

std::vector<std::int16_t> decode_vorbis(const Riff& r, const WemInfo& info, const fs::path& codebooks) {
    const VorbisWem v = vorbis_parts(r);
    const Setup setup = expand_setup(v.payload, info.channels, Codebooks(codebooks));
    BitOut id;
    id.put(1, 8);
    for (const char c : Bytes("vorbis")) id.put(std::uint8_t(c), 8);
    id.put(0, 32), id.put(info.channels, 8), id.put(info.rate, 32), id.put(0, 32), id.put(0, 32), id.put(0, 32);
    id.put(std::uint8_t(v.extra[46]), 4), id.put(std::uint8_t(v.extra[47]), 4), id.put(1, 1);
    std::string comment("\3vorbis", 7);
    put(comment, std::uint32_t(0)), put(comment, std::uint32_t(0));
    comment += '\1';

    vorbis_info vi;
    vorbis_comment vc;
    vorbis_info_init(&vi);
    vorbis_comment_init(&vc);
    struct Clear {
        vorbis_info* vi;
        vorbis_comment* vc;
        ~Clear() { vorbis_comment_clear(vc), vorbis_info_clear(vi); }
    } clear{&vi, &vc};
    long number = 0;
    auto packet = [&](const std::string& bytes) {
        ogg_packet op{};
        op.packet = reinterpret_cast<unsigned char*>(const_cast<char*>(bytes.data()));
        op.bytes = long(bytes.size());
        op.b_o_s = number == 0;
        op.packetno = number++;
        return op;
    };
    for (const std::string* h : std::initializer_list<const std::string*>{&id.b, &comment, &setup.packet}) {
        ogg_packet op = packet(*h);
        if (vorbis_synthesis_headerin(&vi, &vc, &op) < 0) fail("libvorbis refuses this WEM's headers");
    }
    vorbis_dsp_state vd;
    if (vorbis_synthesis_init(&vd, &vi)) fail("libvorbis can't decode this WEM");
    vorbis_block vb;
    vorbis_block_init(&vd, &vb);
    std::vector<std::int16_t> out;
    std::vector<bool> long_block;
    for (const Bytes p : v.packets) long_block.push_back(!p.empty() && setup.long_mode[mode_of(p, setup)]);
    for (size_t i = 0; i < v.packets.size(); ++i) {
        if (v.packets[i].empty()) continue;
        const std::string standard = from_wwise_packet(v.packets[i], setup, i > 0 && long_block[i - 1],
                                                       i + 1 < v.packets.size() && long_block[i + 1]);
        ogg_packet op = packet(standard);
        if (vorbis_synthesis(&vb, &op) == 0) vorbis_synthesis_blockin(&vd, &vb);
        float** pcm = nullptr;
        for (int n; (n = vorbis_synthesis_pcmout(&vd, &pcm)) > 0; vorbis_synthesis_read(&vd, n))
            for (int s = 0; s < n; ++s)
                for (unsigned c = 0; c < info.channels; ++c)
                    out.push_back(std::int16_t(std::clamp(std::lround(pcm[c][s] * 32768.0f), -32768L, 32767L)));
    }
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    out.resize(std::min(out.size(), size_t(info.samples) * info.channels));  // the end padding off
    return out;
}

// ---- Wwise Opus ----

// libopus's mapping family 1 holds channels in Vorbis's order; a WAV's (and Wwise's) differs from 3 channels up.
// Vorbis position i holds WAV channel kVorbisOrder[channels][i].
constexpr int kVorbisOrder[9][8] = {{},
                                    {0},
                                    {0, 1},
                                    {0, 2, 1},
                                    {0, 1, 2, 3},
                                    {0, 2, 1, 3, 4},
                                    {0, 2, 1, 4, 5, 3},
                                    {0, 2, 1, 5, 6, 4, 3},
                                    {0, 2, 1, 6, 7, 4, 5, 3}};

using OpusEncoderPtr = std::unique_ptr<OpusMSEncoder, decltype(&opus_multistream_encoder_destroy)>;

struct OpusLayout {
    int family = 0, streams = 0, coupled = 0;
    unsigned char mapping[255] = {};
    // Which of a WAV's channels goes in libopus's position c: family 1 is in Vorbis's order; 0 and 255 (every
    // channel a stream of its own) as they are. [inferred for 255: no Wwise reference file has it]
    unsigned wav_channel(unsigned channels, unsigned c) const {
        return family == 1 ? unsigned(kVorbisOrder[channels][c]) : c;
    }
};

// libopus's encoder for `channels` in `family` (0: mono or stereo, 1: up to 8 in Vorbis's order, 255: any number,
// each its own stream), as Wwise's files use them; `layout` gets its streams.
OpusEncoderPtr opus_encoder(unsigned channels, int family, OpusLayout& layout) {
    if (channels == 0 || (family == 0 && channels > 2) || (family == 1 && channels > 8) ||
        (family != 0 && family != 1 && family != 255))
        fail("an Opus layout the tool can't write yet (" + std::to_string(channels) + " channels, mapping family " +
             std::to_string(family) + ")");
    int error = 0;
    layout.family = family;
    OpusEncoderPtr e(opus_multistream_surround_encoder_create(48000, int(channels), family, &layout.streams,
                                                              &layout.coupled, layout.mapping,
                                                              OPUS_APPLICATION_AUDIO, &error),
                     &opus_multistream_encoder_destroy);
    if (!e || error != OPUS_OK) fail(std::string("libopus: ") + opus_strerror(error));
    return e;
}

// Header layout: CLAUDE.md §10 "Wwise Opus WEM layout".
std::string encode_opus(const Riff& r, const WemInfo& info, const std::vector<std::int16_t>& pcm) {
    const Bytes extra = r.fmt.substr(18);
    if (extra.size() < 18 || info.rate != 48000) fail("a Wwise Opus header the tool doesn't know");
    const unsigned channels = info.channels;
    OpusLayout layout;
    const OpusEncoderPtr encoder = opus_encoder(channels, std::uint8_t(extra[17]), layout);
    // The original's average bit rate, within libopus's sensible range (a silent original's is tiny).
    const auto bitrate = std::clamp<std::uint64_t>(std::uint64_t(get<std::uint32_t>(r.fmt, 8)) * 8, 32'000 * channels,
                                                   128'000 * channels);
    opus_multistream_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(opus_int32(bitrate)));
    opus_int32 skip = 0;
    opus_multistream_encoder_ctl(encoder.get(), OPUS_GET_LOOKAHEAD(&skip));
    const std::uint64_t frames = pcm.size() / channels;
    const std::uint64_t count = (frames + std::uint64_t(skip) + 959) / 960;  // 20 ms each, the start delay included
    std::vector<std::int16_t> frame(960 * channels);
    std::vector<unsigned char> buffer(1500 * size_t(layout.streams) + 16);
    std::string seek, data;
    for (std::uint64_t k = 0; k < count; ++k) {
        for (std::uint64_t i = 0; i < 960; ++i)
            for (unsigned c = 0; c < channels; ++c) {
                const std::uint64_t n = k * 960 + i;
                frame[i * channels + c] = n < frames ? pcm[n * channels + layout.wav_channel(channels, c)] : 0;
            }
        const opus_int32 bytes =
            opus_multistream_encode(encoder.get(), frame.data(), 960, buffer.data(), opus_int32(buffer.size()));
        if (bytes < 0) fail(std::string("libopus: ") + opus_strerror(bytes));
        put(seek, std::uint16_t(bytes));
        data.append(reinterpret_cast<const char*>(buffer.data()), size_t(bytes));
    }
    std::string fmt;
    put(fmt, kWemOpus);
    put(fmt, std::uint16_t(channels));
    put(fmt, std::uint32_t(48000));
    put(fmt, std::uint32_t(data.size() * 48000 / frames));
    put(fmt, std::uint32_t(0));    // block align, bits per sample
    put(fmt, std::uint16_t(16));  // cbSize says 16; 18 follow (as Wwise writes it)
    put(fmt, std::uint16_t(960));
    fmt += extra.substr(2, 4);  // the channel config
    put(fmt, std::uint32_t(frames));
    put(fmt, std::uint32_t(count));
    put(fmt, std::uint16_t(skip));
    fmt += char(1);  // OpusHead version
    fmt += char(layout.family);
    return riff({{"fmt ", &fmt}, {"seek", &seek}, {"data", &data}});
}

std::vector<std::int16_t> decode_opus(const Riff& r, const WemInfo& info) {
    const Bytes extra = r.fmt.substr(18);
    if (extra.size() < 18) fail("a Wwise Opus header the tool doesn't know");
    const unsigned channels = info.channels;
    OpusLayout layout;
    opus_encoder(channels, std::uint8_t(extra[17]), layout);  // the streams the encoder makes for this layout
    int error = 0;
    std::unique_ptr<OpusMSDecoder, decltype(&opus_multistream_decoder_destroy)> decoder(
        opus_multistream_decoder_create(48000, int(channels), layout.streams, layout.coupled, layout.mapping, &error),
        &opus_multistream_decoder_destroy);
    if (!decoder || error != OPUS_OK) fail(std::string("libopus: ") + opus_strerror(error));
    std::vector<std::int16_t> decoded, frame(5760 * channels);
    for (size_t i = 0, at = 0; i + 2 <= r.seek.size(); i += 2) {
        const size_t size = get<std::uint16_t>(r.seek, i);
        if (size > r.data.size() - at) fail("an Opus packet runs past the WEM's end");
        const int n = opus_multistream_decode(decoder.get(), reinterpret_cast<const unsigned char*>(r.data.data() + at),
                                              opus_int32(size), frame.data(), 5760, 0);
        if (n < 0) fail(std::string("libopus: ") + opus_strerror(n));
        decoded.insert(decoded.end(), frame.begin(), frame.begin() + n * int(channels));
        at += size;
    }
    const size_t skip = get<std::uint16_t>(extra, 14);
    std::vector<std::int16_t> out(size_t(info.samples) * channels);
    for (size_t n = 0; n < info.samples && (skip + n + 1) * channels <= decoded.size(); ++n)
        for (unsigned c = 0; c < channels; ++c)
            out[n * channels + layout.wav_channel(channels, c)] = decoded[(skip + n) * channels + c];
    return out;
}

}  // namespace

// ---- Packages ----

Akpk read_akpk(std::string_view b) {
    if (b.substr(0, 4) != "AKPK") fail("not a sound package (AKPK)");
    const size_t header = get<std::uint32_t>(b, 4), data_start = 8 + header;
    Akpk p;
    p.version = get<std::uint32_t>(b, 8);
    const std::uint32_t sizes[4] = {get<std::uint32_t>(b, 12), get<std::uint32_t>(b, 16), get<std::uint32_t>(b, 20),
                                    get<std::uint32_t>(b, 24)};
    size_t at = 28;
    if (sizes[0] > b.size() - std::min(at, b.size())) fail("a sound package's language map runs past its end");
    p.languages = std::string(b.substr(at, sizes[0]));
    at += sizes[0];
    const bool header_only = b.size() <= data_start;
    std::vector<AkpkFile>* tables[] = {&p.banks, &p.streams, &p.externals};
    for (int t = 0; t < 3; ++t) {
        const size_t end = at + sizes[t + 1];
        const std::uint32_t count = get<std::uint32_t>(b, at);
        size_t q = at + 4;
        for (std::uint32_t i = 0; i < count; ++i) {
            AkpkFile f;
            f.id = t == 2 ? get<std::uint64_t>(b, q) : get<std::uint32_t>(b, q);
            q += t == 2 ? 8 : 4;
            f.block = std::max<std::uint32_t>(1, get<std::uint32_t>(b, q));
            const std::uint64_t size = get<std::uint32_t>(b, q + 4), offset = std::uint64_t(get<std::uint32_t>(b, q + 8)) * f.block;
            f.language = get<std::uint32_t>(b, q + 12);
            q += 16;
            if (!header_only) {
                if (offset < data_start || offset + size > b.size()) fail("a sound package's file runs past its end");
                f.data = std::string(b.substr(size_t(offset), size_t(size)));
            }
            tables[t]->push_back(std::move(f));
        }
        if (q != end) fail("a sound package's table isn't the size its header says");
        at = end;
    }
    if (at != data_start) fail("a sound package's header isn't the size it says");
    return p;
}

std::string write_akpk(const Akpk& p, bool with_data) {
    const std::vector<AkpkFile>* tables[] = {&p.banks, &p.streams, &p.externals};
    std::uint32_t sizes[3];
    for (int t = 0; t < 3; ++t) sizes[t] = std::uint32_t(4 + tables[t]->size() * (t == 2 ? 24 : 20));
    const std::uint32_t header = std::uint32_t(20 + p.languages.size()) + sizes[0] + sizes[1] + sizes[2];
    std::string out = "AKPK", data;
    put(out, header);
    put(out, p.version);
    put(out, std::uint32_t(p.languages.size()));
    for (const std::uint32_t s : sizes) put(out, s);
    out += p.languages;
    std::uint64_t at = 8 + std::uint64_t(header);
    for (int t = 0; t < 3; ++t) {
        put(out, std::uint32_t(tables[t]->size()));
        for (const AkpkFile& f : *tables[t]) {
            const std::uint32_t block = std::max<std::uint32_t>(1, f.block);
            const std::uint64_t start = (at + block - 1) / block;
            data.resize(size_t(start * block - (8 + std::uint64_t(header))), '\0');  // alignment
            if (start > 0xFFFFFFFF || f.data.size() > 0xFFFFFFFF) fail("a sound package over 4 GB");
            t == 2 ? put(out, f.id) : put(out, std::uint32_t(f.id));
            put(out, block);
            put(out, std::uint32_t(f.data.size()));
            put(out, std::uint32_t(start));
            put(out, f.language);
            data += f.data;
            at = start * block + f.data.size();
        }
    }
    return with_data ? out + data : out;
}

// ---- Banks ----

Bank read_bank(std::string_view b) {
    if (b.substr(0, 4) != "BKHD") fail("not a sound bank (BKHD)");
    Bank bank;
    Bytes index, data;
    for (size_t at = 0; at + 8 <= b.size();) {
        const std::string tag(b.substr(at, 4));
        const size_t size = get<std::uint32_t>(b, at + 4);
        if (size > b.size() - at - 8) fail("a sound bank's chunk runs past its end");
        const Bytes body = b.substr(at + 8, size);
        if (tag == "DIDX") index = body;
        if (tag == "DATA") data = body;
        bank.chunks.emplace_back(tag, tag == "DIDX" || tag == "DATA" ? std::string() : std::string(body));
        at += 8 + size;
    }
    for (size_t i = 0; i + 12 <= index.size(); i += 12) {
        const auto id = get<std::uint32_t>(index, i), offset = get<std::uint32_t>(index, i + 4),
                   size = get<std::uint32_t>(index, i + 8);
        if (offset > data.size() || size > data.size() - offset) fail("a sound bank's media runs past its end");
        bank.media.push_back({id, std::string(data.substr(offset, size))});
    }
    return bank;
}

std::string write_bank(const Bank& bank) {
    std::string index, data;
    for (const BankMedia& m : bank.media) {
        data.resize((data.size() + 15) / 16 * 16, '\0');
        put(index, m.id);
        put(index, std::uint32_t(data.size()));
        put(index, std::uint32_t(m.data.size()));
        data += m.data;
    }
    std::string out;
    for (const auto& [tag, body] : bank.chunks) {
        const std::string& bytes = tag == "DIDX" ? index : tag == "DATA" ? data : body;
        out += tag;
        put(out, std::uint32_t(bytes.size()));
        out += bytes;
    }
    return out;
}

// HIRC (bank version 140): u32 count, then objects {u8 type, u32 size, body}. A Sound (type 2) is u32 id then its
// source; a music track (11) u32 id, u8 flags, u32 count, then its sources. A source (AkBankSourceData): u32 plugin,
// u8 stream type, u32 media id, u32 in-memory size, u8 bits. [Checked on every RE4R bank: the sizes match the media.]
std::vector<BankSource> bank_sources(const Bank& bank) {
    std::vector<BankSource> out;
    for (const auto& [tag, body] : bank.chunks) {
        if (tag != "HIRC") continue;
        const Bytes h = body;
        const auto count = get<std::uint32_t>(h, 0);
        auto source = [&](size_t s, bool music) {
            out.push_back({get<std::uint32_t>(h, s + 5), std::uint8_t(h[s + 4]), get<std::uint32_t>(h, s + 9), s + 9, music});
        };
        for (std::uint32_t i = 0, at = 4; i < count && at + 5 <= h.size(); ++i) {
            const std::uint8_t type = std::uint8_t(h[at]);
            const std::uint32_t size = get<std::uint32_t>(h, at + 1), o = at + 5;
            if (size > h.size() - o) fail("a sound bank's event data runs past its end");
            if (type == 2 && size >= 18) source(o + 4, false);
            if (type == 11 && size >= 9)
                if (const auto n = get<std::uint32_t>(h, o + 5); n <= (size - 9) / 14)
                    for (std::uint32_t k = 0; k < n; ++k) source(o + 9 + k * 14, true);
            at = o + size;
        }
    }
    return out;
}

void set_source_memory(Bank& bank, const BankSource& source, std::uint32_t memory) {
    for (auto& [tag, body] : bank.chunks)
        if (tag == "HIRC") put_at(body, source.at, memory);
}

namespace {

// Where a WEM's header ends, then where each packet ends (file offsets).
std::vector<size_t> packet_ends(Bytes wem) {
    const Riff r = chunks(wem);
    const size_t data = size_t(r.data.data() - wem.data());
    std::vector<size_t> ends;
    if (get<std::uint16_t>(r.fmt, 0) == kWemOpus) {
        ends.push_back(data);
        for (size_t i = 0; i + 2 <= r.seek.size(); i += 2) ends.push_back(ends.back() + get<std::uint16_t>(r.seek, i));
    } else if (get<std::uint16_t>(r.fmt, 0) == kWemVorbis) {
        const VorbisWem v = vorbis_parts(r);
        ends.push_back(data + get<std::uint32_t>(v.extra, 26));
        for (const Bytes p : v.packets) ends.push_back(ends.back() + 2 + p.size());
    } else {
        fail("a WEM codec the tool can't read");
    }
    return ends;
}

}  // namespace

size_t wem_prefix(std::string_view wem, size_t packets) {
    const auto ends = packet_ends(wem);
    return std::min(ends[std::min(packets, ends.size() - 1)], wem.size());
}

size_t wem_packets_within(std::string_view wem, size_t bytes) {
    const auto ends = packet_ends(wem);
    size_t k = 0;
    while (k + 1 < ends.size() && ends[k + 1] <= bytes) ++k;
    return k;
}

// ---- WEMs ----

WemInfo read_wem_info(std::string_view wem) {
    const Riff r = chunks(wem);
    WemInfo info{get<std::uint16_t>(r.fmt, 0), get<std::uint16_t>(r.fmt, 2), get<std::uint32_t>(r.fmt, 4), 0};
    if (!info.channels) fail("a WEM with no channels");
    if (info.codec == kWemVorbis || info.codec == kWemOpus)
        info.samples = get<std::uint32_t>(r.fmt, 24);  // both keep it 6 bytes into the extra
    else
        info.samples = std::uint32_t(r.data.size() / (2 * info.channels));  // ponytail: 16-bit PCM assumed
    return info;
}

std::string encode_wem(std::string_view like, const std::vector<std::int16_t>& pcm, const fs::path& codebooks) {
    const Riff r = chunks(like);
    const WemInfo info = read_wem_info(like);
    if (pcm.empty() || pcm.size() % info.channels) fail("no sound, or not " + std::to_string(info.channels) + " channels");
    switch (info.codec) {
    case kWemVorbis:
        if (codebooks.empty()) fail("Wwise's codebook library (packed_codebooks_aoTuV_603.bin) wasn't found");
        return encode_vorbis(r, info, pcm, codebooks);
    case kWemOpus:
        return encode_opus(r, info, pcm);
    default: {
        char hex[8];
        std::snprintf(hex, sizeof hex, "0x%04X", info.codec);
        fail(std::string("a WEM codec the tool can't write (") + hex + ")");
    }
    }
}

std::vector<std::int16_t> decode_wem(std::string_view wem, const fs::path& codebooks) {
    const Riff r = chunks(wem);
    const WemInfo info = read_wem_info(wem);
    if (info.codec == kWemVorbis) {
        if (codebooks.empty()) fail("Wwise's codebook library (packed_codebooks_aoTuV_603.bin) wasn't found");
        return decode_vorbis(r, info, codebooks);
    }
    if (info.codec == kWemOpus) return decode_opus(r, info);
    fail("a WEM codec the tool can't read");
}

fs::path find_codebooks() {
    constexpr const char* name = "packed_codebooks_aoTuV_603.bin";
    const fs::path profiles = find_profiles_dir();
    if (profiles.empty()) return {};
    std::error_code ec;
    for (const fs::path& f : {profiles.parent_path() / "data" / name, profiles.parent_path() / "third_party" / "ww2ogg" / name})
        if (fs::is_regular_file(f, ec)) return f;
    return {};
}

}  // namespace remod
