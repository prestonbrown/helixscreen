// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_content_width_cap.cpp
 * @brief The ultrawide content width cap: 2.4 x screen height
 *
 * These tests cover the pure rule in include/content_width_cap.h. The wiring
 * that writes it onto overlays and main panels lives in
 * test_content_width_cap_push.cpp.
 */

#include "content_width_cap.h"

#include "../catch_amalgamated.hpp"

using helix::content_cap_overflow;
using helix::content_max_width;

TEST_CASE("Ultrawide screens cap content at 2.4 x their height", "[content-cap]") {
    // 2.4 x 320 is 768 exactly; floating-point 2.4 would truncate to 767.
    CHECK(content_max_width(320) == 768);
    CHECK(content_max_width(440) == 1056);

    // Both are far narrower than the space beside the nav bar, so the cap binds.
    CHECK(content_cap_overflow(1480 - 54, content_max_width(320)) == 1480 - 54 - 768);
    CHECK(content_cap_overflow(1920 - 76, content_max_width(440)) == 1920 - 76 - 1056);
}

TEST_CASE("Standard and portrait screens never reach the cap", "[content-cap]") {
    struct Screen {
        int32_t w;
        int32_t h;
    };
    // A view is never wider than the screen, so a cap at or above the screen
    // width can never bind on it.
    for (const Screen s : {Screen{800, 480}, Screen{1024, 600}, Screen{480, 800}, Screen{600, 1024},
                           Screen{320, 1480}}) {
        INFO(s.w << "x" << s.h);
        CHECK(content_max_width(s.h) >= s.w);
        CHECK(content_cap_overflow(s.w, content_max_width(s.h)) == 0);
    }
}

TEST_CASE("A view that fits the cap has no overflow", "[content-cap]") {
    CHECK(content_cap_overflow(768, 768) == 0);
    CHECK(content_cap_overflow(500, 768) == 0);
    CHECK(content_cap_overflow(769, 768) == 1);
    // No cap registered: nothing is taken from the view.
    CHECK(content_cap_overflow(1426, 0) == 0);
}
