#include "mesh.hpp"
#include "texture_converter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>

namespace {

std::string env_var(const char* name) {
    char* v = nullptr;
    size_t n = 0;
    _dupenv_s(&v, &n, name);
    std::string s = v ? v : "";
    std::free(v);
    return s;
}

// How well vertex normals agree with their triangles' own (mean cosine): the same for two readers if they decode
// the normals alike, whatever small rounding each uses.
double normal_agreement(const remod::MeshModel& m) {
    double sum = 0;
    size_t n = 0;
    for (const auto& p : m.parts)
        for (size_t i = 0; i + 3 * remod::MeshPart::kStride <= p.vertices.size(); i += 3 * remod::MeshPart::kStride) {
            const float* v = &p.vertices[i];
            const float ax = v[8] - v[0], ay = v[9] - v[1], az = v[10] - v[2];
            const float bx = v[16] - v[0], by = v[17] - v[1], bz = v[18] - v[2];
            const float fx = ay * bz - az * by, fy = az * bx - ax * bz, fz = ax * by - ay * bx;
            const float len = std::sqrt(fx * fx + fy * fy + fz * fz);
            if (len <= 0) continue;
            for (int c = 0; c < 3; ++c, ++n) {
                const float* q = v + c * remod::MeshPart::kStride + 3;
                sum += (fx * q[0] + fy * q[1] + fz * q[2]) / len;
            }
        }
    return n ? sum / double(n) : 0;
}

}  // namespace

TEST_CASE("parse_obj: parts per material, polygons fanned, v kept as Noesis writes it") {
    const auto m = remod::parse_obj(
        "# Noesis .obj export.\n"
        "mtllib unused.mtl\n"
        "v  0 0 0\nv  1 0 0\nv  1 1 0\nv  0 1 -2\r\n"
        "vt  0.25 0.75 0\nvn 0 0 1\n"
        "g a\nusemtl Skin_Mat\n"
        "f  1/1/1 2/1/1 3/1/1\n"
        "usemtl Cloth_Mat\n"
        "f 1 2 3 4\n"                  // a quad, no uv or normal: two triangles with the face normal
        "usemtl Skin_Mat\n"
        "f -4//1 -3//1 -2//1\n");      // negative indexes, back into the first part
    REQUIRE(m.parts.size() == 2);
    CHECK(m.parts[0].material == "Skin_Mat");
    CHECK(m.parts[0].vertices.size() == 2 * 3 * remod::MeshPart::kStride);
    CHECK(m.parts[1].material == "Cloth_Mat");
    CHECK(m.parts[1].vertices.size() == 2 * 3 * remod::MeshPart::kStride);
    CHECK(m.triangles == 4);
    const float* v = m.parts[0].vertices.data();
    CHECK((v[6] == 0.25f && v[7] == 0.75f));  // u, v unflipped
    const float* q = m.parts[1].vertices.data();
    CHECK((q[3] == 0 && q[4] == 0 && q[5] == 1));  // face normal of the quad's first triangle
    CHECK((m.min == std::array<float, 3>{0, 0, -2} && m.max == std::array<float, 3>{1, 1, 0}));
}

TEST_CASE("parse_obj: the plugin's mesh groups stay apart") {
    const auto m = remod::parse_obj(
        "v 0 0 0\nv 1 0 0\nv 1 1 0\n"
        "g LOD_1_Group_0_Sub_5__Skin_Mat\nusemtl Skin_Mat\nf 1 2 3\n"
        "g LOD_1_Group_2_Sub_1__Skin_Mat\nusemtl Skin_Mat\nf 1 2 3\n"
        "g LOD_1_Group_0_Sub_1__Skin_Mat\nusemtl Skin_Mat\nf 1 2 3\n");
    REQUIRE(m.parts.size() == 2);
    CHECK((m.parts[0].group == 0 && m.parts[0].vertices.size() == 2 * 3 * remod::MeshPart::kStride));
    CHECK((m.parts[1].group == 2 && m.parts[1].material == "Skin_Mat"));
}

