// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_panel_registry_cleanup.cpp
 * @brief Tests that StaticPanelRegistry destroy callbacks properly null static pointers
 *
 * During printer switching, StaticPanelRegistry::destroy_all() destroys global panel
 * C++ objects. If the file-scope static lv_obj_t* pointers aren't also nulled, lazy
 * recreation checks (if (!s_panel_obj && g_panel)) fail and panels can never be
 * recreated for the new printer — clicks do nothing.
 *
 * These tests verify the destroy-and-recreate pattern works correctly.
 */

#include "ui_panel_calibration_pid.h"
#include "ui_panel_macros.h"

#include "../lvgl_test_fixture.h"
#include "core/static_panel_registry.h"
#include "core/static_subject_registry.h"
#include "misc/lv_timer_private.h" // timer_cb — assert the cancel neutered it

#include <memory>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Mock Panel Pattern (mirrors the real AMS/timelapse/retraction pattern)
// ============================================================================

namespace {

// Simulates the file-scope statics in ui_panel_ams.cpp etc.
struct MockPanelGlobals {
    std::unique_ptr<int> cpp_object;   // Simulates g_ams_panel
    lv_obj_t* cached_widget = nullptr; // Simulates s_ams_panel_obj
    int create_count = 0;              // Track how many times lazy creation ran
};

// Simulate the lazy creation + registry pattern used by get_global_ams_panel() etc.
// Returns the cpp_object value (like get_panel() returns panel_).
int* get_or_create_panel_correct(MockPanelGlobals& g, lv_obj_t* parent) {
    if (!g.cpp_object) {
        g.cpp_object = std::make_unique<int>(42);
        // CORRECT: Destroy callback nulls BOTH the widget pointer AND the unique_ptr
        StaticPanelRegistry::instance().register_destroy("MockPanel", [&g]() {
            g.cached_widget = nullptr;
            g.cpp_object.reset();
        });
    }

    // Lazy create the widget if not yet created
    if (!g.cached_widget && g.cpp_object) {
        g.cached_widget = lv_obj_create(parent);
        g.create_count++;
    }

    return g.cpp_object.get();
}

// The BUGGY version — only resets unique_ptr, leaves static pointer dangling
int* get_or_create_panel_buggy(MockPanelGlobals& g, lv_obj_t* parent) {
    if (!g.cpp_object) {
        g.cpp_object = std::make_unique<int>(42);
        // BUG: Destroy callback only resets unique_ptr, NOT the widget pointer
        StaticPanelRegistry::instance().register_destroy("MockPanelBuggy", [&g]() {
            g.cpp_object.reset();
            // Missing: g.cached_widget = nullptr;
        });
    }

    // Lazy create the widget if not yet created
    if (!g.cached_widget && g.cpp_object) {
        g.cached_widget = lv_obj_create(parent);
        g.create_count++;
    }

    return g.cpp_object.get();
}

} // namespace

