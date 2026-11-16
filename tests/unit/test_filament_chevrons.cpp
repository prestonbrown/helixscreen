// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Chevron geometry shared by the AMS path canvas and the filament panel's
// motion glyph, and the manual-move duration that times the glyph.

#include "filament_motion.h"
#include "filament_path_geometry.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::pathgeo;
using Catch::Approx;

namespace {

FilamentPath vertical(float y0, float y1) {
    FilamentPath p;
    p.add_line(10.0f, y0, 10.0f, y1);
    return p;
}

} // namespace

TEST_CASE("chevron_at points its tip along the travel direction", "[filament-chevrons]") {
    // Travel straight down the screen (+y).
    Chevron c = chevron_at({10.0f, 50.0f}, {0.0f, 1.0f}, 4.0f, 6.0f);
    CHECK(c.tip.x == Approx(10.0f));
    CHECK(c.tip.y == Approx(53.0f));
    // Both arm ends trail the tip by the full depth, spread across the path.
    CHECK(c.left.y == Approx(47.0f));
    CHECK(c.right.y == Approx(47.0f));
    CHECK(std::fabs(c.left.x - c.right.x) == Approx(8.0f));
    CHECK((c.left.x + c.right.x) * 0.5f == Approx(10.0f));
}

TEST_CASE("a vertical tube and its chevrons share one center pixel", "[filament-chevrons]") {
    CHECK(centered_gauge(8) == 9);
    CHECK(centered_gauge(9) == 9);

    ChevronSize s;
    s.half_width = 6.75f;
    s.stroke = 2.0f;
    const ChevronSize snapped = pixel_centered(s);
    CHECK(snapped.half_width == Approx(7.0f));
    CHECK(snapped.stroke == Approx(3.0f));
    s.stroke = 3.0f;
    CHECK(pixel_centered(s).stroke == Approx(3.0f));
}

TEST_CASE("chevron_size_for_gauge scales with the tube and floors the stroke",
          "[filament-chevrons]") {
    ChevronSize thin = chevron_size_for_gauge(4.0f);
    ChevronSize thick = chevron_size_for_gauge(8.0f);
    CHECK(thin.stroke >= 1.0f);
    CHECK(thick.half_width > thin.half_width);
    CHECK(thick.spacing > thin.spacing);
    // Arms reach past the tube walls so the shape reads over the filament.
    CHECK(thick.half_width * 2.0f > 8.0f);
    // Chevrons never touch one another.
    CHECK(thick.spacing > thick.depth * 2.0f);
    CHECK(chevron_size_for_gauge(0.0f).stroke == Approx(1.0f));
    CHECK(chevron_size_for_gauge(-3.0f).half_width > 0.0f);
}

TEST_CASE("chevrons_along streams toward the path end, and back on reverse",
          "[filament-chevrons]") {
    const FilamentPath p = vertical(0.0f, 100.0f);
    ChevronSize s{3.0f, 4.0f, 1.0f, 20.0f};
    Chevron out[16];

    int n = chevrons_along(p, s, 5.0f, false, out, 16);
    REQUIRE(n == 5); // centers at 5, 25, 45, 65, 85
    for (int i = 0; i < n; ++i) {
        CHECK(out[i].tip.y > out[i].left.y); // points down: toward the end
        CHECK(out[i].tip.y == Approx(5.0f + 20.0f * i + 2.0f));
    }

    // Reverse: travel from the end back to the start, tips pointing up, and a
    // growing phase moves the stream up the path.
    int r = chevrons_along(p, s, 5.0f, true, out, 16);
    REQUIRE(r == 5);
    CHECK(out[0].tip.y < out[0].left.y);
    CHECK(out[0].tip.y == Approx(95.0f - 2.0f));
    Chevron later[16];
    chevrons_along(p, s, 8.0f, true, later, 16);
    CHECK(later[0].tip.y < out[0].tip.y);
}

TEST_CASE("chevrons_along keeps whole chevrons inside the path", "[filament-chevrons]") {
    const FilamentPath p = vertical(0.0f, 41.0f);
    ChevronSize s{3.0f, 4.0f, 1.0f, 20.0f};
    Chevron out[16];
    // Phase 0 puts a center at 0 whose arms would trail above the start, and
    // 40 whose tip would pass 41: only the center at 20 stays.
    int n = chevrons_along(p, s, 0.0f, false, out, 16);
    REQUIRE(n == 1);
    CHECK(out[0].tip.y == Approx(22.0f));
    // A phase past one spacing wraps, and a negative one wraps forward.
    CHECK(chevrons_along(p, s, 20.0f, false, out, 16) == 1);
    CHECK(chevrons_along(p, s, -15.0f, false, out, 16) == 2); // centers 5, 25
    // The cap is honored.
    CHECK(chevrons_along(vertical(0.0f, 400.0f), s, 5.0f, false, out, 3) == 3);
}

TEST_CASE("chevrons_along follows a bend's tangent", "[filament-chevrons]") {
    FilamentPath p;
    p.add_line(0.0f, 0.0f, 0.0f, 30.0f);
    p.add_line(0.0f, 30.0f, 60.0f, 30.0f);
    ChevronSize s{3.0f, 4.0f, 1.0f, 20.0f};
    Chevron out[16];
    int n = chevrons_along(p, s, 10.0f, false, out, 16); // centers 10, 30, 50, 70
    REQUIRE(n == 4);
    CHECK(out[0].tip.y > out[0].left.y); // still on the downward leg
    CHECK(out[3].tip.x > out[3].left.x); // on the rightward leg, pointing right
}

