#include "graph.hpp"
#include "nodes.hpp"
#include "movie.hpp"
#include "profile.hpp"
#include "sound.hpp"
#include "wwise.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using test::TempDir;
namespace fs = std::filesystem;

namespace {

template <class T>
void put(std::string& b, T v) {
    b.append(reinterpret_cast<const char*>(&v), sizeof v);
}

std::string chunk(const char* tag, const std::string& body) {
    std::string out = tag;
    put(out, std::uint32_t(body.size()));
    return out + body;
}

// A Wwise Opus WEM's header as the game's (CLAUDE.md §10), with no sound: what encode_wem takes as `like`.
std::string opus_like(std::uint16_t channels, std::uint32_t samples, std::uint32_t channel_config, std::uint8_t family) {
    std::string fmt;
    put(fmt, std::uint16_t(0x3041)), put(fmt, channels), put(fmt, std::uint32_t(48000)), put(fmt, std::uint32_t(8000));
    put(fmt, std::uint32_t(0)), put(fmt, std::uint16_t(16));
    put(fmt, std::uint16_t(960)), put(fmt, channel_config), put(fmt, samples), put(fmt, std::uint32_t(0));
    put(fmt, std::uint16_t(312)), fmt += char(1), fmt += char(family);
    const std::string body = "WAVE" + chunk("fmt ", fmt) + chunk("seek", "") + chunk("data", std::string(1, '\0'));
    std::string out = "RIFF";
    put(out, std::uint32_t(body.size()));
    return out + body;
}

// `frames` of a tone per channel (freqs[c] Hz), half volume.
std::vector<std::int16_t> tones(std::uint64_t frames, const std::vector<double>& freqs) {
    std::vector<std::int16_t> out(size_t(frames) * freqs.size());
    for (size_t n = 0; n < frames; ++n)
        for (size_t c = 0; c < freqs.size(); ++c)
            out[n * freqs.size() + c] = std::int16_t(16000 * std::sin(6.283185307 * freqs[c] * double(n) / 48000));
    return out;
}

// Signal to error of channel `c` of `got` against channel `want_c` of `want` (dB), both `channels` wide.
double snr(const std::vector<std::int16_t>& want, size_t want_c, const std::vector<std::int16_t>& got, size_t c,
           size_t channels) {
    double signal = 0, error = 0;
    for (size_t n = 0; n * channels < std::min(want.size(), got.size()); ++n) {
        const double a = want[n * channels + want_c], b = got[n * channels + c];
        signal += a * a, error += (a - b) * (a - b);
    }
    return 10 * std::log10((signal + 1) / (error + 1));
}

// A 16-bit WAV's samples (its data chunk).
std::vector<std::int16_t> wav_samples(const fs::path& file) {
    const std::string b = test::read_file(file);
    for (size_t at = 12; at + 8 <= b.size();) {
        std::uint32_t size;
        std::memcpy(&size, b.data() + at + 4, 4);
        if (b.compare(at, 4, "data") == 0) {
            std::vector<std::int16_t> out(size / 2);
            std::memcpy(out.data(), b.data() + at + 8, out.size() * 2);
            return out;
        }
        at += 8 + size + (size & 1);
    }
    return {};
}

std::string game_dir() {
    char* v = nullptr;
    size_t n = 0;
    _dupenv_s(&v, &n, "REMOD_GAME");
    const std::string game = v ? v : "";
    std::free(v);
    return game;
}

}  // namespace

TEST_CASE("sound packages: written and read back, header-only copies, ids of both widths") {
    remod::Akpk p;
    p.languages = std::string("\1\0\0\0\x0c\0\0\0\0\0\0\0s\0f\0x\0\0\0", 20);
    p.streams = {{.id = 0x7c1d96, .language = 0, .data = "first"}, {.id = 42, .language = 1, .data = "second!"}};
    p.externals = {{.id = 0x1122334455667788ull, .data = "ext"}};
    const std::string full = remod::write_akpk(p, true), header = remod::write_akpk(p, false);
    CHECK(full.substr(0, header.size()) == header);
    CHECK(full.size() == header.size() + 5 + 7 + 3);
    const remod::Akpk back = remod::read_akpk(full);
    CHECK(back.languages == p.languages);
    REQUIRE(back.streams.size() == 2);
    CHECK(back.streams[1].id == 42);
    CHECK(back.streams[1].language == 1);
    CHECK(back.streams[1].data == "second!");
    CHECK(back.externals.at(0).id == 0x1122334455667788ull);
    CHECK(remod::write_akpk(back, true) == full);
    const remod::Akpk head = remod::read_akpk(header);  // the copy outside streaming/
    CHECK(head.streams.at(0).data.empty());
    CHECK(head.streams.at(0).id == 0x7c1d96);
    CHECK_THROWS_WITH(remod::read_akpk("RIFF...."), ContainsSubstring("not a sound package"));
    CHECK_THROWS(remod::read_akpk(full.substr(0, 30)));
}

