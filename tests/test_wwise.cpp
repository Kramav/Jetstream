#include "graph.hpp"
#include "movie.hpp"
#include "profile.hpp"
#include "wwise.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
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

TEST_CASE("Opus WEMs: libopus at the original's channels and length, back again; 3 and 6 channels in a WAV's order") {
    for (const auto& [channels, config, family] : {std::tuple{1, 0x4101u, 0}, {2, 0x3102u, 0}, {3, 0x7103u, 1},
                                                   {6, 0x3F106u, 1}}) {
        CAPTURE(channels);
        std::vector<double> freqs{440, 660, 880, 1100, 1320, 1540};
        freqs.resize(size_t(channels));
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
            if (channels != 6 || c != 3) CHECK(snr(pcm, c, back, c, channels) > 10);
    }
    CHECK_THROWS_WITH(remod::encode_wem(opus_like(12, 1, 0, 255), std::vector<std::int16_t>(12), {}),
                      ContainsSubstring("can't write yet"));
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
    CHECK_THAT(result.nodes.at(1).message, ContainsSubstring("2 sound packages (effects: a beep each second"));
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
    CHECK_THAT(remod::run_graph(g, opt).nodes.at(1).message, ContainsSubstring("sound packages unchanged"));

    // Off: no packages.
    g.find(1)->params["sound"] = "false";
    CHECK_THAT(remod::run_graph(g, opt).nodes.at(2).message, ContainsSubstring("1 other file"));
}
