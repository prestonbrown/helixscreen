// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/// Tests for `ctl long_press` — the server-side hold that makes gesture-driven
/// UI reachable from the remote control.
///
/// The hold duration is the whole correctness question, so it lives in a pure
/// constexpr function and is tested here without LVGL or a running server.

#include "remote_pointer.h"

#include "catch_amalgamated.hpp"

using helix::remote::pointer_long_press_hold_ms;

TEST_CASE("long_press hold clears the configured threshold", "[remote][long_press]") {
    // The whole point: the hold must be strictly longer than the threshold LVGL
    // is comparing against, or the gesture lands as a click.
    for (int32_t threshold : {300, 400, 500, 750, 1000, 1500}) {
        CAPTURE(threshold);
        CHECK(pointer_long_press_hold_ms(threshold) > threshold);
    }
}

TEST_CASE("long_press hold tracks the threshold rather than a fixed duration",
          "[remote][long_press]") {
    // A user who raises the long-press time in Touch & Input must still get a
    // long press from ctl. A hardcoded hold would silently regress to a click.
    const int32_t low = pointer_long_press_hold_ms(300);
    const int32_t high = pointer_long_press_hold_ms(1500);
    CHECK(high > low);
    CHECK(high > 1500);
}

TEST_CASE("long_press hold survives a nonsense threshold", "[remote][long_press]") {
    // 0 means "unset" — the settings manager clamps to [300,1500], but this
    // function is also reachable before settings load.
    CHECK(pointer_long_press_hold_ms(0) > 500);
    // Negative or absurdly small values must not produce a hold that could never
    // register as a long press.
    CHECK(pointer_long_press_hold_ms(-1) > 500);
    CHECK(pointer_long_press_hold_ms(1) >= 550);
}

TEST_CASE("long_press hold leaves margin for one sampling period", "[remote][long_press]") {
    // LVGL counts from the sample that first reports the press, not from when the
    // command ran, so a hold of exactly the threshold races the indev timer. The
    // margin is what stops this being flaky under CI load.
    const int32_t threshold = 500;
    CHECK(pointer_long_press_hold_ms(threshold) - threshold >= 200);
}
