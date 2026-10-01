#pragma once
// Link routing for graph front ends: right-angle paths that go around blocks. Pure geometry, no UI types.
#include <vector>

namespace remod {

struct Pt {
    float x = 0, y = 0;
    bool operator==(const Pt&) const = default;
};
struct Box {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // x0 < x1, y0 < y1 (y grows downwards)
    bool operator==(const Box&) const = default;
};
struct LinkRoute {
    int net = 0;  // links with the same net (one output) share their path where they can: a trunk that branches
    Pt from;      // output pin, on a block's right edge: the path leaves rightwards
    Pt to;        // input pin, on a block's left edge: the path arrives from the left
    bool operator==(const LinkRoute&) const = default;
};
struct Routes {
    std::vector<std::vector<Pt>> paths;  // one per link, from `from` to `to`; consecutive points share x or y
    std::vector<Pt> junctions;           // where a branch leaves its net's trunk
};

// Routes every link around `blocks`, keeping (0.9 of) `gap` away from them except for the stubs at its pins. Links of the same net reuse each other's
// path (a branch that leaves its trunk doesn't rejoin it); links of different nets avoid running along the same
// line or right beside it (closer than 0.6 gap), turning on each other, and crossing where a nearby lane avoids it.
// With `passes` > 1 each link is routed again against all the others, so the order of `links` matters little (3
// passes: 28% fewer crossings than 1 on generated layouts, at 3x the time). A link with no way around (e.g.
// overlapping blocks) gets a plain three-segment path instead.
Routes route_links(const std::vector<Box>& blocks, const std::vector<LinkRoute>& links, float gap, int passes = 3);

// Index of the first path passing within `tolerance` of `p`, or -1.
int hit_link(const std::vector<std::vector<Pt>>& paths, Pt p, float tolerance);

}  // namespace remod