TEST_CASE("Opus WEMs: libopus at the original's channels and length, back again; 3 and 6 channels in a WAV's order, 12 each its own") {
    for (const auto& [channels, config, family] : {std::tuple{1, 0x4101u, 0}, {2, 0x3102u, 0}, {3, 0x7103u, 1},
                                                   {6, 0x3F106u, 1}, {12, 0u, 255}, {1, 0x4101u, 255}}) {
        CAPTURE(channels);
        std::vector<double> freqs{440, 660, 880, 1100, 1320, 1540};
        freqs.resize(size_t(channels));
        if (channels == 12)  // each a mono stream: kept low, where Opus keeps a tone's waveform at this bit rate
            for (int c = 0; c < 12; ++c) freqs[size_t(c)] = 400 + 60 * c;
        const auto pcm = tones(30000, freqs);
        const std::string wem = remod::encode_wem(opus_like(std::uint16_t(channels), 1, config, std::uint8_t(family)),
                                                  pcm, {});
        const remod::WemInfo info = remod::read_wem_info(wem);
        CHECK(info.codec == remod::kWemOpus);
        CHECK(info.channels == unsigned(channels));
        CHECK(info.samples == 30000);
        const auto back = remod::decode_wem(wem, {});
        REQUIRE(back.size() == pcm.size());
        for (int c = 0; c < channels; ++c)  // each tone where it was (6: not the LFE, which Opus low-passes)
            if (channels != 6 || c != 3) {
                CAPTURE(c);
                CHECK(snr(pcm, c, back, c, channels) > 10);
            }
        CHECK(std::uint8_t(wem[20 + 18 + 17]) == family);  // the mapping family, as the original's
    }
    CHECK_THROWS_WITH(remod::encode_wem(opus_like(4, 1, 0, 2), std::vector<std::int16_t>(4), {}),
                      ContainsSubstring("can't write yet"));  // family 2 (ambisonics): not in RE4R
}

TEST_CASE("Wwise's own WEMs decode to their WAVs, channel order too (the local spike files; skipped without them)") {
    const fs::path spike = remod::find_profiles_dir().parent_path() / "spikes/wwise";
    if (!fs::exists(spike / "wem/Windows/vorbis/tone_3ch.wem")) SKIP("make spikes/wwise's reference files to run");
    const fs::path codebooks = remod::find_codebooks();
    REQUIRE(!codebooks.empty());
    for (const char* codec : {"vorbis", "opus"})
        for (const auto& [name, channels] : {std::pair{"tone_3ch", 3}, {"tone_stereo", 2}, {"tone_mono", 1}}) {
            CAPTURE(codec, name);
            const auto want = wav_samples(spike / "in" / (std::string(name) + ".wav"));
            const auto got =
                remod::decode_wem(test::read_file(spike / "wem/Windows" / codec / (std::string(name) + ".wem")), codebooks);
            CHECK(got.size() == want.size());
            for (int c = 0; c < channels; ++c) CHECK(snr(want, c, got, c, channels) > 20);  // 440 / 660 / 880 Hz: L R C
        }
}

TEST_CASE("the game's sound packages and WEMs (set REMOD_GAME to the extracted natives/STM)") {
    const std::string game = game_dir();
    if (game.empty()) SKIP("set REMOD_GAME to run");
    const fs::path wwise = fs::path(game) / "_chainsaw/sound/wwise", streaming = fs::path(game) / "streaming/_chainsaw/sound/wwise";
    if (!fs::exists(streaming / "ch_mva000_bgm.spck.1.x64")) SKIP("needs streaming/_chainsaw/sound/wwise");
    // Read and written again: the same bytes, the header-only copy from the streaming one's table.
    for (const char* name : {"ch_mva000_bgm.spck.1.x64", "ch_mva000_dialogue.spck.1.x64.en", "ch_mva202_se.spck.1.x64",
                             "ch_bgm_castle.spck.1.x64"}) {
        CAPTURE(name);
        const std::string data = test::read_file(streaming / name);
        const remod::Akpk p = remod::read_akpk(data);
        CHECK(remod::write_akpk(p, true) == data);
        CHECK(remod::write_akpk(p, false) == test::read_file(wwise / name));
    }
    const fs::path codebooks = remod::find_codebooks();
    REQUIRE(!codebooks.empty());
    // The intro's music (Wwise Vorbis, libvorbis quality 7's setup) and English dialogue (Opus, 3 channels).
    const auto music = remod::read_akpk(test::read_file(streaming / "ch_mva000_bgm.spck.1.x64")).streams.at(0).data;
    const auto pcm = tones(96000, {440, 660});
    const std::string wem = remod::encode_wem(music, pcm, codebooks);
    const remod::WemInfo info = remod::read_wem_info(wem);
    CHECK(info.codec == remod::kWemVorbis);
    CHECK(info.samples == 96000);
    const auto back = remod::decode_wem(wem, codebooks);
    REQUIRE(back.size() == pcm.size());
    CHECK(snr(pcm, 0, back, 0, 2) > 15);
    CHECK(snr(pcm, 1, back, 1, 2) > 15);
    CHECK(remod::decode_wem(music, codebooks).size() == size_t(remod::read_wem_info(music).samples) * 2);
    const auto dialogue =
        remod::read_akpk(test::read_file(streaming / "ch_mva000_dialogue.spck.1.x64.en")).streams.at(0).data;
    const auto voice = tones(48000, {440, 660, 880});
    const auto spoken = remod::decode_wem(remod::encode_wem(dialogue, voice, codebooks), codebooks);
    for (int c = 0; c < 3; ++c) CHECK(snr(voice, c, spoken, c, 3) > 10);
}