// ============================================================================
// Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Panel registry: correct cleanup enables recreation",
                 "[shutdown][registry][panel-lifecycle]") {
    auto& registry = StaticPanelRegistry::instance();
    registry.destroy_all(); // Start clean

    MockPanelGlobals globals;

    SECTION("first creation works") {
        int* panel = get_or_create_panel_correct(globals, test_screen());
        REQUIRE(panel != nullptr);
        REQUIRE(*panel == 42);
        REQUIRE(globals.cached_widget != nullptr);
        REQUIRE(globals.create_count == 1);

        registry.destroy_all();
    }

    SECTION("destroy_all nulls both pointers") {
        get_or_create_panel_correct(globals, test_screen());
        REQUIRE(globals.cpp_object != nullptr);
        REQUIRE(globals.cached_widget != nullptr);

        registry.destroy_all();

        REQUIRE(globals.cpp_object == nullptr);
        REQUIRE(globals.cached_widget == nullptr);
    }

    SECTION("recreation works after destroy_all") {
        // First creation
        get_or_create_panel_correct(globals, test_screen());
        REQUIRE(globals.create_count == 1);

        // Simulate printer switch teardown
        registry.destroy_all();
        REQUIRE(globals.cached_widget == nullptr);
        REQUIRE(globals.cpp_object == nullptr);

        // Recreation (what happens when user clicks AMS after switching printers)
        int* panel = get_or_create_panel_correct(globals, test_screen());
        REQUIRE(panel != nullptr);
        REQUIRE(*panel == 42);
        REQUIRE(globals.cached_widget != nullptr);
        REQUIRE(globals.create_count == 2); // Created a second time

        registry.destroy_all();
    }

    SECTION("multiple destroy-recreate cycles work") {
        for (int cycle = 1; cycle <= 3; cycle++) {
            int* panel = get_or_create_panel_correct(globals, test_screen());
            REQUIRE(panel != nullptr);
            REQUIRE(globals.create_count == cycle);

            registry.destroy_all();
            REQUIRE(globals.cached_widget == nullptr);
            REQUIRE(globals.cpp_object == nullptr);
        }
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "Panel registry: buggy cleanup blocks recreation",
                 "[shutdown][registry][panel-lifecycle]") {
    auto& registry = StaticPanelRegistry::instance();
    registry.destroy_all(); // Start clean

    MockPanelGlobals globals;

    // First creation works fine
    get_or_create_panel_buggy(globals, test_screen());
    REQUIRE(globals.create_count == 1);
    REQUIRE(globals.cached_widget != nullptr);

    // Save the widget pointer before destroy (it will become dangling)
    lv_obj_t* old_widget = globals.cached_widget;

    // Simulate printer switch teardown — the BUGGY callback only resets unique_ptr
    registry.destroy_all();

    // BUG: cpp_object is null, but cached_widget still holds the old (dangling) pointer
    REQUIRE(globals.cpp_object == nullptr);
    REQUIRE(globals.cached_widget == old_widget); // Still non-null!

    // Attempt recreation — this is what fails in the real bug
    // The lazy check `if (!cached_widget && cpp_object)` fails because cached_widget
    // is non-null (dangling), so widget creation is skipped entirely.
    // A new cpp_object IS created, but get_panel() returns null because setup() was never called.
    int* panel = get_or_create_panel_buggy(globals, test_screen());

    // The C++ object was recreated...
    REQUIRE(panel != nullptr);
    // ...but the widget was NOT recreated (the bug!)
    REQUIRE(globals.create_count == 1); // Still 1 — lazy creation was skipped

    // Cleanup: manually null the dangling pointer so destroy_all doesn't crash
    globals.cached_widget = nullptr;
    registry.destroy_all();
}

TEST_CASE_METHOD(LVGLTestFixture, "Panel registry: destroy_all runs in reverse order",
                 "[shutdown][registry][panel-lifecycle]") {
    auto& registry = StaticPanelRegistry::instance();
    registry.destroy_all(); // Start clean

    std::vector<std::string> destruction_order;

    registry.register_destroy("PanelA",
                              [&destruction_order]() { destruction_order.push_back("A"); });
    registry.register_destroy("PanelB",
                              [&destruction_order]() { destruction_order.push_back("B"); });
    registry.register_destroy("PanelC",
                              [&destruction_order]() { destruction_order.push_back("C"); });

    registry.destroy_all();

    // Panels destroyed in reverse registration order (LIFO)
    REQUIRE(destruction_order.size() == 3);
    REQUIRE(destruction_order[0] == "C");
    REQUIRE(destruction_order[1] == "B");
    REQUIRE(destruction_order[2] == "A");
}

// ============================================================================
// Shutdown ordering: subject deinit must not resurrect a destroyed panel
// ============================================================================
//
// Application::shutdown() runs StaticPanelRegistry::destroy_all() first, then
// StaticSubjectRegistry::deinit_all(). A deinit callback written as
//
//     register_deinit("XPanel", []() { get_global_x_panel().deinit_subjects(); });
//
// reaches through the auto-creating getter, so it constructs a brand new panel
// after the real one was already destroyed. That replacement is never destroyed
// while LVGL and spdlog are alive — its destructor runs during static
// destruction, where the panel's members (Modal backdrops, observers, log calls)
// touch already-freed infrastructure. Panel getters register a destroy callback
// on first call, so a resurrected panel is observable as a non-empty
// StaticPanelRegistry after deinit_all().

// deinit_one() rather than deinit_all(): in a test binary the registry also
// holds entries from earlier fixtures, some of which capture already-destroyed
// objects (see the XMLTestFixture note in tests/test_fixtures.cpp).

TEST_CASE_METHOD(LVGLTestFixture,
                 "Panel registry: MacrosPanel is not resurrected by subject deinit",
                 "[shutdown][registry][panel-lifecycle]") {
    auto& panels = StaticPanelRegistry::instance();

    // Start from a clean slate — earlier tests may have left registrations.
    panels.destroy_all();

    // Boot-equivalent: lazily create the panel and register its subjects.
    get_global_macros_panel().init_subjects();
    REQUIRE(panels.count() >= 1);

    // Shutdown ordering: panels are destroyed first...
    panels.destroy_all();
    REQUIRE(panels.count() == 0);

    // ...then the subject deinit callbacks run.
    REQUIRE(StaticSubjectRegistry::instance().deinit_one("MacrosPanel"));

    // A callback reaching through the auto-creating getter would have built a
    // replacement panel, which registers a destroy callback of its own.
    REQUIRE(panels.count() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Panel registry: PIDCalibrationPanel is not resurrected by subject deinit",
                 "[shutdown][registry][panel-lifecycle]") {
    auto& panels = StaticPanelRegistry::instance();
    panels.destroy_all();

    get_global_pid_cal_panel().init_subjects();
    REQUIRE(panels.count() >= 1);

    panels.destroy_all();
    REQUIRE(panels.count() == 0);

    REQUIRE(StaticSubjectRegistry::instance().deinit_one("PIDCalibrationPanel"));

    REQUIRE(panels.count() == 0);
}

// A panel destroyed mid-calibration never runs stop_progress_tracking(), and
// StaticPanelRegistry::destroy_all() runs BEFORE lv_deinit() — so a live ETA tick
// would dispatch into a freed `this`. Same shape as the wizard's auto-probe timer
// (#1173); the destructor has to cancel it itself.
TEST_CASE_METHOD(LVGLTestFixture,
                 "Panel registry: PIDCalibrationPanel destructor cancels the ETA timer",
                 "[shutdown][registry][panel-lifecycle][timer][regression]") {
    auto panel = std::make_unique<PIDCalibrationPanel>();
    panel->arm_eta_timer_for_test();

    lv_timer_t* timer = panel->eta_timer_for_test();
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);

    panel.reset();

    // Neutered, not deleted: lv_timer_cancel_safe() nulls the callback and lets
    // lv_timer_handler reap the timer on its next pass. Reading it here is safe
    // because the timer is LVGL-owned memory, not the panel's.
    REQUIRE(timer->timer_cb == nullptr);

    // A still-armed tick would dispatch into the freed panel here.
    process_lvgl(50);
}

TEST_CASE_METHOD(LVGLTestFixture, "Panel registry: clear does not run callbacks",
                 "[shutdown][registry][panel-lifecycle]") {
    auto& registry = StaticPanelRegistry::instance();
    // destroy_all() lets a destroyer re-register through a lazy panel getter, so
    // clear() is what leaves the registry empty.
    registry.destroy_all();
    registry.clear();

    bool callback_ran = false;
    registry.register_destroy("TestPanel", [&callback_ran]() { callback_ran = true; });

    REQUIRE(registry.count() == 1);

    // clear() removes entries WITHOUT running callbacks (used after destroy_all in soft restart)
    registry.clear();

    REQUIRE(registry.count() == 0);
    REQUIRE_FALSE(callback_ran);
}
