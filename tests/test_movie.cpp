#include "movie.hpp"
#include "package.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using test::TempDir;
namespace fs = std::filesystem;

namespace {

std::string be32(std::uint32_t v) { return {char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; }
std::string box(const std::string& type, const std::string& payload) { return be32(std::uint32_t(8 + payload.size())) + type + payload; }

// One track: tkhd (version 0, size in 16.16), mdia: mdhd (timescale, duration), hdlr, minf / stbl: stsd (one entry of
// `codec`), stts (`frames` samples).
std::string track(const std::string& handler, const std::string& codec, unsigned w, unsigned h, std::uint32_t timescale,
                  std::uint32_t duration, std::uint32_t frames) {
    std::string tkhd(84, '\0');
    tkhd.replace(76, 4, be32(w << 16));
    tkhd.replace(80, 4, be32(h << 16));
    std::string mdhd(24, '\0');
    mdhd.replace(12, 4, be32(timescale));
    mdhd.replace(16, 4, be32(duration));
    const std::string hdlr = std::string(8, '\0') + handler + std::string(12, '\0');
    const std::string stsd = std::string(4, '\0') + be32(1) + box(codec, std::string(8, '\0'));
    const std::string stts = std::string(4, '\0') + be32(1) + be32(frames) + be32(1);
    return box("trak", box("tkhd", tkhd) +
                           box("mdia", box("mdhd", mdhd) + box("hdlr", hdlr) +
                                           box("minf", box("stbl", box("stsd", stsd) + box("stts", stts)))));
}

}  // namespace

TEST_CASE("read_mp4_info: size, length, frame rate and codecs, with the index after the data") {
    TempDir dir;
    const fs::path file = dir.path / "mva000.mov.1.x64";
    const std::string ftyp = box("ftyp", "mp42" + be32(0) + "mp42isom");
    test::write_file(file, ftyp + box("mdat", std::string(100, 'x')) +
                               box("moov", track("vide", "avc1", 1920, 1080, 30000, 300300, 300) +
                                               track("soun", "mp4a", 0, 0, 48000, 480000, 469)));
    const remod::MovieInfo m = remod::read_mp4_info(file);
    CHECK(m.width == 1920);
    CHECK(m.height == 1080);
    CHECK_THAT(m.seconds, WithinAbs(10.01, 0.001));
    CHECK_THAT(m.fps, WithinAbs(29.97, 0.01));
    CHECK(m.video == "avc1");
    CHECK(m.audio == "mp4a");

    test::write_file(file, ftyp + box("moov", track("vide", "avc1", 3840, 2160, 600, 36540, 3654)));  // silent
    CHECK(remod::read_mp4_info(file).audio.empty());
    test::write_file(file, "REMV" + std::string(34, '\0'));  // the 38-byte .mov beside the streaming one
    CHECK_THROWS_WITH(remod::read_mp4_info(file), ContainsSubstring("isn't an MP4"));
    test::write_file(file, ftyp + be32(4000) + "moov");  // cut short
    CHECK_THROWS(remod::read_mp4_info(file));
}

TEST_CASE("a movie packages as it is, at its streaming path") {
    TempDir dir;
    const fs::path mine = dir.path / "mine.mp4";
    test::write_file(mine, box("ftyp", "mp42" + be32(0)) + box("moov", ""));
    const remod::Profile profile = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    const remod::PackageSpec spec{
        .mod_name = "MyIntro",
        .out_dir = dir.path / "out",
        .info = {.name = "MyIntro"},
        .files = {{mine, "streaming/_chainsaw/movie/mv/mva000/mva000.mov.1.x64"},
                  {mine, "streaming/_chainsaw/movie/mv/mva000/mva000_fhd.mov.1.x64"}},
        .zip = false};
    const fs::path root = remod::build_package(profile, spec);
    for (const char* name : {"mva000.mov.1.x64", "mva000_fhd.mov.1.x64"})
        CHECK(test::read_file(root / "natives/STM/streaming/_chainsaw/movie/mv/mva000" / name) == test::read_file(mine));
}

TEST_CASE("read_mp4_info on the game's own movies (set REMOD_GAME to the extracted natives/STM)") {
    char* v = nullptr;
    size_t n = 0;
    _dupenv_s(&v, &n, "REMOD_GAME");
    const std::string game = v ? v : "";
    std::free(v);
    if (game.empty()) SKIP("set REMOD_GAME to run");
    const fs::path mv = fs::path(game) / "streaming/_chainsaw/movie/mv";
    if (!fs::exists(mv / "mva000/mva000.mov.1.x64")) SKIP("needs streaming/_chainsaw/movie/mv/mva000");
    const remod::MovieInfo intro = remod::read_mp4_info(mv / "mva000/mva000.mov.1.x64");
    CHECK(intro.width == 3840);
    CHECK(intro.height == 2160);
    CHECK(intro.video == "avc1");
    CHECK(intro.audio.empty());  // its sound comes from the game's sound bank
    CHECK_THAT(intro.seconds, WithinAbs(60.9, 0.1));
    CHECK(remod::read_mp4_info(mv / "mva000/mva000_fhd.mov.1.x64").width == 1920);
}
