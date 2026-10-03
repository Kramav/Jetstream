#include "mesh.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>

namespace remod {

namespace {

struct Corner {
    long v = 0, t = 0, n = 0;  // 1-based as in the file, negative = from the end, 0 = none
};

}  // namespace

// ponytail: OBJ text, because it's trivial to read; a 163k-triangle character is 20 MB. Ask Noesis for .glb
// if reading big meshes gets slow.
MeshModel parse_obj(const std::string& text) {
    std::vector<std::array<float, 3>> pos, nrm;
    std::vector<std::array<float, 2>> uv;
    MeshModel model;
    std::map<std::pair<int, std::string>, size_t> parts;  // (group, material) -> part
    int group = -1;
    std::string material;
    size_t part = SIZE_MAX;  // looked up at the next face after g or usemtl
    model.min = {INFINITY, INFINITY, INFINITY};
    model.max = {-INFINITY, -INFINITY, -INFINITY};

    auto resolve = [](long i, size_t count) {
        const long long at = i > 0 ? i - 1 : static_cast<long long>(count) + i;
        if (i == 0 || at < 0 || at >= static_cast<long long>(count)) throw std::runtime_error("OBJ face index out of range");
        return size_t(at);
    };
    std::vector<Corner> face;
    const char* p = text.c_str();
    const char* const end = p + text.size();
    while (p < end) {
        const char* eol = static_cast<const char*>(std::memchr(p, '\n', size_t(end - p)));
        if (!eol) eol = end;
        const char* q = p + 2;  // after "v " (one more for "vt" / "vn")
        if (p[0] == 'v' && (p[1] == ' ' || p[1] == '\t')) {
            std::array<float, 3> v{};
            for (float& f : v) f = std::strtof(q, const_cast<char**>(&q));
            pos.push_back(v);
        } else if (p[0] == 'v' && p[1] == 't') {
            ++q;
            std::array<float, 2> t{};
            for (float& f : t) f = std::strtof(q, const_cast<char**>(&q));
            uv.push_back(t);
        } else if (p[0] == 'v' && p[1] == 'n') {
            ++q;
            std::array<float, 3> n{};
            for (float& f : n) f = std::strtof(q, const_cast<char**>(&q));
            nrm.push_back(n);
        } else if (std::strncmp(p, "usemtl", 6) == 0) {
            material.assign(p + 6, eol);
            while (!material.empty() && std::isspace(static_cast<unsigned char>(material.back()))) material.pop_back();
            material.erase(0, material.find_first_not_of(" \t"));
            part = SIZE_MAX;
        } else if (p[0] == 'g' && std::isspace(static_cast<unsigned char>(p[1]))) {
            const std::string name(p, eol);
            const size_t at = name.find("_Group_");
            group = at == std::string::npos ? -1 : std::atoi(name.c_str() + at + 7);
            part = SIZE_MAX;
        } else if (p[0] == 'f' && (p[1] == ' ' || p[1] == '\t')) {
            face.clear();
            q = p + 1;
            while (true) {
                while (q < eol && (*q == ' ' || *q == '\t' || *q == '\r')) ++q;
                if (q >= eol) break;
                Corner c;
                c.v = std::strtol(q, const_cast<char**>(&q), 10);
                if (*q == '/') {
                    ++q;
                    if (*q != '/') c.t = std::strtol(q, const_cast<char**>(&q), 10);
                    if (*q == '/') c.n = std::strtol(q + 1, const_cast<char**>(&q), 10);
                }
                face.push_back(c);
                while (q < eol && *q != ' ' && *q != '\t' && *q != '\r') ++q;  // anything unexpected
            }
            if (part == SIZE_MAX) {
                const auto [it, added] = parts.try_emplace({group, material}, model.parts.size());
                if (added) model.parts.push_back({material, group, {}});
                part = it->second;
            }
            std::vector<float>& out = model.parts[part].vertices;
            for (size_t k = 1; k + 1 < face.size(); ++k) {
                const Corner tri[3] = {face[0], face[k], face[k + 1]};
                std::array<float, 3> p3[3];
                for (int i = 0; i < 3; ++i) p3[i] = pos[resolve(tri[i].v, pos.size())];
                // The face's normal, for corners without one.
                const float ax = p3[1][0] - p3[0][0], ay = p3[1][1] - p3[0][1], az = p3[1][2] - p3[0][2];
                const float bx = p3[2][0] - p3[0][0], by = p3[2][1] - p3[0][1], bz = p3[2][2] - p3[0][2];
                std::array<float, 3> fn{ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx};
                const float len = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
                if (len > 0)
                    for (float& f : fn) f /= len;
                for (int i = 0; i < 3; ++i) {
                    const auto n = tri[i].n ? nrm[resolve(tri[i].n, nrm.size())] : fn;
                    const auto t = tri[i].t ? uv[resolve(tri[i].t, uv.size())] : std::array<float, 2>{};
                    out.insert(out.end(), {p3[i][0], p3[i][1], p3[i][2], n[0], n[1], n[2], t[0], t[1]});
                    for (int a = 0; a < 3; ++a) {
                        model.min[a] = std::min(model.min[a], p3[i][a]);
                        model.max[a] = std::max(model.max[a], p3[i][a]);
                    }
                }
                ++model.triangles;
            }
        }
        p = eol + 1;
    }
    std::erase_if(model.parts, [](const MeshPart& m) { return m.vertices.empty(); });
    if (model.triangles == 0) throw std::runtime_error("the mesh has no triangles");
    return model;
}

// ---- Native .mesh reading (user, 2026-10-02: no Noesis needed for the 3D view) ----

namespace {

// Little-endian reads with bounds checks: a damaged file throws instead of reading past the end.
struct Bytes {
    const std::string& b;
    const std::filesystem::path& file;
    template <class T>
    T at(std::uint64_t offset) const {
        if (offset > b.size() || sizeof(T) > b.size() - offset)
            throw std::runtime_error(file.string() + " is cut short or damaged");
        T v;
        std::memcpy(&v, b.data() + offset, sizeof(T));
        return v;
    }
    std::string text(std::uint64_t offset) const {  // zero-terminated ASCII
        std::string s;
        for (char c; (c = at<char>(offset)) != 0; ++offset) s += c;
        return s;
    }
};

float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = std::uint32_t(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    std::uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {  // subnormal: normalise
            int e = -1;
            std::uint32_t m = mant;
            do {
                ++e;
                m <<= 1;
            } while (!(m & 0x400));
            bits = sign | std::uint32_t(127 - 15 - e) << 23 | (m & 0x3FF) << 13;
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000 | mant << 13;
    } else {
        bits = sign | (exp + 127 - 15) << 23 | mant << 13;
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

}  // namespace

MeshModel read_mesh(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("can't open " + file.string());
    const std::string data((std::istreambuf_iterator<char>(in)), {});
    const Bytes b{data, file};
    // [REE-Lib MeshFile.cs, MIT] Header, RE4 layout (internal version 220822879, RE4R's .mesh.221108797): name count
    // @20, offsets (u64) of the LODs @32, the vertex buffer header @72, material name indices @112, name offsets
    // @144.
    if (b.at<std::uint32_t>(0) != 0x4853454D) throw std::runtime_error(file.string() + " is not an RE Engine mesh");
    if (const auto version = b.at<std::uint32_t>(4); version != 220822879)
        throw std::runtime_error("mesh version " + std::to_string(version) +
                                 " isn't read yet (RE4R's is); set Noesis64.exe in the Pipeline panel for it");
    const auto name_count = b.at<std::uint16_t>(20);
    const auto lods_at = b.at<std::uint64_t>(32), buffer_at = b.at<std::uint64_t>(72),
               materials_at = b.at<std::uint64_t>(112), names_at = b.at<std::uint64_t>(144);
    if (!lods_at || !buffer_at) throw std::runtime_error(file.string() + " holds no mesh (only shadow or collision?)");

    // The vertex buffer: element headers {i16 type, i16 size, i32 offset}, then positions (3 floats), normals (4
    // signed bytes, then the tangent's 4), UVs (2 halfs) at their offsets from the buffer's start, then the indices.
    const auto elements_at = b.at<std::uint64_t>(buffer_at), vertices_at = b.at<std::uint64_t>(buffer_at + 8);
    const auto total_size = b.at<std::uint32_t>(buffer_at + 24), vertices_size = b.at<std::uint32_t>(buffer_at + 28);
    const auto element_count = b.at<std::uint16_t>(buffer_at + 32);
    struct Element {
        std::int16_t type, size;
        std::int32_t offset;
    };
    std::map<int, Element> elements;  // by type: 0 position, 1 normal and tangent, 2 UV
    for (std::uint64_t i = 0; i < element_count; ++i) {
        const Element e{b.at<std::int16_t>(elements_at + 8 * i), b.at<std::int16_t>(elements_at + 8 * i + 2),
                        b.at<std::int32_t>(elements_at + 8 * i + 4)};
        elements.try_emplace(e.type, e);
    }
    if (element_count < 2 || !elements.contains(0)) throw std::runtime_error(file.string() + ": no vertex positions");
    const Element positions = elements.at(0);
    const auto second = b.at<std::int32_t>(elements_at + 8 + 4);  // the next element starts where positions end
    const std::uint64_t vertex_count =
        positions.size > 0 && second > positions.offset ? std::uint64_t(second - positions.offset) / positions.size : 0;

    // Mesh data: LOD count @0, material count @1, integer indices @6, then the LOD offsets' offset @56 (after a
    // bounding sphere and box).
    const auto lod_count = b.at<std::uint8_t>(lods_at), material_count = b.at<std::uint8_t>(lods_at + 1);
    const bool integer_indices = b.at<std::uint8_t>(lods_at + 6) != 0;
    const auto lod_list = b.at<std::uint64_t>(lods_at + 56);
    const std::uint64_t index_size = integer_indices ? 4 : 2;
    const std::uint64_t indices_at = vertices_at + vertices_size,
                        index_count = total_size > vertices_size ? (std::uint64_t(total_size) - vertices_size) / index_size : 0;

    std::vector<std::string> names(name_count), materials(material_count);
    for (std::uint64_t i = 0; i < name_count && names_at; ++i) names[i] = b.text(b.at<std::uint64_t>(names_at + 8 * i));
    for (std::uint64_t i = 0; i < material_count && materials_at; ++i) {
        const auto index = b.at<std::uint16_t>(materials_at + 2 * i);
        materials[i] = index < names.size() ? names[index] : "material" + std::to_string(i);
    }

    // The most detailed LOD whose triangles are all in this file (a streamed mesh keeps the best ones in its
    // streaming/ copy). Group: {u8 id, u8 submesh count, 6 empty, i32 vertices, i32 indices}, then per submesh
    // {u16 material, u8, u8, i32 index count, i32 first index, i32 first vertex, i32, i32}.
    struct Sub {
        int group;
        std::uint16_t material;
        std::uint64_t count, first_index, first_vertex;
    };
    std::vector<Sub> subs;
    for (std::uint64_t lod = 0; lod < lod_count && subs.empty(); ++lod) {
        const auto lod_at = b.at<std::uint64_t>(lod_list + 8 * lod);
        const auto group_count = b.at<std::uint8_t>(lod_at);
        const auto groups_at = b.at<std::uint64_t>(lod_at + 8);
        std::vector<Sub> found;
        bool fits = true;
        for (std::uint64_t g = 0; g < group_count; ++g) {
            const auto group_at = b.at<std::uint64_t>(groups_at + 8 * g);
            const int id = b.at<std::uint8_t>(group_at);
            const auto sub_count = b.at<std::uint8_t>(group_at + 1);
            for (std::uint64_t k = 0; k < sub_count; ++k) {
                const std::uint64_t at = group_at + 16 + 24 * k;
                const Sub sub{id, b.at<std::uint16_t>(at), b.at<std::uint32_t>(at + 4), b.at<std::uint32_t>(at + 8),
                              b.at<std::uint32_t>(at + 12)};
                fits = fits && sub.first_index + sub.count <= index_count && sub.first_vertex < vertex_count;
                found.push_back(sub);
            }
        }
        if (fits) subs = std::move(found);
    }
    if (subs.empty())
        throw std::runtime_error(file.string() + ": its geometry is in its streaming/ copy, which isn't read yet");

    MeshModel model;
    model.min = {FLT_MAX, FLT_MAX, FLT_MAX};
    model.max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    std::map<std::pair<int, std::string>, size_t> parts;  // (group, material) -> part
    const auto normals = elements.find(1), uvs = elements.find(2);
    for (const Sub& sub : subs) {
        const std::string material = sub.material < materials.size() ? materials[sub.material] : "";
        const auto [it, added] = parts.try_emplace({sub.group, material}, model.parts.size());
        if (added) model.parts.push_back({material, sub.group, {}});
        std::vector<float>& out = model.parts[it->second].vertices;
        for (std::uint64_t i = 0; i + 2 < sub.count; i += 3) {
            for (std::uint64_t c = 0; c < 3; ++c) {
                const std::uint64_t at = indices_at + (sub.first_index + i + c) * index_size;
                const std::uint64_t v =
                    sub.first_vertex + (integer_indices ? b.at<std::uint32_t>(at) : b.at<std::uint16_t>(at));
                if (v >= vertex_count) throw std::runtime_error(file.string() + ": a triangle points past the vertices");
                const std::uint64_t p = vertices_at + std::uint64_t(positions.offset) + v * 12;
                const float x = b.at<float>(p), y = b.at<float>(p + 4), z = b.at<float>(p + 8);
                float nx = 0, ny = 1, nz = 0, u = 0, w = 0;
                if (normals != elements.end()) {  // a signed byte n stands for (2n + 1) / 255
                    const std::uint64_t q = vertices_at + std::uint64_t(normals->second.offset) + v * normals->second.size;
                    nx = (2 * b.at<std::int8_t>(q) + 1) / 255.0f;
                    ny = (2 * b.at<std::int8_t>(q + 1) + 1) / 255.0f;
                    nz = (2 * b.at<std::int8_t>(q + 2) + 1) / 255.0f;
                    if (const float len = std::sqrt(nx * nx + ny * ny + nz * nz); len > 0) nx /= len, ny /= len, nz /= len;
                }
                if (uvs != elements.end()) {
                    const std::uint64_t q = vertices_at + std::uint64_t(uvs->second.offset) + v * uvs->second.size;
                    u = half_to_float(b.at<std::uint16_t>(q));
                    w = half_to_float(b.at<std::uint16_t>(q + 2));
                }
                out.insert(out.end(), {x, y, z, nx, ny, nz, u, w});
                const float pos[] = {x, y, z};
                for (size_t a = 0; a < 3; ++a) {
                    model.min[a] = std::min(model.min[a], pos[a]);
                    model.max[a] = std::max(model.max[a], pos[a]);
                }
            }
            ++model.triangles;
        }
    }
    std::erase_if(model.parts, [](const MeshPart& m) { return m.vertices.empty(); });
    if (model.triangles == 0) throw std::runtime_error(file.string() + " has no triangles");
    return model;
}

std::vector<std::uint8_t> uv_mask(const std::vector<const MeshPart*>& parts, unsigned width, unsigned height,
                                  unsigned grow) {
    std::vector<std::uint8_t> on(size_t(width) * height, 0);
    const auto wrap = [](long v, unsigned n) { return size_t(((v % long(n)) + long(n)) % long(n)); };
    for (const MeshPart* part : parts) {
        const std::vector<float>& v = part->vertices;
        for (size_t t = 0; t + 3 * MeshPart::kStride <= v.size(); t += 3 * MeshPart::kStride) {
            // The triangle in pixels, moved by whole tiles so it sits near the first one (it stays in one piece).
            float x[3], y[3];
            float cu = 0, cv = 0;
            for (size_t c = 0; c < 3; ++c) {
                cu += v[t + c * MeshPart::kStride + 6] / 3;
                cv += v[t + c * MeshPart::kStride + 7] / 3;
            }
            const float tu = std::floor(cu), tv = std::floor(cv);
            for (size_t c = 0; c < 3; ++c) {
                x[c] = (v[t + c * MeshPart::kStride + 6] - tu) * float(width);
                y[c] = (v[t + c * MeshPart::kStride + 7] - tv) * float(height);  // v = 0 at the top, as stored
            }
            const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
            const long x0 = long(std::floor(std::min({x[0], x[1], x[2]}))), x1 = long(std::ceil(std::max({x[0], x[1], x[2]})));
            const long y0 = long(std::floor(std::min({y[0], y[1], y[2]}))), y1 = long(std::ceil(std::max({y[0], y[1], y[2]})));
            if (x1 - x0 > long(width) * 4 || y1 - y0 > long(height) * 4) continue;  // broken UVs: skip, don't stall
            for (long py = y0; py <= y1; ++py)
                for (long px = x0; px <= x1; ++px) {
                    const float sx = float(px) + 0.5f, sy = float(py) + 0.5f;
                    float e[3];
                    for (int k = 0; k < 3; ++k) {  // each edge's side, by the triangle's own winding
                        const int a = k, b = (k + 1) % 3;
                        e[k] = ((x[b] - x[a]) * (sy - y[a]) - (y[b] - y[a]) * (sx - x[a])) * (area < 0 ? -1.0f : 1.0f);
                    }
                    if (area != 0 && e[0] >= 0 && e[1] >= 0 && e[2] >= 0) on[wrap(py, height) * width + wrap(px, width)] = 1;
                }
        }
    }
    for (unsigned g = 0; g < grow; ++g) {  // one pixel out each time, to the 8 neighbours
        std::vector<std::uint8_t> next = on;
        for (unsigned py = 0; py < height; ++py)
            for (unsigned px = 0; px < width; ++px)
                if (on[size_t(py) * width + px])
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const long nx = long(px) + dx, ny = long(py) + dy;
                            if (nx >= 0 && ny >= 0 && nx < long(width) && ny < long(height)) next[size_t(ny) * width + size_t(nx)] = 1;
                        }
        on = std::move(next);
    }
    std::vector<std::uint8_t> bgra(on.size() * 4);
    for (size_t i = 0; i < on.size(); ++i) {
        bgra[i * 4] = bgra[i * 4 + 1] = bgra[i * 4 + 2] = on[i] ? 255 : 0;
        bgra[i * 4 + 3] = 255;
    }
    return bgra;
}

}  // namespace remod
