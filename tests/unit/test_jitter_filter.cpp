// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

// Tests for TouchJitterFilter — the shared jitter filter used by
// calibrated_read_cb in display_backend_fbdev.cpp. Tests exercise the
// exact same apply() method used in production, preventing divergence.
//
// Key behavior: the filter suppresses jitter until the first intentional
// movement exceeds the threshold ("breakout"). After breakout, all
// coordinates pass through unfiltered for smooth scrolling/dragging.

#include "touch_jitter_filter.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("Jitter filter: disabled when threshold is 0", "[jitter-filter]") {
    TouchJitterFilter f{};

    int32_t x = 100, y = 200;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 100);
    REQUIRE(y == 200);

    x = 103;
    y = 202;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 103);
    REQUIRE(y == 202);
}

TEST_CASE("Jitter filter: first press records position", "[jitter-filter]") {
    TouchJitterFilter f{.threshold_sq = 15 * 15};

    int32_t x = 400, y = 300;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);

    REQUIRE(x == 400);
    REQUIRE(y == 300);
    REQUIRE(f.tracking == true);
    REQUIRE(f.broken_out == false);
    REQUIRE(f.last_x == 400);
    REQUIRE(f.last_y == 300);
}

TEST_CASE("Jitter filter: small movements suppressed before breakout", "[jitter-filter]") {
    TouchJitterFilter f{.threshold_sq = 15 * 15}; // 225

    int32_t x = 400, y = 300;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);

    // Jitter within threshold
    x = 405;
    y = 303;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 400);
    REQUIRE(y == 300);
    REQUIRE(f.broken_out == false);

    // Opposite direction jitter
    x = 395;
    y = 298;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 400);
    REQUIRE(y == 300);

    // Right at boundary: dx=10, dy=10, dist²=200 < 225
    x = 410;
    y = 310;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 400);
    REQUIRE(y == 300);
}

TEST_CASE("Jitter filter: breakout disables filtering for rest of touch", "[jitter-filter]") {
    TouchJitterFilter f{.threshold_sq = 15 * 15};

    int32_t x = 400, y = 300;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);

    // Large movement triggers breakout: dx=20, dist²=400 > 225
    x = 420;
    y = 300;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 420);
    REQUIRE(y == 300);
    REQUIRE(f.broken_out == true);

    // After breakout: small movements pass through unfiltered (smooth scrolling)
    x = 423;
    y = 302;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 423);
    REQUIRE(y == 302);

    // Even 1px movements pass through
    x = 424;
    y = 302;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 424);
    REQUIRE(y == 302);
}

TEST_CASE("Jitter filter: tap release snaps to initial position", "[jitter-filter]") {
    TouchJitterFilter f{.threshold_sq = 15 * 15};

    // Press and jitter without breaking out
    int32_t x = 400, y = 300;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);

    x = 407;
    y = 304;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 400); // Suppressed

    // Release during tap: snaps to initial press position
    x = 408;
    y = 305;
    f.apply(LV_INDEV_STATE_RELEASED, x, y);
    REQUIRE(x == 400);
    REQUIRE(y == 300);
    REQUIRE(f.tracking == false);
    REQUIRE(f.broken_out == false);
}

TEST_CASE("Jitter filter: drag release passes through coordinates", "[jitter-filter]") {
    TouchJitterFilter f{.threshold_sq = 15 * 15};

    // Press and break out (start scrolling)
    int32_t x = 400, y = 300;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);

    x = 420;
    y = 300;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(f.broken_out == true);

    // Continue dragging
    x = 450;
    y = 310;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 450);

    // Release during drag: coordinates pass through (no snap)
    x = 455;
    y = 312;
    f.apply(LV_INDEV_STATE_RELEASED, x, y);
    REQUIRE(x == 455);
    REQUIRE(y == 312);
    REQUIRE(f.tracking == false);
}

TEST_CASE("Jitter filter: reset between taps", "[jitter-filter]") {
    TouchJitterFilter f{.threshold_sq = 15 * 15};

    // First tap (no breakout)
    int32_t x = 100, y = 100;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    f.apply(LV_INDEV_STATE_RELEASED, x, y);
    REQUIRE(f.tracking == false);
    REQUIRE(f.broken_out == false);

    // Second tap at different location — fresh start
    x = 500;
    y = 400;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 500);
    REQUIRE(y == 400);
    REQUIRE(f.last_x == 500);
    REQUIRE(f.last_y == 400);
    REQUIRE(f.broken_out == false);
}

TEST_CASE("Jitter filter: breakout resets between touches", "[jitter-filter]") {
    TouchJitterFilter f{.threshold_sq = 10 * 10};

    // First touch: break out (drag)
    int32_t x = 100, y = 100;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    x = 120;
    y = 100;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(f.broken_out == true);

    // Release
    f.apply(LV_INDEV_STATE_RELEASED, x, y);

    // Second touch: filter active again (not broken out)
    x = 200;
    y = 200;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(f.broken_out == false);

    // Small jitter suppressed on second touch
    x = 203;
    y = 202;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 200);
    REQUIRE(y == 200);
}

TEST_CASE("Jitter filter: smooth drag after breakout", "[jitter-filter]") {
    TouchJitterFilter f{.threshold_sq = 10 * 10};

    // Start drag
    int32_t x = 100, y = 100;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);

    // Break out: move to (115, 100), dist²=225 > 100
    x = 115;
    y = 100;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 115);
    REQUIRE(f.broken_out == true);

    // All subsequent moves pass through smoothly — no stepping
    x = 118;
    y = 101;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 118);
    REQUIRE(y == 101);

    x = 120;
    y = 102;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 120);
    REQUIRE(y == 102);

    x = 121;
    y = 102;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 121);
    REQUIRE(y == 102);
}

TEST_CASE("Jitter filter: exact threshold boundary", "[jitter-filter]") {
    TouchJitterFilter f{.threshold_sq = 10 * 10}; // 100

    int32_t x = 100, y = 100;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);

    // Exactly at threshold: dx=10, dy=0, dist²=100 == 100 → suppressed (<=)
    x = 110;
    y = 100;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 100);
    REQUIRE(y == 100);
    REQUIRE(f.broken_out == false);

    // One pixel past: dx=11, dy=0, dist²=121 > 100 → breakout
    x = 111;
    y = 100;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 111);
    REQUIRE(y == 100);
    REQUIRE(f.broken_out == true);
}

TEST_CASE("Jitter filter: negative threshold_sq treated as disabled", "[jitter-filter]") {
    TouchJitterFilter f{.threshold_sq = -100};

    int32_t x = 100, y = 200;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 100);
    REQUIRE(y == 200);

    x = 101;
    y = 201;
    f.apply(LV_INDEV_STATE_PRESSED, x, y);
    REQUIRE(x == 101);
    REQUIRE(y == 201);
}

TEST_CASE("Jitter filter: release without prior press is no-op", "[jitter-filter]") {
    TouchJitterFilter f{.threshold_sq = 15 * 15};

    int32_t x = 300, y = 400;
    f.apply(LV_INDEV_STATE_RELEASED, x, y);
    REQUIRE(x == 300);
    REQUIRE(y == 400);
    REQUIRE(f.tracking == false);
}
