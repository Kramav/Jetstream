#include "route.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <random>

using remod::Box;
using remod::LinkRoute;
using remod::Pt;

namespace {

constexpr float kGap = 20;

bool inside(const Box& b, Pt p) { return p.x > b.x0 + 0.01f && p.x < b.x1 - 0.01f && p.y > b.y0 + 0.01f && p.y < b.y1 - 0.01f; }

// Right angles only, from the output rightwards, into the input from the left, never through a block.
void check_path(const std::vector<Pt>& path, const LinkRoute& l, const std::vector<Box>& blocks) {
    REQUIRE(path.size() >= 2);
    CHECK(path.front() == l.from);
    CHECK(path.back() == l.to);
    CHECK((path[1].y == path[0].y && path[1].x > path[0].x));  // leaves rightwards
    const Pt a = path[path.size() - 2], b = path.back();
    CHECK((a.y == b.y && b.x > a.x));  // arrives from the left
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Pt p = path[i], q = path[i + 1];
        CHECK((p.x == q.x || p.y == q.y));
        for (int k = 0; k <= 20; ++k) {  // sample along the segment
            const Pt s{p.x + (q.x - p.x) * k / 20, p.y + (q.y - p.y) * k / 20};
            for (const Box& box : blocks) CHECK_FALSE(inside(box, s));
        }
    }
}

// Two segments lying on the same line and overlapping by more than a point.
bool overlap(const std::vector<Pt>& a, const std::vector<Pt>& b) {
    for (size_t i = 0; i + 1 < a.size(); ++i)
        for (size_t k = 0; k + 1 < b.size(); ++k) {
            const Pt p = a[i], q = a[i + 1], r = b[k], s = b[k + 1];
            if (p.y == q.y && r.y == s.y && p.y == r.y &&
                std::min(std::max(p.x, q.x), std::max(r.x, s.x)) - std::max(std::min(p.x, q.x), std::min(r.x, s.x)) > 1)
                return true;
            if (p.x == q.x && r.x == s.x && p.x == r.x &&
                std::min(std::max(p.y, q.y), std::max(r.y, s.y)) - std::max(std::min(p.y, q.y), std::min(r.y, s.y)) > 1)
                return true;
        }
    return false;
}

// How many sides of `b` the path lines, outside it within `reach`, each for over half its length (2+ reads as the
// block's outline).
int lined_sides(const std::vector<Pt>& path, const Box& b, float reach) {
    float lined[4] = {};
    for (size_t k = 0; k + 1 < path.size(); ++k) {
        const Pt p = path[k], q = path[k + 1];
        if (p.y == q.y) {
            const float along = std::min(std::max(p.x, q.x), b.x1) - std::max(std::min(p.x, q.x), b.x0);
            if (along > 0 && p.y < b.y0 && b.y0 - p.y <= reach) lined[0] += along;
            if (along > 0 && p.y > b.y1 && p.y - b.y1 <= reach) lined[1] += along;
        } else {
            const float along = std::min(std::max(p.y, q.y), b.y1) - std::max(std::min(p.y, q.y), b.y0);
            if (along > 0 && p.x < b.x0 && b.x0 - p.x <= reach) lined[2] += along;
            if (along > 0 && p.x > b.x1 && p.x - b.x1 <= reach) lined[3] += along;
        }
    }
    const float w = (b.x1 - b.x0) / 2, h = (b.y1 - b.y0) / 2;
    return (lined[0] > w) + (lined[1] > w) + (lined[2] > h) + (lined[3] > h);
}

}  // namespace

TEST_CASE("route: aligned pins with nothing between are one straight line") {
    const std::vector<Box> blocks{{0, 0, 100, 50}, {200, 0, 300, 50}};
    const LinkRoute l{0, {100, 25}, {200, 25}};
    const auto r = remod::route_links(blocks, {l}, kGap);
    REQUIRE(r.paths.size() == 1);
    CHECK(r.paths[0] == std::vector<Pt>{{100, 25}, {200, 25}});
    CHECK(r.junctions.empty());
}

TEST_CASE("route: goes around a block in the way") {
    const std::vector<Box> blocks{{0, 0, 100, 50}, {150, -20, 250, 70}, {300, 0, 400, 50}};
    const LinkRoute l{0, {100, 25}, {300, 25}};
    const auto r = remod::route_links(blocks, {l}, kGap);
    check_path(r.paths.at(0), l, blocks);
    CHECK(r.paths[0].size() > 2);
}

