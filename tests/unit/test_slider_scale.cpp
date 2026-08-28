// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_slider_scale.cpp
 * @brief SliderScale integer-position <-> real-value mapping
 *
 * Machine Limits and Retraction Settings both drive an integer lv_slider that
 * has to express a fractional value: square corner velocity in 0.5 steps
 * (tenths) and retract length in 0.01mm steps (hundredths). Both panels also
 * accept an exact value from the numeric keypad, which has to land back on a
 * valid slider position. That round trip is the behaviour under test.
 *
 * @see include/ui_slider_scale.h
 */

#include "ui_slider_scale.h"

#include "../catch_amalgamated.hpp"

using helix::ui::SliderScale;

TEST_CASE("SliderScale divisor 1 is the identity", "[slider_scale][ui]") {
    SliderScale scale{1};

    CHECK(scale.to_value(300) == Catch::Approx(300.0));
    CHECK(scale.to_slider(300.0) == 300);
    CHECK(scale.to_value(0) == Catch::Approx(0.0));
    CHECK(scale.to_slider(0.0) == 0);
}

TEST_CASE("SliderScale tenths express half steps", "[slider_scale][ui]") {
    SliderScale scale{10};

    SECTION("Square corner velocity round trip") {
        CHECK(scale.to_value(55) == Catch::Approx(5.5));
        CHECK(scale.to_slider(5.5) == 55);
    }

    SECTION("Whole numbers still land exactly") {
        CHECK(scale.to_value(50) == Catch::Approx(5.0));
        CHECK(scale.to_slider(5.0) == 50);
        CHECK(scale.to_value(200) == Catch::Approx(20.0));
        CHECK(scale.to_slider(20.0) == 200);
    }

    SECTION("Values below the resolution round to the nearest position") {
        CHECK(scale.to_slider(5.54) == 55);
        CHECK(scale.to_slider(5.56) == 56);
        CHECK(scale.to_slider(5.99) == 60);
        // 5.55 is deliberately not asserted: it is not representable in
        // binary, so which way it rounds is a property of the literal, not
        // of this helper.
    }
}

TEST_CASE("SliderScale hundredths cover retraction distances", "[slider_scale][ui]") {
    SliderScale scale{100};

    CHECK(scale.to_value(80) == Catch::Approx(0.8));
    CHECK(scale.to_slider(0.8) == 80);
    CHECK(scale.to_value(600) == Catch::Approx(6.0));
    CHECK(scale.to_slider(6.0) == 600);

    SECTION("Sub-hundredth input rounds rather than truncating") {
        CHECK(scale.to_slider(0.804) == 80);
        CHECK(scale.to_slider(0.806) == 81);
    }
}

TEST_CASE("SliderScale survives a nonsense divisor", "[slider_scale][ui]") {
    // A zero divisor would divide by zero and a negative one would invert the
    // slider. Both are construction bugs, but they must not produce inf/NaN
    // that then gets sent to the printer as a velocity limit.
    SECTION("Zero divisor behaves as 1") {
        SliderScale scale{0};
        CHECK(scale.to_value(300) == Catch::Approx(300.0));
        CHECK(scale.to_slider(300.0) == 300);
    }

    SECTION("Negative divisor behaves as 1") {
        SliderScale scale{-10};
        CHECK(scale.to_value(300) == Catch::Approx(300.0));
        CHECK(scale.to_slider(300.0) == 300);
    }
}

TEST_CASE("SliderScale clamps a keypad value into slider range", "[slider_scale][ui]") {
    SliderScale scale{10};

    SECTION("In-range values pass through") {
        CHECK(scale.to_slider_clamped(5.5, 10, 200) == 55);
    }

    SECTION("Below the minimum clamps to the minimum position") {
        CHECK(scale.to_slider_clamped(0.2, 10, 200) == 10);
    }

    SECTION("Above the maximum clamps to the maximum position") {
        CHECK(scale.to_slider_clamped(99.0, 10, 200) == 200);
    }

    SECTION("Exactly on the bounds is not clamped away") {
        CHECK(scale.to_slider_clamped(1.0, 10, 200) == 10);
        CHECK(scale.to_slider_clamped(20.0, 10, 200) == 200);
    }
}

TEST_CASE("SliderScale handles the machine-limit magnitudes", "[slider_scale][ui]") {
    // Max accel is the widest range in the app: 500-50000 on an integer
    // slider. It must survive the round trip without overflow or drift.
    SliderScale scale{1};

    CHECK(scale.to_value(50000) == Catch::Approx(50000.0));
    CHECK(scale.to_slider(50000.0) == 50000);
    CHECK(scale.to_slider_clamped(50000.0, 500, 50000) == 50000);
    CHECK(scale.to_slider_clamped(60000.0, 500, 50000) == 50000);
    CHECK(scale.to_slider_clamped(100.0, 500, 50000) == 500);
}
