#include "mesh.hpp"

#include <catch2/catch_test_macros.hpp>

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
