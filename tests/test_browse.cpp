#include "browse.hpp"
#include "graph.hpp"
#include "texture_converter.hpp"

#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdlib>
#include <tuple>

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

struct TestMaterial {
    std::string name;
    std::vector<std::pair<std::string, std::string>> textures;  // (type, path)
    std::string master;
    std::vector<float> base_color;  // empty: no BaseColor parameter
};

// An .mdf2 in the layout mesh_textures reads (see read_mdf2): materials of 100 bytes from 0x10, then each one's
// texture headers (0x20 bytes) and BaseColor parameter (header and values), then the UTF-16 strings.
std::string mdf2(const std::vector<TestMaterial>& materials) {
    std::string b(0x10 + materials.size() * 100, '\0'), strings;
    size_t headers = 0;
    for (const auto& m : materials) headers += m.textures.size() * 0x20 + (m.base_color.empty() ? 0 : 0x18 + 16);
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
        test::put_le(b, m, add(materials[i].name), 4);
        test::put_le(b, m + 84, add(materials[i].master), 4);
        test::put_le(b, m + 20, std::uint32_t(materials[i].textures.size()), 4);
        test::put_le(b, m + 60, std::uint32_t(0x10 + materials.size() * 100 + header_bytes.size()), 4);
        for (const auto& [type, path] : materials[i].textures) {
            std::string h(0x20, '\0');
            test::put_le(h, 0, add(type), 4);
            test::put_le(h, 16, add(path), 4);
            header_bytes += h;
        }
        if (!materials[i].base_color.empty()) {  // one 4-float parameter, its values right after its header
            const size_t at = 0x10 + materials.size() * 100 + header_bytes.size();
            test::put_le(b, m + 16, 1u, 4);
            test::put_le(b, m + 52, std::uint32_t(at), 4);
            test::put_le(b, m + 76, std::uint32_t(at + 0x18), 4);
            std::string h(0x18 + 16, '\0');
            test::put_le(h, 0, add("BaseColor"), 4);
            test::put_le(h, 20, 4u, 4);
            for (size_t c = 0; c < 4; ++c)
                test::put_le(h, 0x18 + 4 * c, std::bit_cast<std::uint32_t>(materials[i].base_color[c]), 4);
            header_bytes += h;
        }
    }
    return b + header_bytes + strings;
}

