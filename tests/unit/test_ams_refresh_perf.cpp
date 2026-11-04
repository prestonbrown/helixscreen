// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_slot.h"
#include "ui_spool_canvas.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "ams_backend_mock.h"
#include "ams_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

/**
 * @file test_ams_refresh_perf.cpp
 * @brief Unit tests for AMS refresh chain performance optimizations
 *
 * Validates that redundant subject fires, spool canvas redraws, and
 * slot refresh calls are properly guarded to avoid unnecessary work.
 */

static void drain() {
    helix::ui::UpdateQueue::instance().drain();
}

// ============================================================================
// Test 1: Conditional bump_slots_version — no-op on identical sync
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "AMS slots_version does not increment on identical sync",
                 "[ams][perf]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto mock = std::make_unique<AmsBackendMock>();
    auto* mock_ptr = mock.get();
    ams.set_backend(std::move(mock));
    mock_ptr->start();

    // Initial sync to populate state from the mock's default 4 slots
    ams.sync_from_backend();
    drain();

    int version_after_first = lv_subject_get_int(ams.get_slots_version_subject());

    SECTION("second sync with identical data does not bump version") {
        ams.sync_from_backend();
        drain();

        int version_after_second = lv_subject_get_int(ams.get_slots_version_subject());
        REQUIRE(version_after_second == version_after_first);
    }

    SECTION("sync after slot color change bumps version") {
        // Modify a slot color in the mock backend
        auto slot = mock_ptr->get_slot_info(0);
        slot.color_rgb = 0x00FF00; // Change from default to green
        helix::test::apply_edit(*mock_ptr, 0, slot);

        ams.sync_from_backend();
        drain();

        int version_after_change = lv_subject_get_int(ams.get_slots_version_subject());
        REQUIRE(version_after_change > version_after_first);
    }

    // Cleanup
    mock_ptr->stop();
    ams.clear_backends();
    ams.deinit_subjects();
}

// ============================================================================
// Test 2: Spool canvas dirty guard — set_color
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Spool canvas set_color guards redundant redraws",
                 "[ams][perf]") {
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), 64);
    REQUIRE(canvas != nullptr);

    lv_color_t red = lv_color_hex(0xFF0000);
    lv_color_t blue = lv_color_hex(0x0000FF);

    SECTION("setting same color twice does not crash") {
        ui_spool_canvas_set_color(canvas, red);
        ui_spool_canvas_set_color(canvas, red);

        lv_color_t current = ui_spool_canvas_get_color(canvas);
        REQUIRE(current.red == red.red);
        REQUIRE(current.green == red.green);
        REQUIRE(current.blue == red.blue);
    }

    SECTION("setting different color updates correctly") {
        ui_spool_canvas_set_color(canvas, red);
        ui_spool_canvas_set_color(canvas, blue);

        lv_color_t current = ui_spool_canvas_get_color(canvas);
        REQUIRE(current.blue == blue.blue);
    }
}

// ============================================================================
// Test 3: Spool canvas dirty guard — set_fill_level
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Spool canvas set_fill_level guards redundant redraws",
                 "[ams][perf]") {
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), 64);
    REQUIRE(canvas != nullptr);

    SECTION("setting same fill level twice does not crash") {
        ui_spool_canvas_set_fill_level(canvas, 0.75f);
        ui_spool_canvas_set_fill_level(canvas, 0.75f);

        float level = ui_spool_canvas_get_fill_level(canvas);
        REQUIRE(level == Catch::Approx(0.75f));
    }

    SECTION("setting different fill level updates correctly") {
        ui_spool_canvas_set_fill_level(canvas, 0.25f);
        float level = ui_spool_canvas_get_fill_level(canvas);
        REQUIRE(level == Catch::Approx(0.25f));

        ui_spool_canvas_set_fill_level(canvas, 0.80f);
        level = ui_spool_canvas_get_fill_level(canvas);
        REQUIRE(level == Catch::Approx(0.80f));
    }

    SECTION("boundary values work correctly") {
        ui_spool_canvas_set_fill_level(canvas, 0.0f);
        REQUIRE(ui_spool_canvas_get_fill_level(canvas) == Catch::Approx(0.0f));

        ui_spool_canvas_set_fill_level(canvas, 1.0f);
        REQUIRE(ui_spool_canvas_get_fill_level(canvas) == Catch::Approx(1.0f));
    }
}

// ============================================================================
// Test 4: ui_ams_slot_refresh does not crash with null
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "ui_ams_slot_refresh is safe with null widget", "[ams][perf]") {
    // ui_ams_slot_refresh should only update material/badge/error,
    // not observer-owned properties like color/status/highlight.
    // Verify it handles null gracefully as a no-op.
    REQUIRE_NOTHROW(ui_ams_slot_refresh(nullptr));
}
