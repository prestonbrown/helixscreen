// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where a baked unit drawing's spools stand, from the lane geometry the bake records.

#include "ams_unit_shell.h"
#include "unit_art_geometry.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::tray;
using Catch::Approx;

TEST_CASE("unit art geometry: wound filament shrinks linearly from full to the core",
          "[unit-art][geometry]") {
    CHECK(wound_radius(0.25f, 0.93f, 1.0f) == Approx(0.93f));
    CHECK(wound_radius(0.25f, 0.93f, 0.0f) == Approx(0.25f));
    CHECK(wound_radius(0.25f, 0.93f, 0.5f) == Approx(0.59f));
    CHECK(wound_radius(0.25f, 0.93f, 1.5f) == Approx(0.93f));
    CHECK(wound_radius(0.25f, 0.93f, -1.0f) == Approx(0.25f));
}

TEST_CASE("unit art geometry: the bake records every spool unit's lanes", "[unit-art][geometry]") {
    for (const char* id : {"box_turtle", "ace_pro", "ams_2_pro", "qidi_box", "cfs"}) {
        CAPTURE(id);
        const UnitArtGeom* geom = unit_art_geometry(id, 4);
        REQUIRE(geom != nullptr);
        CHECK(geom->aspect > 0.5f);
        for (int i = 0; i < 4; ++i) {
            const ArtLaneGeom& g = geom->lane[i];
            CAPTURE(i);
            CHECK(g.core < g.wound);
            // The spool and its outlet sit inside what the drawing covers.
            CHECK(g.box_x0 >= geom->drawn[0] - 0.01f);
            CHECK(g.box_x1 <= geom->drawn[2] + 0.01f);
            CHECK(g.outlet_y > g.box_y0);
            CHECK(g.outlet_y <= geom->drawn[3] + 0.01f);
            if (i > 0)
                CHECK(geom->lane[i - 1].box_x0 < g.box_x0);
        }
    }
    // A selector rail draws no spools, and no unit is baked past the lanes it is built with.
    CHECK(unit_art_geometry("ercf_rail", 6) == nullptr);
    CHECK(unit_art_geometry("box_turtle", 7) != nullptr);
    CHECK(unit_art_geometry("box_turtle", 9) == nullptr);
}

namespace {
/// The point @p radius flange radii from the end of @p g's axis, straight out along it
/// (past the near flange when @p near_end, else past the far one).
void off_end(const ArtLaneGeom& g, bool near_end, float radius, float& x, float& y) {
    const float det = g.ey_x * g.ez_y - g.ey_y * g.ez_x;
    const float ax = g.near_x - g.far_x, ay = g.near_y - g.far_y;
    // The axis in the flange basis, where discs are circles.
    const float u = (ax * g.ez_y - ay * g.ez_x) / det, v = (g.ey_x * ay - g.ey_y * ax) / det;
    const float len = std::hypot(u, v), s = near_end ? radius / len : -radius / len;
    x = (near_end ? g.near_x : g.far_x) + s * (u * g.ey_x + v * g.ez_x);
    y = (near_end ? g.near_y : g.far_y) + s * (u * g.ey_y + v * g.ez_y);
}
} // namespace

TEST_CASE("unit art geometry: a spool is every flange disc between its two flanges",
          "[unit-art][geometry]") {
    const ArtLaneGeom& g = unit_art_geometry("box_turtle", 4)->lane[1];
    CHECK(art_lane_distance(g, g.far_x, g.far_y) == Approx(0.0f).margin(1e-5));
    CHECK(art_lane_distance(g, g.near_x, g.near_y) == Approx(0.0f).margin(1e-5));
    // Every point of a flange's disc is on the spool.
    const float mx = (g.far_x + g.near_x) / 2, my = (g.far_y + g.near_y) / 2;
    CHECK(art_lane_distance(g, mx + g.ez_x, my + g.ez_y) <= 1.0f + 1e-4f);
    CHECK(art_lane_distance(g, g.near_x + 0.5f * g.ey_x, g.near_y + 0.5f * g.ey_y) <= 0.5f + 1e-4f);
    // Straight out past either end the distance is how far out.
    float x, y;
    off_end(g, true, 0.7f, x, y);
    CHECK(art_lane_distance(g, x, y) == Approx(0.7f).margin(1e-4));
    off_end(g, false, 1.3f, x, y);
    CHECK(art_lane_distance(g, x, y) == Approx(1.3f).margin(1e-4));

    // Coverage is whole inside, none outside, and half on the edge.
    const float k = art_lane_px_per_unit(g, 300.0f);
    CHECK(k > 1.0f);
    CHECK(art_lane_coverage(g, mx, my, 0.5f, k) == Approx(1.0f));
    off_end(g, true, 0.9f, x, y);
    CHECK(art_lane_coverage(g, x, y, 0.5f, k) == Approx(0.0f));
    off_end(g, true, 0.5f, x, y);
    CHECK(art_lane_coverage(g, x, y, 0.5f, k) == Approx(0.5f).margin(1e-3));
}
