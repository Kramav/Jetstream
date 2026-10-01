#include "browse.hpp"
#include "graph.hpp"
#include "texture_converter.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

using test::TempDir;
namespace fs = std::filesystem;

namespace {

// A .tex with real mip entries: each mip is {pitch, size}, data filled with the mip's number.
fs::path tex_with_mips(const fs::path& file, std::uint16_t w, std::uint16_t h, std::uint32_t dxgi,
                       const std::vector<std::pair<std::uint32_t, std::uint32_t>>& mips) {
    std::string b(40 + 16 * mips.size(), '\0');
    test::put_le(b, 0, 0x00584554, 4);
    test::put_le(b, 4, 143221013, 4);
    test::put_le(b, 8, w, 2);
    test::put_le(b, 10, h, 2);
    b[14] = 1;
    b[15] = char(mips.size() * 16);
    test::put_le(b, 16, dxgi, 4);
    for (size_t i = 0; i < mips.size(); ++i) {
        const size_t e = 40 + 16 * i;
        test::put_le(b, e, std::uint32_t(b.size()), 4);  // offset (u64, high half 0)
        test::put_le(b, e + 8, mips[i].first, 4);
        test::put_le(b, e + 12, mips[i].second, 4);
        b += std::string(mips[i].second, char('0' + i));
    }
    test::write_file(file, b);
    return file;
}

std::string utf16(const std::string& s) {
    std::string out;
    for (char c : s) out += {c, '\0'};
    return out + std::string(2, '\0');
}

// An .mdf2 in the layout mesh_textures reads (see read_mdf2): materials of 100 bytes from 0x10, then each one's
// texture headers (0x20 bytes), then the UTF-16 strings.
std::string mdf2(const std::vector<std::pair<std::string, std::vector<std::string>>>& materials) {
    std::string b(0x10 + materials.size() * 100, '\0'), strings;
    size_t headers = 0;
    for (const auto& m : materials) headers += m.second.size() * 0x20;
    const size_t text_at = b.size() + headers;
    auto add = [&](const std::string& s) {
        const size_t at = text_at + strings.size();
        strings += utf16(s);
        return std::uint32_t(at);
    };
    test::put_le(b, 6, std::uint32_t(materials.size()), 2);
    std::string header_bytes;
    for (size_t i = 0; i < materials.size(); ++i) {
        const size_t m = 0x10 + i * 100;
        test::put_le(b, m, add(materials[i].first), 4);
        test::put_le(b, m + 20, std::uint32_t(materials[i].second.size()), 4);
        test::put_le(b, m + 60, std::uint32_t(0x10 + materials.size() * 100 + header_bytes.size()), 4);
        for (const auto& t : materials[i].second) {
            std::string h(0x20, '\0');
            test::put_le(h, 0, add("BaseMap"), 4);
            test::put_le(h, 16, add(t), 4);
            header_bytes += h;
        }
    }
    return b + header_bytes + strings;
}

std::string env(const char* name) {
    char* v = nullptr;
    size_t n = 0;
    _dupenv_s(&v, &n, name);
    std::string s = v ? v : "";
    std::free(v);
    return s;
}

}  // namespace

TEST_CASE("read_tex_pixels picks the mip that fits and reports the stored layout") {
    TempDir dir;
    // BC7 (16 bytes per 4x4 block), 8x8: mip 0 is 2x2 blocks, mip 1 one block.
    const fs::path tex = tex_with_mips(dir.path / "a.tex.143221013", 8, 8, 99, {{32, 64}, {16, 16}});

    auto p = remod::read_tex_pixels(tex, 8);
    CHECK((p.width == 8 && p.height == 8 && p.stored_width == 8 && p.stored_height == 8));
    CHECK((p.format == 99 && p.row_pitch == 32 && p.data.size() == 64 && p.data[0] == '0'));

    p = remod::read_tex_pixels(tex, 4);
    CHECK((p.width == 4 && p.stored_width == 4 && p.data.size() == 16 && p.data[0] == '1'));
    CHECK(remod::read_tex_pixels(tex, 1).width == 4);  // nothing fits: the smallest
}

TEST_CASE("read_tex_pixels keeps the padded row width separate from the visible width") {
    TempDir dir;
    // 6x2 RGBA8 stored with 8-pixel rows (like cs_ui3200_stamp_im: 468 wide, stored 512).
    const auto p = remod::read_tex_pixels(tex_with_mips(dir.path / "a.tex.143221013", 6, 2, 28, {{32, 64}}), 4096);
    CHECK((p.width == 6 && p.height == 2 && p.stored_width == 8 && p.stored_height == 2));
}

TEST_CASE("read_tex_pixels refuses what it can't show") {
    TempDir dir;
    CHECK_THROWS_AS(remod::read_tex_pixels(tex_with_mips(dir.path / "a.tex.1", 4, 4, 999, {{16, 16}}), 64),
                    remod::ConvertError);  // unknown format
    const fs::path cut = tex_with_mips(dir.path / "b.tex.1", 8, 8, 99, {{32, 64}});
    test::write_file(cut, test::read_file(cut).substr(0, 60));
    CHECK_THROWS_AS(remod::read_tex_pixels(cut, 64), remod::ConvertError);
    test::write_file(dir.path / "c.tex.1", "not a texture at all, just some bytes here");
    CHECK_THROWS_AS(remod::read_tex_pixels(dir.path / "c.tex.1", 64), remod::ConvertError);
}