TEST_CASE("chevrons_along rejects empty input", "[filament-chevrons]") {
    ChevronSize s{3.0f, 4.0f, 1.0f, 20.0f};
    Chevron out[4];
    FilamentPath empty;
    CHECK(chevrons_along(empty, s, 0.0f, false, out, 4) == 0);
    CHECK(chevrons_along(vertical(0.0f, 2.0f), s, 0.0f, false, out, 4) == 0);
    CHECK(chevrons_along(vertical(0.0f, 100.0f), s, 0.0f, false, nullptr, 4) == 0);
    ChevronSize no_spacing{3.0f, 4.0f, 1.0f, 0.0f};
    CHECK(chevrons_along(vertical(0.0f, 100.0f), no_spacing, 0.0f, false, out, 4) == 0);
}

TEST_CASE("manual_motion_duration_ms times a move by its length and speed", "[filament-chevrons]") {
    CHECK(helix::manual_motion_duration_ms(10, 5) == 2000u);
    CHECK(helix::manual_motion_duration_ms(25, 10) == 2500u);
    CHECK(helix::manual_motion_duration_ms(1, 50) == helix::kMinManualMotionMs);
    CHECK(helix::manual_motion_duration_ms(10, 0) == helix::kMinManualMotionMs);
    CHECK(helix::manual_motion_duration_ms(0, 5) == helix::kMinManualMotionMs);
    CHECK(helix::manual_motion_duration_ms(100000, 1) == helix::kMaxManualMotionMs);
}

TEST_CASE("filament_motion_from_int reads unknown values as idle", "[filament-chevrons]") {
    using helix::FilamentMotion;
    CHECK(helix::filament_motion_from_int(1) == FilamentMotion::Extrude);
    CHECK(helix::filament_motion_from_int(2) == FilamentMotion::Retract);
    CHECK(helix::filament_motion_from_int(0) == FilamentMotion::Idle);
    CHECK(helix::filament_motion_from_int(7) == FilamentMotion::Idle);
    CHECK(helix::filament_motion_from_int(-1) == FilamentMotion::Idle);
}

TEST_CASE("translate_path moves lines and arcs alike", "[filament-chevrons]") {
    FilamentPath p;
    p.add_line(0.0f, 0.0f, 0.0f, 10.0f);
    p.add_arc(10.0f, 10.0f, 10.0f, 3.14159265f, -1.5707963f);
    const PathPoint before_end = path_point_at(p, path_length(p));
    translate_path(p, 5.0f, -3.0f);
    const PathPoint start = path_point_at(p, 0.0f);
    CHECK(start.x == Approx(5.0f));
    CHECK(start.y == Approx(-3.0f));
    const PathPoint end = path_point_at(p, path_length(p));
    CHECK(end.x == Approx(before_end.x + 5.0f).margin(1e-3));
    CHECK(end.y == Approx(before_end.y - 3.0f).margin(1e-3));
}

TEST_CASE("chevrons_along leaves hidden segments bare", "[filament-chevrons]") {
    // Three 40px legs; the middle one runs inside a box.
    FilamentPath p;
    p.add_line(0.0f, 0.0f, 0.0f, 40.0f);
    p.add_line(0.0f, 40.0f, 0.0f, 80.0f);
    p.add_line(0.0f, 80.0f, 0.0f, 120.0f);
    const bool shown[] = {true, false, true};
    ChevronSize s{3.0f, 4.0f, 1.0f, 10.0f};
    Chevron out[32];
    const int all = chevrons_along(p, s, 5.0f, false, out, 32);
    const int n = chevrons_along(p, s, 5.0f, false, out, 32, shown);
    CHECK(all == 12);
    REQUIRE(n == 8); // centers 5..35 and 85..115
    for (int i = 0; i < n; ++i) {
        const float center = out[i].tip.y - 2.0f;
        CHECK((center < 40.0f - 2.0f || center > 80.0f + 2.0f));
    }
}

TEST_CASE("chevron_size_for_gauge gives a 2px stroke from a 4px gauge", "[filament-chevrons]") {
    CHECK(chevron_size_for_gauge(3.0f).stroke == Approx(1.0f));
    CHECK(chevron_size_for_gauge(5.0f).stroke == Approx(2.0f));
    CHECK(chevron_size_for_gauge(10.0f).stroke == Approx(4.0f));
}

TEST_CASE("chevron_overlaps tests the chevron's extent against a box", "[filament-chevrons]") {
    const Chevron c = chevron_at({10.0f, 50.0f}, {0.0f, 1.0f}, 4.0f, 6.0f); // x 6..14, y 47..53
    CHECK(chevron_overlaps(c, {0.0f, 52.0f, 20.0f, 60.0f}, 0.0f));          // tip enters the box
    CHECK_FALSE(chevron_overlaps(c, {0.0f, 55.0f, 20.0f, 60.0f}, 0.0f));
    CHECK(chevron_overlaps(c, {0.0f, 55.0f, 20.0f, 60.0f}, 2.0f)); // within the pad
    CHECK_FALSE(chevron_overlaps(c, {15.0f, 40.0f, 30.0f, 60.0f}, 0.0f));
}
