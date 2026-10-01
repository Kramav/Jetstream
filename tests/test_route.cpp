#include "route.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

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

