#include "route.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <utility>

namespace remod {

namespace {

constexpr float kClearance = 0.9f;  // how far lines keep from blocks, in gaps
constexpr char kClear = 1, kTight = 2;  // an edge is free at the full clearance, at half the gap
constexpr float kSame = 0.01f;  // coordinates closer than this are one grid line (pins keep their exact line)

// Sorted grid lines, near-duplicates merged, plus a spare lane halfway between neighbours at least `room` apart
// (a lane squeezed between closer ones would hug them).
std::vector<float> lines(std::vector<float> v, float room) {
    std::ranges::sort(v);
    std::vector<float> out;
    for (float x : v)
        if (out.empty() || x - out.back() > kSame) out.push_back(x);
    const size_t n = out.size();
    for (size_t i = 0; i + 1 < n; ++i)
        if (out[i + 1] - out[i] >= room) out.push_back((out[i] + out[i + 1]) * 0.5f);
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

Routes route_links(const std::vector<Box>& blocks, const std::vector<LinkRoute>& links, float gap, int passes) {
    // A sparse grid: two lanes around every block, lines through every pin's stub, and spare lanes between them.
    std::vector<float> xv, yv;
    for (const Box& b : blocks) {
        xv.insert(xv.end(), {b.x0 - gap, b.x1 + gap, b.x0 - 2 * gap, b.x1 + 2 * gap});
        yv.insert(yv.end(), {b.y0 - gap, b.y1 + gap, b.y0 - 2 * gap, b.y1 + 2 * gap});
    }
    for (const LinkRoute& l : links) {
        xv.insert(xv.end(), {l.from.x + gap, l.to.x - gap});
        yv.insert(yv.end(), {l.from.y, l.to.y});
    }
    const std::vector<float> xs = lines(std::move(xv), 2 * gap), ys = lines(std::move(yv), 2 * gap);
    const int nx = int(xs.size()), ny = int(ys.size()), n = nx * ny;
    auto at = [&](int ix, int iy) { return ix * ny + iy; };

    // Free points and edges: lines keep clear of every block by (almost) the gap, the length of a pin's stub; only
    // the stubs themselves touch a block. 0.9 leaves room for pins drawn a pixel inside a block's edge. Where blocks
    // stand closer than that (a pin's stub point too near the next block), a link may squeeze to half the gap.
    // right_ok[p]: p -> x+1; down_ok[p]: p -> y+1, with kClear and kTight flags.
    auto blocked = [&](float xa, float xb, float ya, float yb, float h) {  // touches a block grown by h
        for (const Box& b : blocks)
            if (xb > b.x0 - h && xa < b.x1 + h && yb > b.y0 - h && ya < b.y1 + h) return true;
        return false;
    };
    auto free_bits = [&](float xa, float xb, float ya, float yb) {
        return char((blocked(xa, xb, ya, yb, gap * kClearance) ? 0 : kClear) |
                    (blocked(xa, xb, ya, yb, gap * 0.5f) ? 0 : kTight));
    };
    std::vector<char> right_ok(n), down_ok(n);
    for (int ix = 0; ix < nx; ++ix)
        for (int iy = 0; iy < ny; ++iy) {
            if (ix + 1 < nx) right_ok[at(ix, iy)] = free_bits(xs[ix], xs[ix + 1], ys[iy], ys[iy]);
            if (iy + 1 < ny) down_ok[at(ix, iy)] = free_bits(xs[ix], xs[ix], ys[iy], ys[iy + 1]);
        }

    // What the other links use, to share trunks and keep nets apart: the net on each edge, and the nets that pass
    // through or turn at each point (-1 none, kMany more than one).
    constexpr int kMany = -2;
    std::vector<int> right_net(n), down_net(n), visit_net(n), corner_net(n);
    auto mark = [](int& slot, int net) { slot = slot == -1 || slot == net ? net : kMany; };
    auto other = [](int slot, int net) { return slot != -1 && slot != net; };
    // Parallel lines closer than this read as one line on screen: a lane right beside another net's line counts
    // almost as running along it. ponytail: compares every pair of lines; a sliding window if grids get big.
    const float sep = gap * 0.6f;
    auto near_lines = [&](const std::vector<float>& v) {
        std::vector<std::vector<int>> out(v.size());
        for (size_t i = 0; i < v.size(); ++i)
            for (size_t k = 0; k < v.size(); ++k)
                if (k != i && std::abs(v[k] - v[i]) < sep) out[i].push_back(int(k));
        return out;
    };
    const auto near_x = near_lines(xs), near_y = near_lines(ys);

    // Directions: 0 right (+x), 1 left, 2 down (+y), 3 up. d ^ 1 is the opposite direction.
    struct Edge {
        int to = -1;          // the neighbour, or -1
        int base = 0;         // the edge's left or top point
        bool across = false;  // horizontal
    };
    // The neighbour in direction d over edges free at `need` (kTight by default: every edge a path can use).
    auto step = [&](int p, int d, char need = kTight) {
        const int ix = p / ny, iy = p % ny;
        switch (d) {
        case 0: return right_ok[p] & need ? Edge{at(ix + 1, iy), p, true} : Edge{};
        case 1: return ix > 0 && right_ok[at(ix - 1, iy)] & need ? Edge{at(ix - 1, iy), at(ix - 1, iy), true} : Edge{};
        case 2: return down_ok[p] & need ? Edge{at(ix, iy + 1), p, false} : Edge{};
        default: return iy > 0 && down_ok[at(ix, iy - 1)] & need ? Edge{at(ix, iy - 1), at(ix, iy - 1), false} : Edge{};
        }
    };
    auto owner = [&](const Edge& e) -> int& { return e.across ? right_net[e.base] : down_net[e.base]; };
    auto direction = [&](int p, int q) {  // of the edge p -> q
        int d = 0;
        while (step(p, d).to != q) ++d;
        return d;
    };
    // How much a lane costs per unit length: free on its own trunk, then plain, beside its own trunk, beside
    // another net, back on its own trunk after leaving it (a loop), and along another net's line.
    auto rate = [&](const Edge& e, int net, bool left_trunk) {
        const int own = owner(e);
        if (own == net) return left_trunk ? 10.0f : 0.0f;
        if (own != -1) return 30.0f;
        const int ix = e.base / ny, iy = e.base % ny;
        float r = 1;
        for (const int k : e.across ? near_y[size_t(iy)] : near_x[size_t(ix)]) {
            const int beside = e.across ? right_net[at(ix, k)] : down_net[at(k, iy)];
            if (other(beside, net)) return 8.0f;
            if (beside == net) r = 3;
        }
        return r;
    };
    auto point = [&](int p) { return Pt{xs[p / ny], ys[p % ny]}; };
    // A corner costs some detour; crossing another link a little more (PCB-like traces); a corner on another link
    // much more (it reads as joining it).
    const float bend = 2 * gap, cross = 3 * gap, touch = 4 * bend;
    constexpr float kInf = std::numeric_limits<float>::infinity();

    // Grid points of each link's path (empty: the plain fallback).
    std::vector<std::vector<int>> grids(links.size());
    // Fills the maps above from every link's path but `skip`'s.
    auto occupy = [&](size_t skip) {
        std::ranges::fill(right_net, -1);
        std::ranges::fill(down_net, -1);
        std::ranges::fill(visit_net, -1);
        std::ranges::fill(corner_net, -1);
        for (size_t j = 0; j < links.size(); ++j) {
            if (j == skip || grids[j].empty()) continue;
            const std::vector<int>& g = grids[j];
            const int net = links[j].net;
            int arrived = 0;  // direction into g[i]; the stubs at both ends run rightwards
            for (size_t i = 0; i < g.size(); ++i) {
                mark(visit_net[size_t(g[i])], net);
                const int leave = i + 1 < g.size() ? direction(g[i], g[i + 1]) : 0;
                if (i + 1 < g.size()) mark(owner(step(g[i], leave)), net);
                if (leave != arrived) mark(corner_net[size_t(g[i])], net);
                arrived = leave;
            }
        }
    };
    // Routes link `i` against the others' current paths over edges free at `need`; false if there's no way.
    auto search = [&](size_t i, char need) {
        const LinkRoute& l = links[i];
        const int sx = find_line(xs, l.from.x + gap), sy = find_line(ys, l.from.y);
        const int ex = find_line(xs, l.to.x - gap), ey = find_line(ys, l.to.y);
        if (sx < 0 || sy < 0 || ex < 0 || ey < 0) return false;  // never: the pins made those lines
        const int start = at(sx, sy), goal = at(ex, ey);
        const Pt s = point(start), e = point(goal);
        const float h = need == kClear ? gap * kClearance : gap * 0.5f;
        if (blocked(s.x, s.x, s.y, s.y, h) || blocked(e.x, e.x, e.y, e.y, h)) return false;
        bool trunk_exists = false;
        for (size_t j = 0; j < links.size(); ++j) trunk_exists |= j != i && links[j].net == l.net && !grids[j].empty();

        // Dijkstra over (point, direction of arrival, has left its trunk); the start counts as arriving rightwards.
        auto state_of = [](int p, int d, int left) { return (p * 4 + d) * 2 + left; };
        std::vector<float> dist(size_t(n) * 8, kInf);
        std::vector<int> prev(size_t(n) * 8, -1);
        using Item = std::pair<float, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
        dist[size_t(state_of(start, 0, 0))] = 0;
        open.push({0.0f, state_of(start, 0, 0)});
        int best = -1;
        float best_cost = kInf;
        while (!open.empty()) {
            const auto [cost, state] = open.top();
            open.pop();
            if (cost > dist[size_t(state)] || cost >= best_cost) continue;
            const int left = state % 2, d = state / 2 % 4, p = state / 8;
            if (p == goal && d != 1) {  // arriving leftwards would need a U-turn into the pin
                const float total = cost + (d == 0 ? 0 : bend + (other(visit_net[size_t(p)], l.net) ? touch : 0));
                if (total < best_cost) best_cost = total, best = state;
            }
            for (int nd = 0; nd < 4; ++nd) {
                if (nd == (d ^ 1)) continue;  // no U-turns
                const Edge edge = step(p, nd, need);
                if (edge.to < 0) continue;
                const Pt a = point(p), b = point(edge.to);
                const float len = std::abs(a.x - b.x) + std::abs(a.y - b.y);
                const bool shared = owner(edge) == l.net, turn = nd != d;
                const int next_left = left || (trunk_exists && !shared);
                float c = cost + len * rate(edge, l.net, left);
                if (turn && !(shared && !left)) {
                    c += bend;
                    if (other(visit_net[size_t(p)], l.net)) c += touch;  // turning on another link
                }
                if (other(corner_net[size_t(edge.to)], l.net)) c += touch;  // passing another link's corner
                else if (other(visit_net[size_t(edge.to)], l.net)) c += cross;  // crossing another link
                const int next = state_of(edge.to, nd, next_left);
                if (c < dist[size_t(next)]) {
                    dist[size_t(next)] = c;
                    prev[size_t(next)] = state;
                    open.push({c, next});
                }
            }
        }
        if (best < 0) return false;
        for (int st = best; st >= 0; st = prev[size_t(st)]) grids[i].push_back(st / 8);
        std::ranges::reverse(grids[i]);
        return true;
    };
    auto route_one = [&](size_t i) {
        occupy(i);
        grids[i].clear();
        if (!search(i, kClear)) search(i, kTight);
    };

    // Each link in turn, then each again against all the others (rip-up and reroute): a link routed early no longer
    // forces later ones to cross it when it could have taken another lane.
    for (int pass = 0; pass < std::max(passes, 1); ++pass)
        for (size_t i = 0; i < links.size(); ++i) route_one(i);

    Routes out;
    std::map<int, std::set<std::pair<int, int>>> net_edges;  // edges laid by earlier links of each net
    for (size_t i = 0; i < links.size(); ++i) {
        const LinkRoute& l = links[i];
        const std::vector<int>& g = grids[i];
        if (g.empty()) {  // no way around: a plain right-angle path, drawn over whatever is in the way
            const float mx = (l.from.x + l.to.x) * 0.5f;
            out.paths.push_back(simplify({l.from, {mx, l.from.y}, {mx, l.to.y}, l.to}));
            continue;
        }
        auto& laid = net_edges[l.net];
        bool on_trunk = !laid.empty();
        std::vector<Pt> path{l.from};
        for (size_t k = 0; k < g.size(); ++k) {
            path.push_back(point(g[k]));
            if (k + 1 == g.size()) break;
            const Edge edge = step(g[k], direction(g[k], g[k + 1]));
            const std::pair<int, int> key{edge.base, edge.across};
            if (on_trunk && !laid.contains(key)) {  // the branch leaves the trunk here
                out.junctions.push_back(point(g[k]));
                on_trunk = false;
            }
        }
        for (size_t k = 0; k + 1 < g.size(); ++k) {
            const Edge edge = step(g[k], direction(g[k], g[k + 1]));
            laid.insert({edge.base, edge.across});
        }
        path.push_back(l.to);
        out.paths.push_back(simplify(path));
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
