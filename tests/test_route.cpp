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
    size_t links_total = 0, odd = 0;
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
        bool crowded = false;  // blocks closer than the gap are what the plain fallback is for
        for (size_t a = 0; a < blocks.size(); ++a)
            for (size_t b = a + 1; b < blocks.size(); ++b)
                crowded = crowded || (blocks[a].x0 < blocks[b].x1 + gap && blocks[b].x0 < blocks[a].x1 + gap &&
                                      blocks[a].y0 < blocks[b].y1 + gap && blocks[b].y0 < blocks[a].y1 + gap);
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
        for (size_t i = 0; i < links.size(); ++i)
            for (size_t k = 0; k < i; ++k) {
                if (links[i].net == links[k].net) continue;
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
    REQUIRE(links_total > 200);
    CHECK(odd * 20 < links_total);  // under 5% (the previous router: 22%, mostly hugging lines and touching corners)
}

TEST_CASE("route: a boxed-in pin falls back to a plain path") {
    const std::vector<Box> blocks{{0, 0, 100, 50}, {90, -10, 300, 60}, {400, 0, 500, 50}};
    const LinkRoute l{0, {100, 25}, {400, 125}};
    const auto r = remod::route_links(blocks, {l}, kGap);
    REQUIRE(r.paths.size() == 1);
    CHECK(r.paths[0].front() == l.from);
    CHECK(r.paths[0].back() == l.to);
    CHECK(r.paths[0].size() == 4);  // from -> middle x -> to
}

TEST_CASE("hit_link finds the path near a point") {
    const std::vector<std::vector<Pt>> paths{{{0, 0}, {100, 0}}, {{0, 50}, {50, 50}, {50, 100}}};
    CHECK(remod::hit_link(paths, {40, 3}, 4) == 0);
    CHECK(remod::hit_link(paths, {52, 80}, 4) == 1);
    CHECK(remod::hit_link(paths, {80, 80}, 4) == -1);
}