TEST_CASE("route: a backwards link loops around, still leaving right and entering from the left") {
    const std::vector<Box> blocks{{300, 0, 400, 50}, {0, 100, 100, 150}};
    const LinkRoute l{0, {400, 25}, {0, 125}};
    const auto r = remod::route_links(blocks, {l}, kGap);
    check_path(r.paths.at(0), l, blocks);
}

TEST_CASE("route: one output into two inputs shares a trunk that branches once") {
    const std::vector<Box> blocks{{0, 0, 100, 50}, {300, -100, 400, -50}, {300, 100, 400, 150}};
    const std::vector<LinkRoute> links{{0, {100, 25}, {300, -75}}, {0, {100, 25}, {300, 125}}};
    const auto r = remod::route_links(blocks, links, kGap);
    for (size_t i = 0; i < links.size(); ++i) check_path(r.paths.at(i), links[i], blocks);
    CHECK(r.paths[0][1] == r.paths[1][1]);  // the same first segment
    REQUIRE(r.junctions.size() == 1);
    CHECK(remod::hit_link({r.paths[0]}, r.junctions[0], 0.1f) == 0);  // the branch point is on the trunk
}

TEST_CASE("route: different outputs don't run along the same line") {
    const std::vector<Box> blocks{{0, 0, 100, 150}, {300, 0, 400, 150}};
    const std::vector<LinkRoute> links{{0, {100, 25}, {300, 125}}, {1, {100, 125}, {300, 25}}};
    const auto r = remod::route_links(blocks, links, kGap);
    for (size_t i = 0; i < links.size(); ++i) check_path(r.paths.at(i), links[i], blocks);
    CHECK_FALSE(overlap(r.paths[0], r.paths[1]));
}

