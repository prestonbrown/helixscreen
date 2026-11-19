// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_path_canvas.h"

#include "src/ui/filament/ui_filament_path_plan.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// hub_box_hit() — pure coordinate hit-test for the selector/hub box.
//
// Box under test: center (200, 90), width 100, height 30, margin 4.
//   X half-extent + margin = 50 + 4 = 54  -> hit when |x - 200| <= 54
//   Y half-extent + margin = 15 + 4 = 19  -> hit when |y - 90|  <= 19
// ============================================================================

namespace {
constexpr int32_t kCx = 200;
constexpr int32_t kCy = 90;
constexpr int32_t W = 100;
constexpr int32_t H = 30;
constexpr int32_t kMargin = 4;

bool hit(int32_t x, int32_t y) {
    lv_point_t p{x, y};
    return helix::ui::hub_box_hit(p, kCx, kCy, W, H, kMargin);
}
} // namespace

TEST_CASE("hub_box_hit: dead-center point hits", "[canvas][hit_test]") {
    REQUIRE(hit(200, 90));
}

TEST_CASE("hub_box_hit: inside-edge points hit", "[canvas][hit_test]") {
    // Just inside the un-margined box bounds.
    REQUIRE(hit(151, 76));  // left/top inner corner-ish (|x-200|=49, |y-90|=14)
    REQUIRE(hit(249, 104)); // right/bottom (|x-200|=49, |y-90|=14)
}

TEST_CASE("hub_box_hit: exact box-edge points hit", "[canvas][hit_test]") {
    REQUIRE(hit(150, 90));  // left box edge (|x-200|=50 <= 54)
    REQUIRE(hit(250, 90));  // right box edge
    REQUIRE(hit(200, 75));  // top box edge (|y-90|=15 <= 19)
    REQUIRE(hit(200, 105)); // bottom box edge
}

TEST_CASE("hub_box_hit: within-margin points hit", "[canvas][hit_test]") {
    REQUIRE(hit(253, 90));  // |x-200|=53 <= 54
    REQUIRE(hit(254, 90));  // |x-200|=54 == limit, inclusive
    REQUIRE(hit(200, 109)); // |y-90|=19 == limit, inclusive
}

TEST_CASE("hub_box_hit: just-outside-margin points miss", "[canvas][hit_test]") {
    REQUIRE_FALSE(hit(255, 90));  // |x-200|=55 > 54
    REQUIRE_FALSE(hit(200, 110)); // |y-90|=20 > 19
}

TEST_CASE("hub_box_hit: far-left point misses", "[canvas][hit_test]") {
    REQUIRE_FALSE(hit(140, 90)); // |x-200|=60 > 54
}

TEST_CASE("hub_box_hit: above-box point misses", "[canvas][hit_test]") {
    REQUIRE_FALSE(hit(200, 60)); // |y-90|=30 > 19
}

TEST_CASE("hub_box_hit: argument order is width-then-height", "[canvas][hit_test]") {
    // A wide-short box: 120 wide, 20 tall, margin 0.
    // X half-extent 60, Y half-extent 10. A point at x-offset 55, y-offset 12
    // hits only if width is used for X (55<=60) and height for Y... but 12>10
    // so it MUST miss. If args were swapped (h for X, w for Y), it would hit.
    lv_point_t p{kCx + 55, kCy + 12};
    REQUIRE_FALSE(helix::ui::hub_box_hit(p, kCx, kCy, 120, 20, 0));
    // Same x-offset, y-offset 8 -> hits (within both extents).
    lv_point_t p2{kCx + 55, kCy + 8};
    REQUIRE(helix::ui::hub_box_hit(p2, kCx, kCy, 120, 20, 0));
}

// ============================================================================
// Record-and-read contract guards. The renderer stores the EXACT drawn box in
// data->{buffer,bypass}_hit_rect (absolute coords); the click handler reads it
// back via hub_box_hit. These cases reproduce the renderer's rect-construction
// math and assert hub_box_hit reproduces the same hit decisions the old inline
// re-derived tests made — so a future drift in either side fails here.
// ============================================================================

