// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_subject_initializer.cpp
 * @brief Unit tests for SubjectInitializer class
 *
 * Tests subject initialization ordering, observer registration, and API injection.
 *
 * Note: SubjectInitializer has heavy dependencies (all panels, LVGL subjects, etc.)
 * that make it difficult to unit test in isolation. These tests focus on the
 * RuntimeConfig interface and document expected behavior. Full initialization
 * tests are done as integration tests with the actual application.
 */

#include "ui_observer_guard.h"

#include "../../lvgl_test_fixture.h"
#include "../../test_helpers/ams_state_test_access.h"
#include "ams_state.h"
#include "core/observer_factory.h"
#include "filament_sensor_manager.h"
#include "runtime_config.h"
#include "settings_manager.h"
#include "subject_initializer.h"

#include <atomic>
#include <string>

#include "../../catch_amalgamated.hpp"

// ============================================================================
// RuntimeConfig Tests (SubjectInitializer dependency)
// ============================================================================

TEST_CASE("RuntimeConfig defaults to non-test mode", "[application][subjects][config]") {
    RuntimeConfig config;

    REQUIRE_FALSE(config.is_test_mode());
    REQUIRE_FALSE(config.test_mode);
}

TEST_CASE("RuntimeConfig test_mode enables mock flags", "[application][subjects][config]") {
    RuntimeConfig config;
    config.test_mode = true;

    REQUIRE(config.is_test_mode());
    REQUIRE(config.should_mock_wifi());
    REQUIRE(config.should_mock_ethernet());
    REQUIRE(config.should_mock_moonraker());
    REQUIRE(config.should_mock_ams());
    REQUIRE(config.should_mock_usb());
    REQUIRE(config.should_use_test_files());
}

TEST_CASE("RuntimeConfig real flags override mock behavior", "[application][subjects][config]") {
    RuntimeConfig config;
    config.test_mode = true;

    // Real WiFi flag should disable WiFi mocking
    config.use_real_wifi = true;
    REQUIRE_FALSE(config.should_mock_wifi());
    REQUIRE(config.should_mock_ethernet()); // Other mocks unaffected

    // Real Moonraker flag
    config.use_real_moonraker = true;
    REQUIRE_FALSE(config.should_mock_moonraker());

    // Real AMS flag
    config.use_real_ams = true;
    REQUIRE_FALSE(config.should_mock_ams());

    // Real files flag
    config.use_real_files = true;
    REQUIRE_FALSE(config.should_use_test_files());
}

TEST_CASE("RuntimeConfig production mode ignores real flags", "[application][subjects][config]") {
    RuntimeConfig config;
    config.test_mode = false;

    // In production mode, all mock functions return false
    // regardless of real_* flag settings
    REQUIRE_FALSE(config.should_mock_wifi());
    REQUIRE_FALSE(config.should_mock_moonraker());
    REQUIRE_FALSE(config.should_mock_usb());

    // Setting real flags in production mode has no effect
    config.use_real_wifi = true;
    REQUIRE_FALSE(config.should_mock_wifi());
}

TEST_CASE("RuntimeConfig skip_splash behavior", "[application][subjects][config]") {
    RuntimeConfig config;

    // Default: no skip
    REQUIRE_FALSE(config.skip_splash);
    REQUIRE_FALSE(config.should_skip_splash());

    // Explicit skip flag
    config.skip_splash = true;
    REQUIRE(config.should_skip_splash());

    // Reset and test that test_mode also skips splash
    config.skip_splash = false;
    config.test_mode = true;
    REQUIRE(config.should_skip_splash());
}

TEST_CASE("RuntimeConfig simulation speedup defaults", "[application][subjects][config]") {
    RuntimeConfig config;

    REQUIRE(config.sim_speedup == 1.0);
    REQUIRE(config.mock_ams_gate_count == 4);
}

TEST_CASE("RuntimeConfig gcode viewer defaults", "[application][subjects][config]") {
    RuntimeConfig config;

    REQUIRE(config.gcode_test_file == nullptr);
    REQUIRE_FALSE(config.gcode_camera_azimuth_set);
    REQUIRE_FALSE(config.gcode_camera_elevation_set);
    REQUIRE_FALSE(config.gcode_camera_zoom_set);
    REQUIRE(config.gcode_camera_zoom == 1.0f);
    REQUIRE_FALSE(config.gcode_debug_colors);
    REQUIRE(config.gcode_render_mode == -1);
}

TEST_CASE("RuntimeConfig test file path helper", "[application][subjects][config]") {
    const char* path = RuntimeConfig::get_default_test_file_path();

    REQUIRE(path != nullptr);
    REQUIRE(std::string(path).find("assets/test_gcodes") != std::string::npos);
    REQUIRE(std::string(path).find("3DBenchy.gcode") != std::string::npos);
}

