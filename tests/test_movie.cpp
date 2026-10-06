#include "graph.hpp"
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

TEST_CASE("encode_movie: a test card, then a video, at the size and frame rate of the movie they replace") {
    TempDir dir;
    const fs::path card = dir.path / "card.mp4", video = dir.path / "video.mp4";
    remod::encode_movie({}, {.width = 320, .height = 240, .seconds = 2, .fps = 30, .bitrate = 2'000'000}, "mva000",
                        card);
    const remod::MovieInfo c = remod::read_mp4_info(card);
    CHECK(c.width == 320);
    CHECK(c.height == 240);
    CHECK(c.video == "avc1");
    CHECK(c.audio.empty());
    CHECK_THAT(c.seconds, WithinAbs(2, 0.05));
    CHECK_THAT(c.fps, WithinAbs(30, 0.1));

    // The card as someone's video: decoded, scaled, retimed to 29.97 (the game's 1080p copies).
    remod::encode_movie(card, {.width = 640, .height = 360, .seconds = 9, .fps = 29.97, .bitrate = 2'000'000}, "",
                        video);
    const remod::MovieInfo v = remod::read_mp4_info(video);
    CHECK(v.width == 640);
    CHECK(v.height == 360);
    CHECK_THAT(v.seconds, WithinAbs(2, 0.05));  // the video's length, not the original's
    CHECK_THAT(v.fps, WithinAbs(29.97, 0.05));
    CHECK_THROWS_WITH(remod::encode_movie(dir.path / "missing.mp4", c, "", dir.path / "x.mp4"),
                      ContainsSubstring("can't open"));
}

TEST_CASE("Replace movie: both copies at their in-game paths, packaged; one without a 1080p copy; the stub refused") {
    TempDir dir;
    const fs::path natives = dir.path / "natives/STM", folder = natives / "streaming/_chainsaw/movie/mv/mva000";
    fs::create_directories(folder);
    remod::encode_movie({}, {.width = 320, .height = 180, .seconds = 1, .fps = 30, .bitrate = 1'000'000}, "a",
                        folder / "mva000.mov.1.x64");
    remod::encode_movie({}, {.width = 160, .height = 90, .seconds = 1, .fps = 29.97, .bitrate = 1'000'000}, "b",
                        folder / "mva000_fhd.mov.1.x64");
    remod::Graph g;
    g.add_node("ReplaceMovie").params["movie"] = (folder / "mva000_fhd.mov.1.x64").string();  // 1: either copy
    auto& pack = g.add_node("PackageMod").params;                                               // 2
    pack["name"] = "MovieTest";
    pack["out"] = "mods";
    pack["replace"] = "true";
    REQUIRE(g.connect({1, "movie", 2, "file"}) == "");
    REQUIRE(g.connect({1, "fhd", 2, "file"}) == "");
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
    CHECK_THAT(result.nodes.at(1).message, ContainsSubstring("test card"));
    const fs::path mod = dir.path / "mods/MovieTest/natives/STM/streaming/_chainsaw/movie/mv/mva000";
    CHECK(remod::read_mp4_info(mod / "mva000.mov.1.x64").width == 320);
    CHECK(remod::read_mp4_info(mod / "mva000_fhd.mov.1.x64").width == 160);
    CHECK_THAT(remod::read_mp4_info(mod / "mva000_fhd.mov.1.x64").fps, WithinAbs(29.97, 0.05));
    CHECK(fs::exists(dir.path / "mods/MovieTest.zip"));
    // Run again: nothing encoded.
    CHECK_THAT(remod::run_graph(g, opt).nodes.at(1).message, ContainsSubstring("unchanged"));

    // No 1080p copy: that output gives nothing, and only the movie is packaged.
    fs::remove(folder / "mva000_fhd.mov.1.x64");
    g.find(1)->params["movie"] = (folder / "mva000.mov.1.x64").string();
    const auto alone = remod::run_graph(g, opt);
    CHECK_THAT(alone.nodes.at(2).message, ContainsSubstring("1 other file"));

    // The 38-byte stub outside streaming\ isn't the movie.
    test::write_file(natives / "_chainsaw/movie/mv/mva000/mva000.mov.1.x64", "REMV" + std::string(34, '\0'));
    g.find(1)->params["movie"] = (natives / "_chainsaw/movie/mv/mva000/mva000.mov.1.x64").string();
    CHECK_THROWS_WITH(remod::run_graph(g, opt), ContainsSubstring("streaming"));

    // Package with nothing linked: its run says so (a texture is no longer required: a mod can be movies only).
    remod::Graph empty;
    empty.add_node("PackageMod").params = pack;
    CHECK_THROWS_WITH(remod::run_graph(empty, opt), ContainsSubstring("nothing to package"));
}