TEST_CASE("parse_obj refuses broken input") {
    CHECK_THROWS(remod::parse_obj("v 0 0 0\nf 1 2 3\n"));  // index out of range
    CHECK_THROWS(remod::parse_obj("v 0 0 0\n"));          // no faces
}

// CLAUDE.md §8: real meshes are local only (REMOD_FIXTURES, *.mesh.221108797). With REMOD_NOESIS set too, each is
// compared with what Noesis's RE plugin makes of it: the same triangles, bounds and (group, material) parts.
TEST_CASE("read_mesh reads RE4R meshes as Noesis does") {
    const std::string fixtures = env_var("REMOD_FIXTURES"), noesis = env_var("REMOD_NOESIS");
    if (fixtures.empty()) SKIP("set REMOD_FIXTURES to run");
    int count = 0;
    for (const auto& e : std::filesystem::directory_iterator(fixtures)) {
        if (!e.path().string().ends_with(".mesh.221108797")) continue;
        ++count;
        CAPTURE(e.path().filename().string());
        const remod::MeshModel mine = remod::read_mesh(e.path());
        CHECK(mine.triangles > 0);
        if (noesis.empty()) continue;
        const remod::MeshModel theirs = remod::NoesisConverter(noesis).load_mesh(e.path());
        CHECK(mine.triangles == theirs.triangles);
        for (size_t a = 0; a < 3; ++a) {  // Noesis writes centimetres; the game's units (and ours) are metres
            const float size = std::max(1e-3f, theirs.max[a] - theirs.min[a]);
            CHECK(std::abs(mine.min[a] * 100 - theirs.min[a]) < 1e-3f * size);
            CHECK(std::abs(mine.max[a] * 100 - theirs.max[a]) < 1e-3f * size);
        }
        // UVs summed over every corner (triangle order may differ): v as Noesis writes it. Normals by how they agree
        // with the geometry: REE-Lib decodes a byte n as (2n + 1) / 255, Noesis slightly differently.
        auto uv_sums = [](const remod::MeshModel& m) {
            std::array<double, 2> s{};
            for (const auto& p : m.parts)
                for (size_t i = 0; i < p.vertices.size(); i += remod::MeshPart::kStride)
                    for (size_t k = 0; k < 2; ++k) s[k] += p.vertices[i + 6 + k];
            return s;
        };
        const auto a = uv_sums(mine), t = uv_sums(theirs);
        for (size_t k = 0; k < 2; ++k) CHECK(std::abs(a[k] - t[k]) < 1e-2 * std::max(1.0, std::abs(t[k])));
        CHECK(std::abs(normal_agreement(mine) - normal_agreement(theirs)) < 0.01);
        auto keys = [](const remod::MeshModel& m) {
            std::set<std::pair<int, std::string>> k;
            for (const auto& p : m.parts) k.insert({p.group, p.material});
            return k;
        };
        CHECK(keys(mine) == keys(theirs));
    }
    REQUIRE(count > 0);
}



TEST_CASE("read_mesh says what it can't read") {
    const auto dir = std::filesystem::temp_directory_path() / "remod_mesh_test";
    std::filesystem::create_directories(dir);
    auto write = [&](const char* name, const std::string& bytes) {
        std::ofstream(dir / name, std::ios::binary) << bytes;
        return dir / name;
    };
    CHECK_THROWS_WITH(remod::read_mesh(write("text.mesh", "not a mesh at all, just some text here")),
                      Catch::Matchers::ContainsSubstring("not an RE Engine mesh"));
    std::string other(200, '\0');
    other.replace(0, 8, std::string("MESH\x01\x02\x03\x04", 8));  // another game's version
    CHECK_THROWS_WITH(remod::read_mesh(write("other.mesh", other)), Catch::Matchers::ContainsSubstring("isn't read yet"));
    CHECK_THROWS_WITH(remod::read_mesh(write("short.mesh", "MESH")), Catch::Matchers::ContainsSubstring("cut short"));
    std::filesystem::remove_all(dir);
}
