#include "route.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>

namespace remod {

namespace {

constexpr float kSame = 0.5f;  // coordinates closer than this are one grid line

// Sorted grid lines, near-duplicates merged, plus a spare lane halfway between each neighbouring pair.
std::vector<float> lines(std::vector<float> v) {
    std::ranges::sort(v);
    std::vector<float> out;
    for (float x : v)
        if (out.empty() || x - out.back() > kSame) out.push_back(x);
    const size_t n = out.size();
    for (size_t i = 0; i + 1 < n; ++i) out.push_back((out[i] + out[i + 1]) * 0.5f);
    std::ranges::sort(out);
    return out;
}

int find_line(const std::vector<float>& v, float x) {
    const auto it = std::ranges::lower_bound(v, x - kSame);
    return it != v.end() && std::abs(*it - x) <= kSame ? int(it - v.begin()) : -1;
}

// Drops points that repeat or lie on a straight run.
std::vector<Pt> simplify(const std::vector<Pt>& in) {
    std::vector<Pt> out;
    for (const Pt& p : in) {
        if (!out.empty() && std::abs(out.back().x - p.x) < 0.01f && std::abs(out.back().y - p.y) < 0.01f) continue;
        if (out.size() >= 2) {
            const Pt& a = out[out.size() - 2];
            const Pt& b = out.back();
            if ((std::abs(a.x - b.x) < 0.01f && std::abs(b.x - p.x) < 0.01f) ||
                (std::abs(a.y - b.y) < 0.01f && std::abs(b.y - p.y) < 0.01f))
                out.pop_back();
        }
        out.push_back(p);
    }
    return out;
}

}  // namespace

Routes route_links(const std::vector<Box>& blocks, const std::vector<LinkRoute>& links, float gap) {
    // A sparse grid: lines just outside every block and through every pin's stub, plus spare lanes between them.
    std::vector<float> xv, yv;
    for (const Box& b : blocks) {
        xv.insert(xv.end(), {b.x0 - gap, b.x1 + gap});
        yv.insert(yv.end(), {b.y0 - gap, b.y1 + gap});
    }
    for (const LinkRoute& l : links) {
        xv.insert(xv.end(), {l.from.x + gap, l.to.x - gap});
        yv.insert(yv.end(), {l.from.y, l.to.y});
    }
    const std::vector<float> xs = lines(std::move(xv)), ys = lines(std::move(yv));
    const int nx = int(xs.size()), ny = int(ys.size()), n = nx * ny;
    auto at = [&](int ix, int iy) { return ix * ny + iy; };

    // Free points and edges: nothing inside a block grown by half the gap. right_ok[p]: p -> x+1; down_ok[p]: p -> y+1.
    const float h = gap * 0.5f;
    auto blocked = [&](float xa, float xb, float ya, float yb) {  // segment (or point) touches a grown block's inside
        for (const Box& b : blocks)
            if (xb > b.x0 - h && xa < b.x1 + h && yb > b.y0 - h && ya < b.y1 + h) return true;
        return false;
    };
    std::vector<char> right_ok(n), down_ok(n);
    for (int ix = 0; ix < nx; ++ix)
        for (int iy = 0; iy < ny; ++iy) {
            if (ix + 1 < nx) right_ok[at(ix, iy)] = !blocked(xs[ix], xs[ix + 1], ys[iy], ys[iy]);
            if (iy + 1 < ny) down_ok[at(ix, iy)] = !blocked(xs[ix], xs[ix], ys[iy], ys[iy + 1]);
        }
    // Which net first used each edge, to share trunks and keep nets apart.
    std::vector<int> right_net(n, -1), down_net(n, -1);

    // Directions: 0 right (+x), 1 left, 2 down (+y), 3 up. d ^ 1 is the opposite direction.
    // Returns the neighbour of p in direction d (or -1), with that edge's owner slot.
    auto step = [&](int p, int d, int** owner) -> int {
        const int ix = p / ny, iy = p % ny;
        switch (d) {
        case 0: *owner = &right_net[p]; return right_ok[p] ? at(ix + 1, iy) : -1;
        case 1: if (ix == 0 || !right_ok[at(ix - 1, iy)]) return -1; *owner = &right_net[at(ix - 1, iy)]; return at(ix - 1, iy);
        case 2: *owner = &down_net[p]; return down_ok[p] ? at(ix, iy + 1) : -1;
        default: if (iy == 0 || !down_ok[at(ix, iy - 1)]) return -1; *owner = &down_net[at(ix, iy - 1)]; return at(ix, iy - 1);
        }
    };
    auto point = [&](int p) { return Pt{xs[p / ny], ys[p % ny]}; };
    const float bend = 2 * gap;
    constexpr float kInf = std::numeric_limits<float>::infinity();

    Routes out;
    std::vector<char> net_seen;
    for (const LinkRoute& l : links) {
        const int sx = find_line(xs, l.from.x + gap), sy = find_line(ys, l.from.y);
        const int ex = find_line(xs, l.to.x - gap), ey = find_line(ys, l.to.y);
        const bool on_grid = sx >= 0 && sy >= 0 && ex >= 0 && ey >= 0;  // always, as the pins made those lines
        const int start = on_grid ? at(sx, sy) : 0, goal = on_grid ? at(ex, ey) : 0;
        const Pt s = point(start), e = point(goal);

        // Dijkstra over (point, direction of arrival); the start counts as arriving rightwards.
        std::vector<float> dist(size_t(n) * 4, kInf);
        std::vector<int> prev(size_t(n) * 4, -1);
        using Item = std::pair<float, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
        if (on_grid && !blocked(s.x, s.x, s.y, s.y) && !blocked(e.x, e.x, e.y, e.y)) {
            dist[size_t(start) * 4] = 0;
            open.push({0.0f, start * 4});
        }
        int best = -1;
        float best_cost = kInf;
        while (!open.empty()) {
            const auto [cost, state] = open.top();
            open.pop();
            if (cost > dist[state] || cost >= best_cost) continue;
            const int p = state / 4, d = state % 4;
            if (p == goal && d != 1) {  // arriving leftwards would need a U-turn into the pin
                const float total = cost + (d == 0 ? 0 : bend);
                if (total < best_cost) best_cost = total, best = state;
            }
            for (int nd = 0; nd < 4; ++nd) {
                if (nd == (d ^ 1)) continue;  // no U-turns
                int* owner = nullptr;
                const int q = step(p, nd, &owner);
                if (q < 0) continue;
                const Pt a = point(p), b = point(q);
                const float len = std::abs(a.x - b.x) + std::abs(a.y - b.y);
                const bool shared = *owner == l.net, foreign = *owner >= 0 && !shared;
                const float c = cost + (shared ? 0 : len * (foreign ? 10.0f : 1.0f)) + (nd != d && !shared ? bend : 0);
                const int next = q * 4 + nd;
                if (c < dist[next]) {
                    dist[next] = c;
                    prev[next] = state;
                    open.push({c, next});
                }
            }
        }

        const bool trunk_exists = l.net >= 0 && size_t(l.net) < net_seen.size() && net_seen[l.net];
        if (best < 0) {  // no way around: a plain right-angle path, drawn over whatever is in the way
            const float mx = (l.from.x + l.to.x) * 0.5f;
            out.paths.push_back(simplify({l.from, {mx, l.from.y}, {mx, l.to.y}, l.to}));
        } else {
            std::vector<int> grid;  // grid points from start to goal
            for (int st = best; st >= 0; st = prev[st]) grid.push_back(st / 4);
            std::ranges::reverse(grid);
            std::vector<Pt> path{l.from};
            bool on_trunk = trunk_exists;
            for (size_t i = 0; i < grid.size(); ++i) {
                path.push_back(point(grid[i]));
                if (i + 1 == grid.size()) break;
                int* owner = nullptr;
                for (int d = 0; d < 4; ++d)
                    if (step(grid[i], d, &owner) == grid[i + 1]) break;
                if (on_trunk && *owner != l.net) {  // the branch leaves the trunk here
                    out.junctions.push_back(point(grid[i]));
                    on_trunk = false;
                }
                if (*owner < 0) *owner = l.net;
            }
            path.push_back(l.to);
            out.paths.push_back(simplify(path));
        }
        if (l.net >= 0) {
            if (size_t(l.net) >= net_seen.size()) net_seen.resize(size_t(l.net) + 1);
            net_seen[l.net] = 1;
        }
    }
    return out;
}

int hit_link(const std::vector<std::vector<Pt>>& paths, Pt p, float tolerance) {
    for (size_t i = 0; i < paths.size(); ++i)
        for (size_t k = 0; k + 1 < paths[i].size(); ++k) {
            const Pt a = paths[i][k], b = paths[i][k + 1];
            const float x = std::clamp(p.x, std::min(a.x, b.x), std::max(a.x, b.x));
            const float y = std::clamp(p.y, std::min(a.y, b.y), std::max(a.y, b.y));
            if (std::hypot(p.x - x, p.y - y) <= tolerance) return int(i);
        }
    return -1;
}

}  // namespace remod
