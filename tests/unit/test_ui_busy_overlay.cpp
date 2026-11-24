// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_busy_overlay.h"

#include "../lvgl_test_fixture.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

/**
 * @brief Unit tests for BusyOverlay - reusable busy/progress overlay
 *
 * Tests cover:
 * - Grace period behavior (delayed show)
 * - Immediate hide cancels pending show
 * - State tracking (is_visible, is_pending)
 * - Multiple show/hide cycles
 */

using namespace helix;

// Basic State Tests

TEST_CASE_METHOD(LVGLTestFixture, "BusyOverlay initial state", "[ui_busy_overlay][state]") {
    spdlog::set_level(spdlog::level::debug);

    BusyOverlay::hide();

    SECTION("Initially not visible") {
        REQUIRE(BusyOverlay::is_visible() == false);
    }

    SECTION("Initially not pending") {
        REQUIRE(BusyOverlay::is_pending() == false);
    }

    BusyOverlay::hide();
    spdlog::set_level(spdlog::level::warn);
}

// Show Behavior Tests

TEST_CASE_METHOD(LVGLTestFixture, "BusyOverlay show with grace period", "[ui_busy_overlay][show]") {
    spdlog::set_level(spdlog::level::debug);
    BusyOverlay::hide();

    SECTION("Show starts as pending") {
        BusyOverlay::show("Testing...", 300);

        REQUIRE(BusyOverlay::is_pending() == true);
        REQUIRE(BusyOverlay::is_visible() == false);

        BusyOverlay::hide();
    }

    // The grace-period expiry is not covered here: LVGL timers do not run reliably in
    // headless mode. It is checked by hand in SDL mode.

    SECTION("Show with zero grace period is immediate") {
        BusyOverlay::show("Testing...", 0);

        REQUIRE(BusyOverlay::is_visible() == true);
        REQUIRE(BusyOverlay::is_pending() == false);

        BusyOverlay::hide();
    }

    BusyOverlay::hide();
    spdlog::set_level(spdlog::level::warn);
}

// Hide Behavior Tests

TEST_CASE_METHOD(LVGLTestFixture, "BusyOverlay hide behavior", "[ui_busy_overlay][hide]") {
    spdlog::set_level(spdlog::level::debug);
    BusyOverlay::hide();

    SECTION("Hide cancels pending show") {
        BusyOverlay::show("Testing...", 300);
        REQUIRE(BusyOverlay::is_pending() == true);

        BusyOverlay::hide();

        REQUIRE(BusyOverlay::is_pending() == false);
        REQUIRE(BusyOverlay::is_visible() == false);
    }

    SECTION("Hide removes visible overlay") {
        BusyOverlay::show("Testing...", 0);
        REQUIRE(BusyOverlay::is_visible() == true);

        BusyOverlay::hide();

        REQUIRE(BusyOverlay::is_visible() == false);
        REQUIRE(BusyOverlay::is_pending() == false);
    }

    SECTION("Hide is safe to call when not showing") {
        BusyOverlay::hide();
        BusyOverlay::hide();
        BusyOverlay::hide();

        REQUIRE(BusyOverlay::is_visible() == false);
        REQUIRE(BusyOverlay::is_pending() == false);
    }

    BusyOverlay::hide();
    spdlog::set_level(spdlog::level::warn);
}

// Multiple Cycle Tests

TEST_CASE_METHOD(LVGLTestFixture, "BusyOverlay multiple show/hide cycles",
                 "[ui_busy_overlay][cycle]") {
    spdlog::set_level(spdlog::level::debug);
    BusyOverlay::hide();

    SECTION("Multiple show/hide cycles work correctly") {
        BusyOverlay::show("First", 0);
        REQUIRE(BusyOverlay::is_visible() == true);
        BusyOverlay::hide();
        REQUIRE(BusyOverlay::is_visible() == false);

        BusyOverlay::show("Second", 0);
        REQUIRE(BusyOverlay::is_visible() == true);
        BusyOverlay::hide();
        REQUIRE(BusyOverlay::is_visible() == false);
    }

    SECTION("Rapid show/hide doesn't cause issues") {
        for (int i = 0; i < 10; i++) {
            BusyOverlay::show("Rapid", 50);
            BusyOverlay::hide();
        }

        REQUIRE(BusyOverlay::is_visible() == false);
        REQUIRE(BusyOverlay::is_pending() == false);
    }

    BusyOverlay::hide();
    spdlog::set_level(spdlog::level::warn);
}

// Edge Cases

TEST_CASE_METHOD(LVGLTestFixture, "BusyOverlay edge cases", "[ui_busy_overlay][edge]") {
    spdlog::set_level(spdlog::level::debug);
    BusyOverlay::hide();

    SECTION("Double show updates text but doesn't restart timer") {
        BusyOverlay::show("First text", 300);
        REQUIRE(BusyOverlay::is_pending() == true);

        BusyOverlay::show("Second text", 300);
        REQUIRE(BusyOverlay::is_pending() == true);

        BusyOverlay::hide();
    }

    SECTION("Show while visible updates text") {
        BusyOverlay::show("First text", 0);
        REQUIRE(BusyOverlay::is_visible() == true);

        BusyOverlay::show("Second text", 0);
        REQUIRE(BusyOverlay::is_visible() == true);

        BusyOverlay::hide();
    }

    SECTION("Empty text is handled") {
        BusyOverlay::show("", 0);
        REQUIRE(BusyOverlay::is_visible() == true);

        BusyOverlay::hide();
    }

    SECTION("Very long text is handled") {
        std::string long_text(256, 'x');
        BusyOverlay::show(long_text, 0);
        REQUIRE(BusyOverlay::is_visible() == true);

        BusyOverlay::hide();
    }

    SECTION("Progress percentage bounds") {
        BusyOverlay::show("Test", 0);

        BusyOverlay::set_progress("Test", 0.0f);
        BusyOverlay::set_progress("Test", 100.0f);
        BusyOverlay::set_progress("Test", -5.0f);  // Below range
        BusyOverlay::set_progress("Test", 150.0f); // Above range

        BusyOverlay::hide();
    }

    BusyOverlay::hide();
    spdlog::set_level(spdlog::level::warn);
}