TEST_CASE_METHOD(LVGLTestFixture, "ObserverGuard RAII removes observer on destruction",
                 "[application][subjects][observer]") {
    // Test that ObserverGuard properly removes observers when going out of scope
    // This verifies the RAII pattern used by SubjectInitializer

    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);

    std::atomic<int> callback_count{0};

    // Helper struct to act as a "Panel" for the observer factory
    struct TestReceiver {
        std::atomic<int>* counter;
    };
    TestReceiver receiver{&callback_count};

    {
        // Create observer in inner scope
        auto guard = helix::ui::observe<int>(
            &subject, &receiver, [](TestReceiver* r, int /*value*/) { r->counter->fetch_add(1); },
            subject_never_freed());

        REQUIRE(guard); // Guard should be valid

        // observe<int> defers callbacks via queue_update(), so we must
        // drain the update queue to process the initial subscription callback
        process_lvgl(10);
        REQUIRE(callback_count.load() == 1); // Initial callback on subscription

        // Value changes should trigger callback (drain queue after each)
        lv_subject_set_int(&subject, 42);
        process_lvgl(10);
        REQUIRE(callback_count.load() == 2);

        lv_subject_set_int(&subject, 100);
        process_lvgl(10);
        REQUIRE(callback_count.load() == 3);

        // Guard goes out of scope here - observer should be removed
    }

    // After guard destroyed, observer should be removed
    // Reset counter to verify no more callbacks
    callback_count.store(0);
    lv_subject_set_int(&subject, 200);
    process_lvgl(10);
    REQUIRE(callback_count.load() == 0); // No callback - observer was removed

    lv_subject_set_int(&subject, 300);
    process_lvgl(10);
    REQUIRE(callback_count.load() == 0); // Still no callback

    lv_subject_deinit(&subject);
}

TEST_CASE_METHOD(LVGLTestFixture, "ObserverGuard move semantics transfer ownership",
                 "[application][subjects][observer]") {
    // Test that move assignment properly transfers observer ownership
    // Important for SubjectInitializer which stores guards in member variables

    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);

    std::atomic<int> callback_count{0};

    struct TestReceiver {
        std::atomic<int>* counter;
    };
    TestReceiver receiver{&callback_count};

    ObserverGuard outer_guard; // Empty guard

    {
        auto inner_guard = helix::ui::observe<int>(
            &subject, &receiver, [](TestReceiver* r, int /*value*/) { r->counter->fetch_add(1); },
            subject_never_freed());

        REQUIRE(inner_guard);

        // observe<int> defers callbacks via queue_update(), drain the queue
        process_lvgl(10);
        REQUIRE(callback_count.load() == 1);

        // Move to outer scope
        outer_guard = std::move(inner_guard);

        REQUIRE(outer_guard);       // Outer now owns it
        REQUIRE_FALSE(inner_guard); // Inner is empty after move

        // Inner scope ends - but observer should NOT be removed (ownership transferred)
    }

    // Observer should still be active via outer_guard
    callback_count.store(0);
    lv_subject_set_int(&subject, 42);
    process_lvgl(10);
    REQUIRE(callback_count.load() == 1); // Callback still works

    // Explicitly reset to remove observer
    outer_guard.reset();
    REQUIRE_FALSE(outer_guard);

    callback_count.store(0);
    lv_subject_set_int(&subject, 100);
    process_lvgl(10);
    REQUIRE(callback_count.load() == 0); // No callback after reset

    lv_subject_deinit(&subject);
}

// Panels built by init_panels() observe the language subject from their
// constructors, and LVGL refuses an observer on a subject not yet initialised.
TEST_CASE_METHOD(LVGLTestFixture, "Settings subjects are live once core and state subjects are",
                 "[application][subjects][i18n]") {
    auto& settings = helix::SettingsManager::instance();
    settings.deinit_subjects();
    REQUIRE_FALSE(settings.are_subjects_initialized());

    SubjectInitializer subjects;
    subjects.init_core_and_state();

    CHECK(settings.are_subjects_initialized());
}

// The clog meter's motion-sensor fallback re-syncs on FilamentSensorManager's
// motion signature, whose subject only exists once the sensor managers have
// initialised, so the wiring belongs to the initializer (#1823).
TEST_CASE_METHOD(LVGLTestFixture, "Core and state init wires the clog meter to the motion sensors",
                 "[application][subjects][motion][1823]") {
    auto& ams = helix::AmsState::instance();
    auto& sensors = helix::FilamentSensorManager::instance();
    sensors.init_subjects();
    lv_subject_t* signature = sensors.get_motion_signature_subject();
    helix::AmsStateTestAccess::remove_motion_sensor_observer(ams);
    const uint32_t before = lv_ll_get_len(&signature->subs_ll);

    SubjectInitializer subjects;
    subjects.init_core_and_state();

    CHECK(lv_ll_get_len(&signature->subs_ll) == before + 1);
}
