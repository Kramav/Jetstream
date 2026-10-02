#pragma once
// A mesh's shape for the browser's 3D view (CLAUDE.md §10). Noesis exports the game's .mesh to OBJ
// (NoesisConverter::load_mesh); this reads that OBJ. Textures are applied by the viewer from the material
// (mesh_textures), not from the OBJ.
#include <array>
#include <string>
#include <vector>

namespace remod {

struct MeshPart {
    std::string material;          // the OBJ's usemtl name = the .mdf2 material name
    // The RE Engine mesh group: the plugin names OBJ groups "LOD_1_Group_<id>_Sub_..." (seen in its OBJ). The game
    // shows or hides whole groups (e.g. Leon's group 2 is his bare forearms); that choice isn't in the mesh.
    int group = -1;                // -1: no "_Group_<id>_" in the name
    // Triangles, not indexed. Per vertex: x y z, nx ny nz, u v (v = 0 at the top, as D3D samples). Noesis's OBJ
    // already writes v that way, unlike plain OBJ: flipping it put Leon's shirt on the skin above it in his texture
    // (2026-10-01; 1,470 of 12,777 shirt triangles flipped, 0 as written).
    std::vector<float> vertices;
    static constexpr size_t kStride = 8;
};

struct MeshModel {
    std::vector<MeshPart> parts;
    std::array<float, 3> min{}, max{};  // bounds of every position
    size_t triangles = 0;
};

// Reads OBJ text: v, vt, vn, f (any polygon, fanned into triangles; negative indexes), g, usemtl; one part per
// (group, material). Faces without
// normals get the face's normal. Throws std::runtime_error on a bad face index or no faces at all.
MeshModel parse_obj(const std::string& text);

}  // namespace remod