TEST_CASE("read_tex_pixels matches read_tex_meta on real textures") {
    const std::string fixtures = env("REMOD_FIXTURES");
    if (fixtures.empty()) SKIP("REMOD_FIXTURES not set");
    const remod::Profile profile = remod::load_profile(REMOD_PROFILES_DIR "/re4r.toml");
    int n = 0;
    for (const auto& e : fs::directory_iterator(fixtures)) {
        if (e.path().extension() != "." + profile.tex_suffix) continue;
        const auto meta = remod::read_tex_meta(e.path(), profile);
        const auto p = remod::read_tex_pixels(e.path(), 1u << 16);
        CHECK((p.width == meta.width && p.height == meta.height));
        ++n;
    }
    CHECK(n > 0);
}

TEST_CASE("index_assets lists textures and meshes, sorted, without streaming copies") {
    TempDir dir;
    for (const char* f : {"b/x.tex.143221013", "M/y.mesh.221108797", "a/c.TEX.143221013", "streaming/b/x.tex.143221013",
                          "a/readme.txt", "a/plain.tex", "a/m.mdf2.32"})
        test::write_file(dir.path / f, "x");
    const auto index = remod::index_assets(dir.path);
    CHECK(index.textures == std::vector<std::string>{"a/c.TEX.143221013", "b/x.tex.143221013"});
    CHECK(index.meshes == std::vector<std::string>{"M/y.mesh.221108797"});
    CHECK(remod::index_assets(dir.path / "").textures == index.textures);  // a root with a final slash
}

TEST_CASE("folder_tree and search") {
    const std::vector<std::string> paths{"a/b/x", "a/c/y", "a/b/z", "w"};
    const auto tree = remod::folder_tree(paths);
    REQUIRE(tree.folders[0].children.size() == 1);
    CHECK(tree.folders[0].files == std::vector<size_t>{3});
    const auto& a = tree.folders[tree.folders[0].children[0]];
    REQUIRE(a.children.size() == 2);
    CHECK(tree.folders[a.children[0]].name == "b");
    CHECK(tree.folders[a.children[0]].files == std::vector<size_t>{0, 2});

    CHECK(remod::search(paths, "A/B") == std::vector<size_t>{0, 2});
    CHECK(remod::search(paths, "z a") == std::vector<size_t>{2});
    CHECK(remod::search(paths, "  ").size() == 4);
    CHECK(remod::file_name("a/b/x") == "x");
    CHECK(remod::file_name("w") == "w");
}

TEST_CASE("mesh_textures reads the material next to the mesh") {
    TempDir dir;
    test::write_file(dir.path / "ch/body.mesh.221108797", "x");
    test::write_file(dir.path / "ch/body_mat.mdf2.32",
                     mdf2({{"Skin_Mat", {"CH/Body_NRMR.tex", "CH/Body_ALBM.tex", "@CH/Body_ALBD.tex"}},
                           {"Cloth_Mat", {"CH/Body_ALBM.tex", "ch/missing_ALBD.tex", "x/fx_rtt.rtex"}},
                           {"Bare_Mat", {}}}));
    const std::vector<std::string> textures{"aa.tex.1", "ch/body_albd.tex.143221013", "ch/body_albm.tex.143221013",
                                            "ch/body_nrmr.tex.143221013"};

    auto m = remod::mesh_textures(dir.path, "ch/body.mesh.221108797", textures);
    CHECK(m.material == "ch/body_mat.mdf2.32");
    CHECK(m.textures == std::vector<std::string>{"ch/body_nrmr.tex.143221013", "ch/body_albm.tex.143221013",
                                                 "ch/body_albd.tex.143221013", "ch/missing_ALBD.tex"});
    CHECK(m.found == std::vector<bool>{true, true, true, false});
    REQUIRE(m.materials.size() == 3);
    CHECK(m.materials[0].name == "Skin_Mat");
    CHECK(m.materials[0].textures == std::vector<size_t>{0, 1, 2});
    CHECK(m.materials[0].albedo == 2);  // _albd preferred over _albm
    CHECK(m.materials[1].textures == std::vector<size_t>{1, 3});  // .rtex left out
    CHECK(m.materials[1].albedo == 1);  // the _albd one isn't in the files
    CHECK(m.materials[2].albedo == -1);

    test::write_file(dir.path / "ch/body.mdf2.32", mdf2({{"Other", {"ch/body_albd.tex"}}}));  // the exact name wins
    m = remod::mesh_textures(dir.path, "ch/body.mesh.221108797", textures);
    CHECK(m.material == "ch/body.mdf2.32");
    CHECK(m.textures == std::vector<std::string>{"ch/body_albd.tex.143221013"});

    test::write_file(dir.path / "ch/body.mdf2.32", mdf2({{"Other", {}}}).substr(0, 30));  // cut short
    CHECK_THROWS(remod::mesh_textures(dir.path, "ch/body.mesh.221108797", textures));

    test::write_file(dir.path / "lone/thing.mesh.221108797", "x");
    CHECK_THROWS(remod::mesh_textures(dir.path, "lone/thing.mesh.221108797", textures));
}

TEST_CASE("preview_file prefers the streaming copy") {
    TempDir dir;
    test::write_file(dir.path / "a/x.tex.1", "x");
    test::write_file(dir.path / "a/y.tex.1", "x");
    test::write_file(dir.path / "streaming/a/y.tex.1", "x");
    CHECK(remod::preview_file(dir.path, "a/x.tex.1") == dir.path / "a/x.tex.1");
    CHECK(remod::preview_file(dir.path, "a/y.tex.1") == dir.path / "streaming/a/y.tex.1");
}

TEST_CASE("texture_target: the selected Original texture block, else the only one") {
    remod::Graph g;
    const int a = g.add_node("LoadTex").id;
    const int other = g.add_node("ExportImage").id;
    CHECK(remod::texture_target(g, 0) == a);
    CHECK(remod::texture_target(g, other) == a);
    const int b = g.add_node("LoadTex").id;
    CHECK(remod::texture_target(g, 0) == 0);
    CHECK(remod::texture_target(g, b) == b);
}

