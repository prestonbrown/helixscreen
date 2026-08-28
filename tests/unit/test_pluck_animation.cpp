// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../include/ui_pluck_animation.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

TEST_CASE("isometric projection matches the generator", "[belt][anim]") {
    // screen_x = OX + (x - y) * cos(30) * SC
    // screen_y = OY + ((x + y) * 0.5 - z) * SC
    const auto p = pluck_iso_project(0.0f, 0.0f, 0.0f, 100.0f, 50.0f, 1.0f);
    CHECK(p.x == Catch::Approx(100.0f));
    CHECK(p.y == Catch::Approx(50.0f));

    const auto q = pluck_iso_project(10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    CHECK(q.x == Catch::Approx(8.66f).margin(0.01f));
    CHECK(q.y == Catch::Approx(5.0f).margin(0.01f));
}

TEST_CASE("z raises a point on screen", "[belt][anim]") {
    const auto low = pluck_iso_project(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    const auto high = pluck_iso_project(0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 1.0f);
    CHECK(high.y < low.y);
}

TEST_CASE("the loop visits three frames and returns", "[belt][anim]") {
    CHECK(pluck_frame_at_ms(0) == 0);
    CHECK(pluck_frame_at_ms(PLUCK_LOOP_MS - 1) == 2);
    CHECK(pluck_frame_at_ms(PLUCK_LOOP_MS) == 0);
    CHECK(pluck_frame_at_ms(PLUCK_LOOP_MS * 3 + 10) == pluck_frame_at_ms(10));
}

TEST_CASE("the pull profile peaks in the middle frame", "[belt][anim]") {
    // Frame 2 is the held deflection; frames 1 and 3 are taut.
    CHECK(pluck_deflection_at_ms(0) == Catch::Approx(0.0f).margin(0.01f));
    const float mid = pluck_deflection_at_ms(PLUCK_LOOP_MS / 2);
    CHECK(mid > 0.5f);
}