namespace {
// Mirror the bypass rect the renderer records: half-extents sensor_r*3 (X) and
// sensor_r*4 (Y) about (bypass_x, bypass_merge_y), read with margin 0.
bool bypass_hit(lv_point_t p, int32_t bypass_x, int32_t merge_y, int32_t sensor_r) {
    lv_area_t r{bypass_x - sensor_r * 3, merge_y - sensor_r * 4, bypass_x + sensor_r * 3,
                merge_y + sensor_r * 4};
    int32_t cx = (r.x1 + r.x2) / 2;
    int32_t cy = (r.y1 + r.y2) / 2;
    return helix::ui::hub_box_hit(p, cx, cy, r.x2 - r.x1, r.y2 - r.y1, 0);
}
} // namespace

TEST_CASE("buffer control hit rect: capsule, button and the thumb's glow, 4 px to spare",
          "[canvas][hit_test]") {
    using helix::ui::fpath::buffer_control_hit;
    const int32_t cx = 200, cy = 150;
    // cap 14: track 35 either side, a 16 px button cell, a 20 px thumb whose glow reaches 13.
    lv_area_t small = buffer_control_hit(cx, cy, 14);
    CHECK((small.x1 == cx - 39 && small.x2 == cx + 55));
    CHECK((small.y1 == cy - 17 && small.y2 == cy + 17));
    // cap 20: track 50, a 22 px cell, the thumb's glow reaching 16.
    lv_area_t large = buffer_control_hit(cx, cy, 20);
    CHECK((large.x1 == cx - 54 && large.x2 == cx + 76));
    CHECK((large.y1 == cy - 20 && large.y2 == cy + 20));
}

TEST_CASE("bypass hit rect: full-extent bounds preserved (margin 0)", "[canvas][hit_test]") {
    // sensor_r=4 -> X half-extent 12, Y half-extent 16.
    const int32_t bx = 300, my = 120, sr = 4;
    REQUIRE(bypass_hit({bx, my}, bx, my, sr));            // center hits
    REQUIRE(bypass_hit({bx + 12, my}, bx, my, sr));       // X edge inclusive
    REQUIRE_FALSE(bypass_hit({bx + 13, my}, bx, my, sr)); // beyond X misses
    REQUIRE(bypass_hit({bx, my + 16}, bx, my, sr));       // Y edge inclusive
    REQUIRE_FALSE(bypass_hit({bx, my + 17}, bx, my, sr)); // beyond Y misses
}

// ============================================================================
// recorded_box_hit() — a box recorded at render time follows the widget when it
// moves without a re-render (an overlay sliding in, a scrolled parent).
// ============================================================================

TEST_CASE("recorded box: hits where it is drawn after the widget moves", "[canvas][hit_test]") {
    // Recorded while the panel was mid-slide at x=700; it has since settled at
    // x=88. The drawn box moved with the widget, 612px to the left.
    const lv_area_t selector = {792, 204, 1180, 232}; // center (986, 218)
    const lv_point_t rendered_at{700, 60};
    const lv_point_t now_at{88, 60};

    REQUIRE(helix::ui::recorded_box_hit({374, 218}, selector, rendered_at, now_at, 4));
    REQUIRE(helix::ui::recorded_box_hit({185, 218}, selector, rendered_at, now_at, 4));
    REQUIRE_FALSE(helix::ui::recorded_box_hit({986, 218}, selector, rendered_at, now_at, 4));
    REQUIRE_FALSE(helix::ui::recorded_box_hit({374, 240}, selector, rendered_at, now_at, 4));
}

TEST_CASE("recorded box: unmoved widget reads the box as recorded", "[canvas][hit_test]") {
    const lv_area_t box = {100, 50, 200, 80};
    const lv_point_t origin{20, 10};
    REQUIRE(helix::ui::recorded_box_hit({150, 65}, box, origin, origin, 0));
    REQUIRE(helix::ui::recorded_box_hit({200, 80}, box, origin, origin, 0));
    REQUIRE_FALSE(helix::ui::recorded_box_hit({201, 65}, box, origin, origin, 0));
    REQUIRE(helix::ui::recorded_box_hit({203, 65}, box, origin, origin, 4));
}