// Layouts shaped like the app's: columns of blocks with pins down their sides, links mostly forwards.
TEST_CASE("route: different outputs keep apart on generated layouts (no shared or hugging lines, no touching corners)") {
    const float font = 16, gap = font * 0.8f;
    std::mt19937 rng(7);
    auto uni = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
    auto pick = [&](int n) { return int(rng() % unsigned(n)); };
    size_t links_total = 0, odd = 0, too_close = 0, portals = 0, outlined = 0;
    for (int layout = 0; layout < 60; ++layout) {
        std::vector<Box> blocks;
        std::vector<int> column;
        for (int c = 0, cols = 3 + pick(4); c < cols; ++c)
            for (float y = uni(-100, 100), rows = float(1 + pick(3)); rows > 0; --rows) {
                const float x = c * 420 + uni(-120, 120), w = uni(220, 320), h = font * 8 + uni(20, 120);
                blocks.push_back({x, y, x + w, y + h});
                column.push_back(c);
                y += h + uni(30, 200);
            }
        // Blocks closer than two gaps leave no room for the full clearance (links squeeze to half the gap there).
        const float room = 2 * gap;
        bool crowded = false;
        for (size_t a = 0; a < blocks.size(); ++a)
            for (size_t b = a + 1; b < blocks.size(); ++b)
                crowded = crowded || (blocks[a].x0 < blocks[b].x1 + room && blocks[b].x0 < blocks[a].x1 + room &&
                                      blocks[a].y0 < blocks[b].y1 + room && blocks[b].y0 < blocks[a].y1 + room);
        if (crowded) continue;
        std::vector<LinkRoute> links;
        std::vector<std::pair<int, int>> used;
        for (int k = 0, count = int(blocks.size()) * 2; k < count; ++k) {
            const int a = pick(int(blocks.size())), b = pick(int(blocks.size())), o = pick(2), in = pick(3);
            if (a == b || (column[size_t(b)] <= column[size_t(a)] && pick(4) != 0) ||
                std::ranges::count(used, std::pair{b, in}))
                continue;
            used.emplace_back(b, in);
            links.push_back({a * 2 + o, {blocks[size_t(a)].x1, blocks[size_t(a)].y0 + font * (3 + 1.6f * float(o))},
                             {blocks[size_t(b)].x0, blocks[size_t(b)].y0 + font * (3 + 1.6f * float(in))}});
        }
        const auto r = remod::route_links(blocks, links, gap);
        links_total += links.size();
        portals += size_t(std::ranges::count(r.portals, 1));
        for (size_t i = 0; i < links.size(); ++i)
            for (const Box& b : blocks)
                if (!r.portals[i] && lined_sides(r.paths[i], b, 1.5f * gap) >= 2) ++outlined;
        // Lines keep clear of every block, but for the first and last segment at the link's own blocks' edges.
        for (size_t i = 0; i < links.size(); ++i) {
            if (r.portals[i]) continue;  // no line between its stubs
            const auto& p = r.paths[i];
            for (size_t a = 0; a + 1 < p.size(); ++a)
                for (const Box& b : blocks) {
                    const bool own = (a == 0 && std::abs(b.x1 - p[0].x) < 0.01f) ||
                                     (a + 2 == p.size() && std::abs(b.x0 - p.back().x) < 0.01f);
                    const float c = gap * 0.85f;  // the router keeps 0.9
                    const float x0 = std::min(p[a].x, p[a + 1].x), x1 = std::max(p[a].x, p[a + 1].x);
                    const float y0 = std::min(p[a].y, p[a + 1].y), y1 = std::max(p[a].y, p[a + 1].y);
                    const bool close = x1 > b.x0 - c && x0 < b.x1 + c && y1 > b.y0 - c && y0 < b.y1 + c;
                    if (close && !own) ++too_close;
                }
        }
        for (size_t i = 0; i < links.size(); ++i)
            for (size_t k = 0; k < i; ++k) {
                if (links[i].net == links[k].net || r.portals[i] || r.portals[k]) continue;
                const auto& p = r.paths[i];
                const auto& q = r.paths[k];
                for (size_t a = 0; a + 1 < p.size(); ++a)
                    for (size_t b = 0; b + 1 < q.size(); ++b) {
                        const Pt p0 = p[a], p1 = p[a + 1], q0 = q[b], q1 = q[b + 1];
                        const bool ph = p0.y == p1.y, qh = q0.y == q1.y;
                        if (ph != qh) continue;
                        const float d = ph ? std::abs(p0.y - q0.y) : std::abs(p0.x - q0.x);
                        const float shared = ph ? std::min(std::max(p0.x, p1.x), std::max(q0.x, q1.x)) -
                                                      std::max(std::min(p0.x, p1.x), std::min(q0.x, q1.x))
                                                : std::min(std::max(p0.y, p1.y), std::max(q0.y, q1.y)) -
                                                      std::max(std::min(p0.y, p1.y), std::min(q0.y, q1.y));
                        if (d < gap * 0.6f && shared > 1) ++odd;  // on, or hugging, the other line
                    }
                for (const auto* corners : {&p, &q})  // a corner of one on the other
                    for (size_t c = 1; c + 1 < corners->size(); ++c)
                        if (remod::hit_link({corners == &p ? q : p}, (*corners)[c], 0.5f) == 0) ++odd;
            }
    }
    REQUIRE(links_total > 100);
    CHECK(outlined == 0);
    CHECK(portals * 20 < links_total);  // under 5% have no clean route
    CHECK(odd * 20 < links_total);
    CHECK(too_close == 0);  // under 5% (the previous router: 22%, mostly hugging lines and touching corners)
}

// Proper crossings between two paths (a horizontal segment of one through a vertical segment of the other).
int crossings(const std::vector<Pt>& a, const std::vector<Pt>& b) {
    int n = 0;
    for (size_t i = 0; i + 1 < a.size(); ++i)
        for (size_t k = 0; k + 1 < b.size(); ++k)
            for (int flip = 0; flip < 2; ++flip) {
                const Pt h0 = flip ? b[k] : a[i], h1 = flip ? b[k + 1] : a[i + 1];
                const Pt v0 = flip ? a[i] : b[k], v1 = flip ? a[i + 1] : b[k + 1];
                if (h0.y != h1.y || v0.x != v1.x) continue;
                if (v0.x > std::min(h0.x, h1.x) && v0.x < std::max(h0.x, h1.x) && h0.y > std::min(v0.y, v1.y) &&
                    h0.y < std::max(v0.y, v1.y))
                    ++n;
            }
    return n;
}

