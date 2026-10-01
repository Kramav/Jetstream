#include "mesh.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

}  // namespace remod