TEST_CASE("Replace movie: new sound packages for a movie whose sound is in them, the effects beeping, the rest silent") {
    TempDir dir;
    const fs::path natives = dir.path / "natives/STM", folder = natives / "streaming/_chainsaw/movie/mv/mva201";
    fs::create_directories(folder);
    remod::encode_movie({}, {.width = 160, .height = 90, .seconds = 1, .fps = 30, .bitrate = 500'000}, "a",
                        folder / "mva201.mov.1.x64");
    // Its packages: effects (stereo) and English dialogue (mono), header-only copies outside streaming\.
    const auto package = [&](const std::string& name, const std::string& wem, std::uint32_t id) {
        remod::Akpk p;
        p.streams = {{.id = id, .data = wem}};
        test::write_file(natives / "streaming/_chainsaw/sound/wwise" / name, remod::write_akpk(p, true));
        test::write_file(natives / "_chainsaw/sound/wwise" / name, remod::write_akpk(p, false));
    };
    package("ch_mva201_se.spck.1.x64", opus_like(2, 48000, 0x3102, 0), 7);
    package("ch_mva201_dialogue.spck.1.x64.en", opus_like(1, 24000, 0x4101, 0), 8);
    package("ch_mva999_se.spck.1.x64", opus_like(2, 48000, 0x3102, 0), 9);  // another movie's

    remod::Graph g;
    g.add_node("ReplaceMovie").params["movie"] = (folder / "mva201.mov.1.x64").string();  // 1
    auto& pack = g.add_node("PackageMod").params;                                           // 2
    pack["name"] = "SoundTest";
    pack["out"] = "mods";
    pack["replace"] = "true";
    REQUIRE(g.connect({1, "movie", 2, "file"}) == "");
    REQUIRE(g.connect({1, "sound", 2, "file"}) == "");
    REQUIRE(g.validate().empty());
    struct NoTextures : remod::ITextureConverter {
        remod::TexMeta load_tex(const fs::path&, const fs::path&, const remod::Profile&) override { return {}; }
        remod::TexMeta save_tex(const fs::path&, const fs::path&, const fs::path&, const remod::Profile&) override {
            return {};
        }
    } conv;
    const remod::Profile profile = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    const remod::RunOptions opt{.profile = profile, .converter = conv, .base_dir = dir.path,
                                .cache_dir = dir.path / "cache"};
    const auto result = remod::run_graph(g, opt);
    CHECK_THAT(result.nodes.at(1).message, ContainsSubstring("2 sound packages and their banks (effects: a beep each second"));
    CHECK_THAT(result.nodes.at(2).message, ContainsSubstring("5 other file"));  // the movie, 2 packages x 2 copies
    const fs::path mod = dir.path / "mods/SoundTest/natives/STM";
    const auto se = remod::read_akpk(test::read_file(mod / "streaming/_chainsaw/sound/wwise/ch_mva201_se.spck.1.x64"));
    REQUIRE(se.streams.size() == 1);
    CHECK(se.streams[0].id == 7);
    CHECK(remod::write_akpk(se, false) == test::read_file(mod / "_chainsaw/sound/wwise/ch_mva201_se.spck.1.x64"));
    const auto beep = remod::decode_wem(se.streams[0].data, {});
    REQUIRE(beep.size() == 48000 * 2);  // the original's length
    auto loudest = [&](const std::vector<std::int16_t>& s, size_t from, size_t to) {
        int peak = 0;
        for (size_t i = from; i < to && i < s.size(); ++i) peak = std::max(peak, std::abs(int(s[i])));
        return peak;
    };
    CHECK(loudest(beep, 2 * 1000, 2 * 4000) > 4000);    // the beep, the first tenth of a second
    CHECK(loudest(beep, 2 * 10000, 2 * 40000) < 200);   // then quiet
    const auto voice = remod::read_akpk(test::read_file(mod / "streaming/_chainsaw/sound/wwise/ch_mva201_dialogue.spck.1.x64.en"));
    const auto silent = remod::decode_wem(voice.streams.at(0).data, {});
    CHECK(silent.size() == 24000);
    CHECK(loudest(silent, 0, silent.size()) < 200);
    CHECK_FALSE(fs::exists(mod / "streaming/_chainsaw/sound/wwise/ch_mva999_se.spck.1.x64"));
    CHECK_THAT(remod::run_graph(g, opt).nodes.at(1).message, ContainsSubstring("their banks unchanged"));

    // Off: no packages.
    g.find(1)->params["sound"] = "false";
    CHECK_THAT(remod::run_graph(g, opt).nodes.at(2).message, ContainsSubstring("1 other file"));
}

namespace {

// Event data (HIRC) with one Sound (type 2) or music track (11) per entry: {type, media id, stream type, size}.
std::string events(const std::vector<std::tuple<std::uint8_t, std::uint32_t, std::uint8_t, std::uint32_t>>& objects) {
    std::string out;
    put(out, std::uint32_t(objects.size()));
    std::uint32_t object = 1000;
    for (const auto& [type, media, stream, memory] : objects) {
        std::string body;
        put(body, object++);
        if (type == 11) body += char(0), put(body, std::uint32_t(1));  // flags, one source
        put(body, std::uint32_t(0x00040001)), body += char(stream), put(body, media), put(body, memory), body += char(0);
        body += std::string(10, '\x7f');  // the rest of the object, untouched
        out += char(type);
        put(out, std::uint32_t(body.size()));
        out += body;
    }
    return out;
}

remod::Bank bank_of(std::vector<remod::BankMedia> media, const std::string& hirc) {
    remod::Bank b;
    b.chunks = {{"BKHD", std::string(12, '\x8c')}, {"DIDX", ""}, {"DATA", ""}, {"HIRC", hirc}};
    b.media = std::move(media);
    return b;
}

// A small game sound folder: bank a (sounds 7100, and 7300 in a music track, whole), package p (streamed sound 7200,
// stereo) with its first part in bank b (English), and bank c (an unrelated sound).
struct SoundTree {
    TempDir dir;
    fs::path natives = dir.path / "natives/STM";
    std::string o100 = remod::encode_wem(opus_like(1, 1, 0x4101, 0), tones(24000, {440}), {});
    std::string o200 = remod::encode_wem(opus_like(2, 1, 0x3102, 0), tones(48000, {440, 660}), {});
    std::string o300 = remod::encode_wem(opus_like(1, 1, 0x4101, 0), tones(24000, {880}), {});
    SoundTree() {
        const fs::path wwise = natives / "_chainsaw/sound/wwise";
        test::write_file(wwise / "a.sbnk.1.x64",
                         remod::write_bank(bank_of({{7100, o100}, {7300, o300}},
                                                   events({{2, 7100, 0, std::uint32_t(o100.size())},
                                                           {11, 7300, 0, std::uint32_t(o300.size())}}))));
        const size_t first = remod::wem_prefix(o200, 3);
        test::write_file(wwise / "b.sbnk.1.x64.en",
                         remod::write_bank(bank_of({{7200, o200.substr(0, first)}},
                                                   events({{2, 7200, 1, std::uint32_t(first)}}))));
        test::write_file(wwise / "c.sbnk.1.x64", remod::write_bank(bank_of({{7999, o100}}, events({}))));
        remod::Akpk p;
        p.streams = {{.id = 7200, .data = o200}};
        test::write_file(wwise / "p.spck.1.x64", remod::write_akpk(p, false));
        test::write_file(natives / "streaming/_chainsaw/sound/wwise/p.spck.1.x64", remod::write_akpk(p, true));
    }
};

const remod::SoundFile* file_at(const std::vector<remod::SoundFile>& files, const std::string& game_path) {
    for (const auto& f : files)
        if (f.game_path == game_path) return &f;
    return nullptr;
}

struct NoTextures : remod::ITextureConverter {
    remod::TexMeta load_tex(const fs::path&, const fs::path&, const remod::Profile&) override { return {}; }
    remod::TexMeta save_tex(const fs::path&, const fs::path&, const fs::path&, const remod::Profile&) override {
        return {};
    }
};

}  // namespace

TEST_CASE("sound banks: media and event data read and written back; the sizes the event data records") {
    const std::string wem = remod::encode_wem(opus_like(1, 1, 0x4101, 0), tones(9600, {440}), {});
    const std::string bytes = remod::write_bank(
        bank_of({{5, wem}, {6, "odd"}, {7, ""}}, events({{2, 5, 0, std::uint32_t(wem.size())}, {11, 6, 1, 3}})));
    remod::Bank b = remod::read_bank(bytes);
    REQUIRE(b.media.size() == 3);
    CHECK(b.media[0].data == wem);
    CHECK(b.media[1].data == "odd");
    CHECK(remod::write_bank(b) == bytes);
    const auto sources = remod::bank_sources(b);
    REQUIRE(sources.size() == 2);
    CHECK((sources[0].media == 5 && sources[0].stream == 0 && sources[0].memory == wem.size() && !sources[0].music));
    CHECK((sources[1].media == 6 && sources[1].stream == 1 && sources[1].music));
    remod::set_source_memory(b, sources[1], 1234);
    CHECK(remod::bank_sources(remod::read_bank(remod::write_bank(b)))[1].memory == 1234);
    // Prefetch arithmetic: the header and the first packets, and back.
    const size_t three = remod::wem_prefix(wem, 3);
    CHECK(remod::wem_packets_within(wem, three) == 3);
    CHECK(remod::wem_packets_within(wem, three - 1) == 2);
    CHECK(remod::wem_prefix(wem, 100000) == wem.size());
    CHECK_THROWS_WITH(remod::read_bank("RIFF1234"), ContainsSubstring("not a sound bank"));
}

TEST_CASE("replace_sounds: banks, packages, a streamed sound's first part and every recorded size, in step") {
    SoundTree t;
    std::map<std::uint32_t, bool> music;
    const auto got = remod::replace_sounds(
        t.natives, "_chainsaw/sound/wwise", {7100, 7200, 7300},
        [&](std::uint32_t id, const remod::WemInfo& like, bool is_music) {
            music[id] = is_music;
            return tones(is_music ? like.samples : 36000, std::vector<double>(like.channels, 1000));
        },
        {});
    CHECK(got.missing.empty());
    CHECK(music == std::map<std::uint32_t, bool>{{7100, false}, {7200, false}, {7300, true}});
    REQUIRE(got.files.size() == 4);  // a, b, and both copies of p; c untouched
    const auto* a = file_at(got.files, "_chainsaw/sound/wwise/a.sbnk.1.x64");
    const auto* b = file_at(got.files, "_chainsaw/sound/wwise/b.sbnk.1.x64.en");
    const auto* p = file_at(got.files, "streaming/_chainsaw/sound/wwise/p.spck.1.x64");
    REQUIRE((a && b && p && file_at(got.files, "_chainsaw/sound/wwise/p.spck.1.x64")));

    const remod::Bank bank_a = remod::read_bank(a->bytes);
    CHECK(remod::read_wem_info(bank_a.media[0].data).samples == 36000);  // its own length
    CHECK(remod::read_wem_info(bank_a.media[1].data).samples == 24000);  // a music track's: the original's
    for (const auto& s : remod::bank_sources(bank_a))
        CHECK(s.memory == (s.media == 7100 ? bank_a.media[0] : bank_a.media[1]).data.size());

    const std::string streamed = remod::read_akpk(p->bytes).streams.at(0).data;
    CHECK(remod::decode_wem(streamed, {}).size() == 36000 * 2);
    const remod::Bank bank_b = remod::read_bank(b->bytes);
    const std::string first = bank_b.media.at(0).data;
    CHECK(first == streamed.substr(0, remod::wem_prefix(streamed, 3)));  // as many packets as the original's
    CHECK(remod::bank_sources(bank_b).at(0).memory == first.size());

    CHECK(remod::replace_sounds(t.natives, "_chainsaw/sound/wwise", {7100, 4242}, {}, {}).missing ==
          std::set<std::uint32_t>{4242});
}

TEST_CASE("list_sounds and sound_wem: a bank's and a package's sounds, a streamed one's whole from its package") {
    SoundTree t;
    const fs::path wwise = t.natives / "_chainsaw/sound/wwise";
    const auto in_b = remod::list_sounds(wwise / "b.sbnk.1.x64.en");
    REQUIRE(in_b.size() == 1);
    CHECK(in_b[0].id == 7200);
    CHECK(in_b[0].where == "streamed: its first part");
    CHECK(in_b[0].info.channels == 2);
    const auto in_p = remod::list_sounds(wwise / "p.spck.1.x64");  // the header-only copy: the streaming one's
    REQUIRE(in_p.size() == 1);
    CHECK(in_p[0].where == "in this package");
    CHECK(in_p[0].info.samples == 48000);
    CHECK(remod::sound_wem(wwise / "b.sbnk.1.x64.en", 7200) == t.o200);
    CHECK(remod::sound_wem(wwise / "a.sbnk.1.x64", 7300) == t.o300);
    CHECK(remod::list_sounds(wwise / "a.sbnk.1.x64").at(0).where == "in this bank");
    CHECK(remod::sound_id_in("vo_leon_880852580.wav") == 880852580);
    CHECK(remod::sound_id_in("take2_123.wav") == 0);
    CHECK(remod::wav_bytes(tones(10, {1, 2}), 2, 48000).size() == 44 + 10 * 2 * 2);
}

TEST_CASE("Replace sounds: your files by the ids in their names, into Package; an unknown id named") {
    SoundTree t;
    const fs::path voice = t.dir.path / "audio/voice_7100.wav", effect = t.dir.path / "audio/hit 7200.wav";
    test::write_file(voice, remod::wav_bytes(tones(24000, {500}), 1, 48000));
    test::write_file(effect, remod::wav_bytes(tones(12000, {700, 900}), 2, 48000));
    remod::Graph g;
    g.add_node("Value").params["value"] = voice.string();   // 1
    g.add_node("Value").params["value"] = effect.string();  // 2
    g.add_node("ReplaceSounds").params["game"] = t.natives.string();  // 3
    auto& pack = g.add_node("PackageMod").params;                     // 4
    pack["name"] = "Sounds";
    pack["out"] = "mods";
    pack["replace"] = "true";
    REQUIRE(g.connect({1, "value", 3, "sounds"}) == "");
    REQUIRE(g.connect({2, "value", 3, "sounds"}) == "");
    REQUIRE(g.connect({3, "files", 4, "file"}) == "");
    REQUIRE(g.validate().empty());
    NoTextures conv;
    const remod::Profile profile = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    const remod::RunOptions opt{.profile = profile, .converter = conv, .base_dir = t.dir.path,
                                .cache_dir = t.dir.path / "cache"};
    const auto result = remod::run_graph(g, opt);
    CHECK_THAT(result.nodes.at(3).message, ContainsSubstring("2 sound(s) replaced: 4 sound files"));
    CHECK_THAT(result.nodes.at(4).message, ContainsSubstring("4 other file"));
    const fs::path mod = t.dir.path / "mods/Sounds/natives/STM";
    const auto bank = remod::read_bank(test::read_file(mod / "_chainsaw/sound/wwise/a.sbnk.1.x64"));
    CHECK(remod::read_wem_info(bank.media.at(0).data).samples == 24000);  // voice_7100.wav's own length
    CHECK(fs::exists(mod / "streaming/_chainsaw/sound/wwise/p.spck.1.x64"));
    CHECK_THAT(remod::run_graph(g, opt).nodes.at(3).message, ContainsSubstring("unchanged"));

    test::write_file(t.dir.path / "audio/no id.wav", remod::wav_bytes(tones(100, {500}), 1, 48000));
    g.find(1)->params["value"] = (t.dir.path / "audio/no id.wav").string();
    CHECK_THROWS_WITH(remod::run_graph(g, opt), ContainsSubstring("has no sound id"));
    test::write_file(t.dir.path / "audio/x_4242.wav", remod::wav_bytes(tones(100, {500}), 1, 48000));
    g.find(1)->params["value"] = (t.dir.path / "audio/x_4242.wav").string();
    CHECK_THROWS_WITH(remod::run_graph(g, opt), ContainsSubstring("not in the game's sound files: 4242"));
}

TEST_CASE("listing the castle's music reads headers, not its 837 MB (set REMOD_GAME)") {
    const std::string game = game_dir();
    if (game.empty()) SKIP("set REMOD_GAME to run");
    const fs::path file = fs::path(game) / "_chainsaw/sound/wwise/ch_bgm_castle.spck.1.x64";
    if (!fs::exists(file)) SKIP("needs ch_bgm_castle.spck");
    const auto start = std::chrono::steady_clock::now();
    const auto sounds = remod::list_sounds(file);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(took < 2.0);
    const auto table = remod::read_akpk(test::read_file(file)).streams;  // the header-only copy: ids only
    REQUIRE(sounds.size() == table.size());
    CHECK(std::ranges::all_of(sounds, [](const remod::SoundEntry& s) { return s.info.channels > 0; }));
    const std::string last = remod::sound_wem(file, sounds.back().id);  // a whole WEM: its RIFF size is its own
    REQUIRE(last.size() >= 8);
    std::uint32_t riff = 0;
    std::memcpy(&riff, last.data() + 4, 4);
    CHECK(riff + 8 == last.size());
    CHECK(remod::read_wem_info(last).samples == sounds.back().info.samples);
    // Byte for byte against reading the whole package (a small one).
    const fs::path small = fs::path(game) / "_chainsaw/sound/wwise/ch_mva000_bgm.spck.1.x64";
    const auto whole = remod::read_akpk(test::read_file(fs::path(game) / "streaming/_chainsaw/sound/wwise/ch_mva000_bgm.spck.1.x64"));
    CHECK(remod::sound_wem(small, std::uint32_t(whole.streams.at(0).id)) == whole.streams.at(0).data);
}

TEST_CASE("new_sound_bank: a brand-new bank from a game one, every id it defines new, its sound ours (set REMOD_GAME)") {
    const std::string game = game_dir();
    if (game.empty()) SKIP("set REMOD_GAME to run");
    const fs::path like = fs::path(game) / "_chainsaw/sound/wwise/ch_csa404_se.sbnk.1.x64";
    if (!fs::exists(like)) SKIP("needs ch_csa404_se");
    const std::string was = test::read_file(like);
    const remod::NewSoundBank made = remod::new_sound_bank(
        was, "remod_snd001", [](const remod::WemInfo& info) { return tones(info.rate * 2, std::vector<double>(info.channels, 500)); },
        remod::find_codebooks());
    // The ids each event data object defines (its first u32), old and new: none of the old left, references kept.
    const auto objects = [](const remod::Bank& b) {
        std::vector<std::pair<int, std::uint32_t>> out;
        const std::string& h = std::ranges::find(b.chunks, std::string("HIRC"), &std::pair<std::string, std::string>::first)->second;
        std::uint32_t count, size, id;
        std::memcpy(&count, h.data(), 4);
        for (size_t i = 0, at = 4; i < count; ++i, at += 5 + size) {
            std::memcpy(&size, h.data() + at + 1, 4);
            std::memcpy(&id, h.data() + at + 5, 4);
            out.emplace_back(std::uint8_t(h[at]), id);
        }
        return std::pair{out, h};
    };
    const remod::Bank old_bank = remod::read_bank(was), new_bank = remod::read_bank(made.bytes);
    const auto [old_objects, old_hirc] = objects(old_bank);
    const auto [new_objects, new_hirc] = objects(new_bank);
    REQUIRE(old_objects.size() == new_objects.size());
    for (size_t i = 0; i < old_objects.size(); ++i) {
        CHECK(old_objects[i].first == new_objects[i].first);
        CHECK(old_objects[i].second != new_objects[i].second);
        const std::uint32_t old_id = old_objects[i].second;
        CHECK(new_hirc.find(std::string(reinterpret_cast<const char*>(&old_id), 4)) == std::string::npos);
    }
    CHECK(std::ranges::count(new_objects, std::pair<int, std::uint32_t>{4, made.event_id}) == 1);
    REQUIRE(new_bank.media.size() == 1);
    CHECK(new_bank.media[0].id != old_bank.media[0].id);
    const remod::WemInfo info = remod::read_wem_info(new_bank.media[0].data);
    CHECK(info.samples == info.rate * 2);  // ours: 2 s
    for (const remod::BankSource& s : remod::bank_sources(new_bank))
        if (s.media == new_bank.media[0].id) CHECK(s.memory == new_bank.media[0].data.size());
    CHECK(remod::new_sound_bank(was, "remod_snd001", [](const remod::WemInfo& i) { return tones(i.rate, {500}); },
                                remod::find_codebooks()).event_id == made.event_id);  // the same name, the same ids
}

TEST_CASE("the game's banks, and a streamed dialogue line replaced in step (set REMOD_GAME)") {
    const std::string game = game_dir();
    if (game.empty()) SKIP("set REMOD_GAME to run");
    const fs::path wwise = fs::path(game) / "_chainsaw/sound/wwise";
    if (!fs::exists(wwise / "ch_mva000_bgm.sbnk.1.x64")) SKIP("needs _chainsaw/sound/wwise");
    for (const char* name : {"ch_mva000_bgm.sbnk.1.x64", "ch_mva000_dialogue.sbnk.1.x64.en",
                             "ao_cha8_voice_emo_media.sbnk.1.x64.en", "ch_bgm_castle.sbnk.1.x64"}) {
        CAPTURE(name);
        const std::string bytes = test::read_file(wwise / name);
        CHECK(remod::write_bank(remod::read_bank(bytes)) == bytes);
    }
    // The intro's English dialogue: streamed, its first part (header and some packets) in its bank.
    const std::uint32_t id = 880852580;
    const auto got = remod::replace_sounds(fs::path(game), "_chainsaw/sound/wwise", {id},
                                           [](std::uint32_t, const remod::WemInfo& like, bool) {
                                               return tones(48000, std::vector<double>(like.channels, 500));
                                           },
                                           remod::find_codebooks());
    REQUIRE(got.missing.empty());
    const auto* bank = file_at(got.files, "_chainsaw/sound/wwise/ch_mva000_dialogue.sbnk.1.x64.en");
    const auto* package = file_at(got.files, "streaming/_chainsaw/sound/wwise/ch_mva000_dialogue.spck.1.x64.en");
    REQUIRE((bank && package));
    const std::string original =
        remod::read_akpk(test::read_file(fs::path(game) / "streaming/_chainsaw/sound/wwise/ch_mva000_dialogue.spck.1.x64.en"))
            .streams.at(0)
            .data;
    const std::string streamed = remod::read_akpk(package->bytes).streams.at(0).data;
    const remod::Bank now = remod::read_bank(bank->bytes);
    const remod::Bank was = remod::read_bank(test::read_file(wwise / "ch_mva000_dialogue.sbnk.1.x64.en"));
    const std::string first = std::ranges::find(now.media, id, &remod::BankMedia::id)->data;
    const std::string first_was = std::ranges::find(was.media, id, &remod::BankMedia::id)->data;
    CHECK(streamed.starts_with(first));
    CHECK(remod::wem_packets_within(streamed, first.size()) == remod::wem_packets_within(original, first_was.size()));
    for (const auto& s : remod::bank_sources(now))
        if (s.media == id) CHECK(s.memory == first.size());
    CHECK(remod::decode_wem(streamed, {}).size() == 48000 * 3);
}

TEST_CASE("Game sound blocks: a sound picked by its file and id, your audio or silence, into Replace sounds") {
    SoundTree t;
    std::string file;
    std::uint32_t id = 0;
    CHECK(remod::parse_game_sound(remod::game_sound_text("{game}/x.sbnk.1.x64.en", 7100), file, id));
    CHECK((file == "{game}/x.sbnk.1.x64.en" && id == 7100));
    CHECK_FALSE(remod::parse_game_sound("x.sbnk.1.x64", file, id));
    CHECK_FALSE(remod::parse_game_sound("x.sbnk.1.x64#12a", file, id));
    CHECK_THAT(remod::path_fit(remod::PathKind::GameSound, nullptr, "x.wav", false), ContainsSubstring("game sound"));

    const fs::path wwise = t.natives / "_chainsaw/sound/wwise";
    const fs::path mine = t.dir.path / "audio/anything.wav";  // no id in its name
    test::write_file(mine, remod::wav_bytes(tones(30000, {500}), 1, 48000));
    remod::Graph g;
    auto& voice = g.add_node("GameSound").params;  // 1
    voice["sound"] = remod::game_sound_text((wwise / "a.sbnk.1.x64").string(), 7100);
    voice["audio"] = mine.string();
    g.add_node("GameSound").params["sound"] = remod::game_sound_text((wwise / "b.sbnk.1.x64.en").string(), 7200);  // 2: silence
    g.add_node("ReplaceSounds").params["game"] = t.natives.string();  // 3
    auto& pack = g.add_node("PackageMod").params;                     // 4
    pack["name"] = "Picked";
    pack["out"] = "mods";
    pack["replace"] = "true";
    REQUIRE(g.connect({1, "sound", 3, "sounds"}) == "");
    REQUIRE(g.connect({2, "sound", 3, "sounds"}) == "");
    REQUIRE(g.connect({3, "files", 4, "file"}) == "");
    REQUIRE(g.validate().empty());
    NoTextures conv;
    const remod::Profile profile = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    const remod::RunOptions opt{.profile = profile, .converter = conv, .base_dir = t.dir.path,
                                .cache_dir = t.dir.path / "cache"};
    const auto result = remod::run_graph(g, opt);
    CHECK_THAT(result.nodes.at(1).message, ContainsSubstring("sound 7100: anything.wav"));
    CHECK_THAT(result.nodes.at(2).message, ContainsSubstring("silence"));
    CHECK_THAT(result.nodes.at(3).message, ContainsSubstring("2 sound(s) replaced"));
    const fs::path mod = t.dir.path / "mods/Picked/natives/STM";
    const auto a = remod::read_bank(test::read_file(mod / "_chainsaw/sound/wwise/a.sbnk.1.x64"));
    CHECK(remod::read_wem_info(a.media.at(0).data).samples == 30000);
    const auto p = remod::read_akpk(test::read_file(mod / "streaming/_chainsaw/sound/wwise/p.spck.1.x64"));
    const auto silent = remod::decode_wem(p.streams.at(0).data, {});
    CHECK(silent.size() == 48000 * 2);  // the original's length
    CHECK(std::ranges::all_of(silent, [](std::int16_t v) { return std::abs(int(v)) < 200; }));

    // Straight into Package: refused, pointing to Replace sounds.
    remod::Graph wrong;
    wrong.add_node("GameSound").params = g.find(1)->params;
    wrong.add_node("PackageMod").params = pack;
    REQUIRE(wrong.connect({1, "sound", 2, "file"}) == "");
    CHECK_THROWS_WITH(remod::run_graph(wrong, opt), ContainsSubstring("goes into Replace sounds"));
    // Two picks of one sound.
    g.find(2)->params["sound"] = g.find(1)->params["sound"];
    CHECK_THROWS_WITH(remod::run_graph(g, opt), ContainsSubstring("two sounds for game sound 7100"));
}