TEST_CASE("route: links into one block's inputs take lanes that don't cross when they can") {
    // The upper input's link arrives from below the lower input (the user's Export / Convert layout, 2026-10-01):
    // routed one at a time, the first link took the inner lane and the second had to cross it.
    const std::vector<Box> blocks{{-100, 10, 0, 60}, {-100, 100, 0, 150}, {200, -30, 300, 60}};
    const std::vector<LinkRoute> links{{0, {0, 40}, {200, 0}}, {1, {0, 120}, {200, 25}}};
    const auto r = remod::route_links(blocks, links, kGap);
    for (size_t i = 0; i < links.size(); ++i) check_path(r.paths.at(i), links[i], blocks);
    CHECK(crossings(r.paths[0], r.paths[1]) == 0);
    CHECK_FALSE(overlap(r.paths[0], r.paths[1]));
}

TEST_CASE("route: a boxed-in pin is a portal pair: a stub at each pin, no line between") {
    const std::vector<Box> blocks{{0, 0, 100, 50}, {90, -10, 300, 60}, {400, 0, 500, 50}};
    const LinkRoute l{0, {100, 25}, {400, 125}};
    const auto r = remod::route_links(blocks, {l}, kGap);
    REQUIRE(r.portals == std::vector<char>{1});
    CHECK(r.paths[0] == std::vector<Pt>{{100, 25}, {140, 25}, {360, 125}, {400, 125}});
    CHECK(remod::hit_link(r.paths, {120, 25}, 2, r.portals) == 0);    // the out stub
    CHECK(remod::hit_link(r.paths, {380, 125}, 2, r.portals) == 0);   // the in stub
    CHECK(remod::hit_link(r.paths, {250, 75}, 2, r.portals) == -1);   // nothing between
}

TEST_CASE("route: never the block's outline (the user's wrapped Original texture, 2026-10-01)") {
    // A Split tucked under Original texture's right end, its input left of the texture output feeding it: the only
    // route at full clearance went right round Original texture, reading as its border.
    const float gap = 12.8f;
    const std::vector<Box> blocks{{100, 85, 475, 240}, {482, 237, 608, 378}};
    const LinkRoute l{0, {475, 218}, {482, 278}};
    const auto r = remod::route_links(blocks, {l}, gap);
    REQUIRE(r.paths.size() == 1);
    for (const Box& b : blocks) CHECK(lined_sides(r.paths[0], b, 1.5f * gap) < 2);
}

TEST_CASE("route: the long way round is a portal") {
    // A wall between the blocks: around it is ten times the direct distance.
    const std::vector<Box> blocks{{0, 0, 100, 50}, {150, -1000, 200, 1000}, {300, 0, 400, 50}};
    const LinkRoute l{0, {100, 25}, {300, 25}};
    CHECK(remod::route_links(blocks, {l}, kGap).portals == std::vector<char>{1});
    // A plain backwards link is no detour: it's routed.
    const std::vector<Box> back{{300, 0, 400, 50}, {0, 100, 100, 150}};
    CHECK(remod::route_links(back, {{0, {400, 25}, {0, 125}}}, kGap).portals == std::vector<char>{0});
}

TEST_CASE("route: lines take the lane a little away from a block when there's room") {
    // Two blocks far apart vertically, the link coming down past the target's left side: it runs on a lane further
    // out than the one right beside the side.
    const std::vector<Box> blocks{{0, 0, 100, 50}, {300, 200, 400, 400}};
    const LinkRoute l{0, {100, 25}, {300, 380}};
    const auto r = remod::route_links(blocks, {l}, kGap);
    check_path(r.paths.at(0), l, blocks);
    bool passes_beside = false;  // a vertical run alongside the target's left side
    for (size_t k = 0; k + 1 < r.paths[0].size(); ++k) {
        const Pt p = r.paths[0][k], q = r.paths[0][k + 1];
        if (p.x != q.x || std::max(p.y, q.y) < 250 || std::min(p.y, q.y) > 350) continue;
        passes_beside = true;
        CHECK(p.x <= 300 - 2 * kGap + 0.01f);  // not on the lane right beside it
    }
    CHECK(passes_beside);
}

TEST_CASE("hit_link finds the path near a point") {
    const std::vector<std::vector<Pt>> paths{{{0, 0}, {100, 0}}, {{0, 50}, {50, 50}, {50, 100}}};
    CHECK(remod::hit_link(paths, {40, 3}, 4) == 0);
    CHECK(remod::hit_link(paths, {52, 80}, 4) == 1);
    CHECK(remod::hit_link(paths, {80, 80}, 4) == -1);
}