TEST_CASE("encode_movie: same_length cuts a longer video and holds a shorter one's last frame") {
    TempDir dir;
    const fs::path two = dir.path / "two.mp4", out = dir.path / "out.mp4";
    remod::encode_movie({}, {.width = 320, .height = 240, .seconds = 2, .fps = 30, .bitrate = 1'000'000}, "x", two);
    remod::encode_movie(two, {.width = 320, .height = 240, .seconds = 3, .fps = 30, .bitrate = 1'000'000}, "", out, true);
    CHECK_THAT(remod::read_mp4_info(out).seconds, WithinAbs(3, 0.05));
    remod::encode_movie(two, {.width = 320, .height = 240, .seconds = 1, .fps = 30, .bitrate = 1'000'000}, "", out, true);
    CHECK_THAT(remod::read_mp4_info(out).seconds, WithinAbs(1, 0.05));
}

TEST_CASE("Export movie -> Edit video -> Replace movie: waits for your edit, then encodes it at the original's length") {
    TempDir dir;
    const fs::path folder = dir.path / "natives/STM/streaming/_chainsaw/movie/mv/mva402";
    fs::create_directories(folder);
    const fs::path movie = folder / "mva402.mov.1.x64", edited = dir.path / "edits/mva402_edited.mp4";
    remod::encode_movie({}, {.width = 320, .height = 240, .seconds = 2, .fps = 30, .bitrate = 1'000'000}, "game", movie);
    remod::Graph g;
    g.add_node("Value").params["value"] = movie.string();              // 1
    g.add_node("Split");                                               // 2
    g.add_node("ExportMovie").params["video"] = "edits/mva402.mp4";    // 3
    g.add_node("EditVideo");                                           // 4
    g.add_node("ReplaceMovie");                                        // 5
    for (const remod::Link& l : {remod::Link{1, "value", 2, "in"}, remod::Link{2, "out", 3, "movie"},
                                 remod::Link{2, "out", 5, "movie"}, remod::Link{3, "video", 4, "video"},
                                 remod::Link{4, "video", 5, "video"}})
        REQUIRE(g.connect(l) == "");
    REQUIRE(g.validate().empty());
    struct NoTextures : remod::ITextureConverter {
        remod::TexMeta load_tex(const fs::path&, const fs::path&, const remod::Profile&) override { return {}; }
        remod::TexMeta save_tex(const fs::path&, const fs::path&, const fs::path&, const remod::Profile&) override {
            return {};
        }
    } conv;
    const remod::Profile profile = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    const remod::RunOptions opt{.profile = profile, .converter = conv, .base_dir = dir.path,
                                .cache_dir = dir.path / "cache"};  // the encodes outlive the run

    // First run: the movie is copied out for you, and the run waits.
    auto first = remod::run_graph(g, opt);
    remod::apply_run(g, first);
    CHECK(first.nodes.at(4).state == remod::NodeState::Waiting);
    CHECK_THAT(first.nodes.at(4).message, ContainsSubstring("mva402_edited.mp4"));
    CHECK(first.nodes.at(5).state == remod::NodeState::NotReached);
    CHECK(test::read_file(dir.path / "edits/mva402.mp4") == test::read_file(movie));

    // Done without rendering the edit: the run says where it should be.
    remod::set_edit_done(g, 4, true);
    CHECK_THROWS_WITH(remod::run_graph(g, opt), ContainsSubstring("your edit isn't there"));

    // Your edit, 1 s long: the copy is kept, and the edit encoded at the original's 2 s (its last frame held).
    remod::encode_movie({}, {.width = 640, .height = 360, .seconds = 1, .fps = 30, .bitrate = 1'000'000}, "edit", edited);
    const auto built = remod::run_graph(g, opt);
    CHECK_THAT(built.nodes.at(3).message, ContainsSubstring("kept your"));
    CHECK(built.nodes.at(5).state == remod::NodeState::Done);
    const remod::MovieInfo out = remod::read_mp4_info(built.values.at({5, "movie"}));
    CHECK(out.width == 320);
    CHECK_THAT(out.seconds, WithinAbs(2, 0.05));
    g.find(5)->params["same_length"] = "false";  // its own length
    CHECK_THAT(remod::read_mp4_info(remod::run_graph(g, opt).values.at({5, "movie"})).seconds, WithinAbs(1, 0.05));

    // Another movie: exported again over the old copy, and the edit isn't done any more.
    remod::encode_movie({}, {.width = 320, .height = 240, .seconds = 1, .fps = 30, .bitrate = 1'000'000}, "other",
                        folder / "mva403.mov.1.x64");
    g.find(1)->params["value"] = (folder / "mva403.mov.1.x64").string();
    const auto other = remod::run_graph(g, opt);
    CHECK(other.nodes.at(3).message.starts_with("exported"));
    CHECK(other.nodes.at(4).state == remod::NodeState::Waiting);
}