std::vector<std::pair<std::string, std::string>> base_maps(const std::vector<std::string>& paths) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& p : paths) out.emplace_back("BaseMap", p);
    return out;
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
                     mdf2({{"Skin_Mat", base_maps({"CH/Body_NRMR.tex", "CH/Body_ALBM.tex", "@CH/Body_ALBD.tex"})},
                           {"Cloth_Mat", base_maps({"CH/Body_ALBM.tex", "ch/missing_ALBD.tex", "x/fx_rtt.rtex"})},
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

    test::write_file(dir.path / "ch/body.mdf2.32", mdf2({{"Other", base_maps({"ch/body_albd.tex"})}}));  // exact name wins
    m = remod::mesh_textures(dir.path, "ch/body.mesh.221108797", textures);
    CHECK(m.material == "ch/body.mdf2.32");
    CHECK(m.textures == std::vector<std::string>{"ch/body_albd.tex.143221013"});

    test::write_file(dir.path / "ch/body.mdf2.32", mdf2({{"Other", {}}}).substr(0, 30));  // cut short
    CHECK_THROWS(remod::mesh_textures(dir.path, "ch/body.mesh.221108797", textures));

    test::write_file(dir.path / "lone/thing.mesh.221108797", "x");
    CHECK_THROWS(remod::mesh_textures(dir.path, "lone/thing.mesh.221108797", textures));
}

TEST_CASE("mesh_textures follows the plugin's rules for colour, cut-outs and hidden materials") {
    TempDir dir;
    test::write_file(dir.path / "ch/hair.mesh.221108797", "x");
    test::write_file(
        dir.path / "ch/hair.mdf2.32",
        mdf2({{"Hair_Mat",
               {{"BaseShiftMap", "ch/hair_ALBD.tex"}, {"AlphaTranslucentOcclusionCavityMap", "ch/hair_ATOC.tex"}},
               "_Chainsaw/MasterMaterial/Character_Hair",
               {0.5f, 0.25f, 0.125f, 1}},
              {"Cloth_Mat",  // not a cut-out master: no mask; no _alb texture: the Base...Map one
               {{"AlphaTranslucentOcclusionCavityMap", "ch/hair_ATOC.tex"}, {"BaseMetalMap", "ch/cloth_MSK.tex"}},
               "_Chainsaw/MasterMaterial/Character_Detail"},
              {"Decal_Mat", {{"BaseAlphaMap", "ch/decal_ALBA.tex"}}, "_Chainsaw/Decal_Decal"},
              {"Stitch_Mat", {{"Stitch_NAM", "ch/stitch_MSK4.tex"}}},
              {"L_tearline_mat", {{"BaseMap", "ch/hair_ALBD.tex"}}},
              {"EyeWet_mat", {}}}));
    const std::vector<std::string> textures{"ch/cloth_msk.tex.1", "ch/decal_alba.tex.1", "ch/hair_albd.tex.1",
                                            "ch/hair_atoc.tex.1", "ch/stitch_msk4.tex.1"};
    const auto m = remod::mesh_textures(dir.path, "ch/hair.mesh.221108797", textures);
    REQUIRE(m.materials.size() == 6);
    const auto& hair = m.materials[0];
    CHECK(m.textures[size_t(hair.albedo)] == "ch/hair_albd.tex.1");
    CHECK(hair.base_color == std::array<float, 4>{0.5f, 0.25f, 0.125f, 1});
    REQUIRE(hair.opacity.texture >= 0);
    CHECK(m.textures[size_t(hair.opacity.texture)] == "ch/hair_atoc.tex.1");
    CHECK(hair.opacity.channel == 0);  // red
    const auto& cloth = m.materials[1];
    REQUIRE(cloth.albedo >= 0);
    CHECK(m.textures[size_t(cloth.albedo)] == "ch/cloth_msk.tex.1");
    CHECK(cloth.opacity.texture == -1);
    CHECK(cloth.base_color == std::array<float, 4>{1, 1, 1, 1});
    const auto& decal = m.materials[2];
    CHECK((decal.opacity.texture == decal.albedo && decal.opacity.channel == 3));  // the colour texture's alpha
    CHECK(m.materials[3].albedo == -1);
    CHECK((!m.materials[3].hidden && m.materials[4].hidden && m.materials[5].hidden));
}

TEST_CASE("preview_file prefers the streaming copy") {
    TempDir dir;
    test::write_file(dir.path / "a/x.tex.1", "x");
    test::write_file(dir.path / "a/y.tex.1", "x");
    test::write_file(dir.path / "streaming/a/y.tex.1", "x");
    CHECK(remod::preview_file(dir.path, "a/x.tex.1") == dir.path / "a/x.tex.1");
    CHECK(remod::preview_file(dir.path, "a/y.tex.1") == dir.path / "streaming/a/y.tex.1");
}

TEST_CASE("tidy_layout: columns by step order, blocks level with what feeds them") {
    remod::Graph g;
    for (const auto& [id, type, x, y] : std::vector<std::tuple<int, const char*, float, float>>{
             {1, "LoadTex", 500.0f, 300.0f}, {2, "ExportImage", 40.0f, 900.0f}, {3, "EditImage", 0.0f, 0.0f},
             {4, "SaveTex", 900.0f, 20.0f}, {5, "PackageMod", 100.0f, 100.0f}, {6, "Text", 300.0f, 50.0f}})
        g.nodes.push_back({id, type, {}, x, y});
    g.links = {{1, "tex", 2, "tex"}, {2, "png", 3, "png"}, {3, "image", 4, "image"}, {1, "tex", 4, "original"},
               {4, "tex", 5, "tex"}, {6, "text", 5, "name"}};
    const std::vector<std::array<float, 2>> sizes{{300, 200}, {300, 150}, {250, 120}, {300, 180}, {320, 400}, {200, 80}};
    const auto at = remod::tidy_layout(g, sizes, 100, 40);
    REQUIRE(at.size() == 6);
    // Columns: LoadTex and Text first; Export, Edit, SaveTex, Package each one further right.
    CHECK(at[0][0] == 0);
    CHECK(at[5][0] == 0);
    CHECK(at[1][0] == 300 + 100);
    CHECK(at[2][0] == at[1][0] + 300 + 100);
    CHECK(at[3][0] == at[2][0] + 250 + 100);
    CHECK(at[4][0] == at[3][0] + 300 + 100);
    // Starts at the old top-left corner; Text (above LoadTex before) stays above it, with the gap between them.
    CHECK(at[5][1] == 0);
    CHECK(at[0][1] == 80 + 40);
    // Level with what feeds them: Export with LoadTex; SaveTex between Edit and LoadTex.
    CHECK(at[1][1] == at[0][1]);
    CHECK(at[3][1] == (at[2][1] + at[0][1]) / 2);
    // A cycle (not allowed by can_connect, but a file could hold one) doesn't hang.
    g.links.push_back({5, "tex", 1, "tex"});
    CHECK(remod::tidy_layout(g, sizes, 100, 40).size() == 6);
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


TEST_CASE("file_kind and list_folder: any folder, folders first, sorted ignoring case") {
    using remod::FileKind;
    CHECK(remod::file_kind("cha000_albd.tex.143221013") == FileKind::Texture);
    CHECK(remod::file_kind("plain.TEX") == FileKind::Texture);
    CHECK(remod::file_kind("cha000.mesh.221108797") == FileKind::Mesh);
    CHECK(remod::file_kind("shot.PNG") == FileKind::Image);
    CHECK(remod::file_kind("notes.txt") == FileKind::Other);
    CHECK(remod::file_kind("a.tex.png") == FileKind::Image);

    TempDir tmp;
    fs::create_directories(tmp.path / "zeta");
    fs::create_directories(tmp.path / "Alpha");
    test::write_file(tmp.path / "b.png", "x");
    test::write_file(tmp.path / "A.tex.143221013", "x");
    std::string error;
    const auto entries = remod::list_folder(tmp.path, &error);
    CHECK(error.empty());
    REQUIRE(entries.size() == 4);
    CHECK(entries[0].name == "Alpha");
    CHECK(entries[0].kind == FileKind::Folder);
    CHECK(entries[1].name == "zeta");
    CHECK(entries[2].name == "A.tex.143221013");
    CHECK(entries[2].kind == FileKind::Texture);
    CHECK(entries[3].kind == FileKind::Image);

    CHECK(remod::list_folder(tmp.path / "missing", &error).empty());
    CHECK_FALSE(error.empty());
    CHECK_FALSE(remod::drive_roots().empty());
}
