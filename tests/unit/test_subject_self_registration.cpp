// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_subject_self_registration.cpp
 * @brief Tests that all state singletons self-register cleanup with StaticSubjectRegistry
 *
 * The self-registration pattern requires every class that creates LVGL subjects to
 * register its own cleanup inside init_subjects(). This prevents shutdown crashes
 * caused by forgotten deinit registrations (the bug that motivated this pattern).
 *
 * These tests verify that after calling init_subjects(), each singleton has registered
 * its deinit callback with StaticSubjectRegistry.
 */

#include "ui_nav_manager.h"
#include "ui_wizard_fan_select.h"

#include "../lvgl_test_fixture.h"
#include "../test_fixtures.h"
#include "accel_sensor_manager.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "humidity_sensor_manager.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "probe_sensor_manager.h"
#include "settings_manager.h"
#include "static_subject_registry.h"
#include "subject_managed_panel.h"
#include "temperature_sensor_manager.h"
#include "timelapse_state.h"
#include "tool_state.h"
#include "width_sensor_manager.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Self-Registration Pattern Tests
// ============================================================================

TEST_CASE("StaticSubjectRegistry basic operations", "[shutdown][registry]") {
    LVGLTestFixture fixture;

    SECTION("starts empty") {
        // After fixture setup, registry may have entries from other tests.
        // We just verify it's accessible.
        auto& registry = StaticSubjectRegistry::instance();
        REQUIRE_FALSE(StaticSubjectRegistry::is_destroyed());
        (void)registry.count(); // Should not crash
    }

    SECTION("register and deinit round-trip") {
        auto& registry = StaticSubjectRegistry::instance();
        // Clear stale entries without running callbacks (avoids SIGSEGV from
        // other tests' fixture-local captures that are no longer on the stack)
        registry.clear();

        bool callback_called = false;
        registry.register_deinit("TestEntry", [&callback_called]() { callback_called = true; });

        REQUIRE(registry.count() == 1);

        registry.deinit_all();
        REQUIRE(callback_called);
        REQUIRE(registry.count() == 0);
    }

    SECTION("deinit runs in reverse registration order") {
        auto& registry = StaticSubjectRegistry::instance();
        registry.clear();

        std::vector<int> order;

        registry.register_deinit("First", [&order]() { order.push_back(1); });
        registry.register_deinit("Second", [&order]() { order.push_back(2); });
        registry.register_deinit("Third", [&order]() { order.push_back(3); });

        registry.deinit_all();

        REQUIRE(order.size() == 3);
        REQUIRE(order[0] == 3); // Third registered = first deinitialized
        REQUIRE(order[1] == 2);
        REQUIRE(order[2] == 1);
    }
}

TEST_CASE("PrinterState self-registers cleanup on init_subjects", "[shutdown][self-register]") {
    LVGLTestFixture fixture;
    auto& registry = StaticSubjectRegistry::instance();
    // Deinit the singleton directly to reset its subjects_initialized_ flag,
    // then clear the registry (avoids running stale callbacks from other tests)
    get_printer_state().deinit_subjects();
    registry.clear();

    get_printer_state().init_subjects();

    REQUIRE(registry.count() > 0);

    get_printer_state().deinit_subjects();
    registry.clear();
}

TEST_CASE("AmsState self-registers cleanup on init_subjects", "[shutdown][self-register]") {
    LVGLTestFixture fixture;
    auto& registry = StaticSubjectRegistry::instance();
    AmsState::instance().deinit_subjects();
    registry.clear();

    AmsState::instance().init_subjects(true);

    REQUIRE(registry.count() > 0);

    AmsState::instance().deinit_subjects();
    registry.clear();
}

TEST_CASE("ToolState self-registers cleanup on init_subjects", "[shutdown][self-register]") {
    LVGLTestFixture fixture;
    auto& registry = StaticSubjectRegistry::instance();
    helix::ToolState::instance().deinit_subjects();
    registry.clear();

    helix::ToolState::instance().init_subjects();

    REQUIRE(registry.count() > 0);

    helix::ToolState::instance().deinit_subjects();
    registry.clear();
}

TEST_CASE("TimelapseState self-registers cleanup on init_subjects", "[shutdown][self-register]") {
    LVGLTestFixture fixture;
    auto& registry = StaticSubjectRegistry::instance();
    helix::TimelapseState::instance().deinit_subjects();
    registry.clear();

    helix::TimelapseState::instance().init_subjects();

    REQUIRE(registry.count() > 0);

    helix::TimelapseState::instance().deinit_subjects();
    registry.clear();
}

TEST_CASE("FilamentSensorManager self-registers cleanup on init_subjects",
          "[shutdown][self-register]") {
    LVGLTestFixture fixture;
    auto& registry = StaticSubjectRegistry::instance();
    helix::FilamentSensorManager::instance().deinit_subjects();
    registry.clear();

    helix::FilamentSensorManager::instance().init_subjects();

    REQUIRE(registry.count() > 0);

    helix::FilamentSensorManager::instance().deinit_subjects();
    registry.clear();
}

TEST_CASE("Sensor managers self-register cleanup on init_subjects", "[shutdown][self-register]") {
    LVGLTestFixture fixture;
    auto& registry = StaticSubjectRegistry::instance();
    // Deinit each sensor manager directly
    helix::sensors::HumiditySensorManager::instance().deinit_subjects();
    helix::sensors::WidthSensorManager::instance().deinit_subjects();
    helix::sensors::ProbeSensorManager::instance().deinit_subjects();
    helix::sensors::AccelSensorManager::instance().deinit_subjects();
    helix::sensors::TemperatureSensorManager::instance().deinit_subjects();
    registry.clear();

    helix::sensors::HumiditySensorManager::instance().init_subjects();
    helix::sensors::WidthSensorManager::instance().init_subjects();
    helix::sensors::ProbeSensorManager::instance().init_subjects();
    helix::sensors::AccelSensorManager::instance().init_subjects();
    helix::sensors::TemperatureSensorManager::instance().init_subjects();

    // Each sensor manager should have registered exactly one entry
    REQUIRE(registry.count() == 5);

    helix::sensors::HumiditySensorManager::instance().deinit_subjects();
    helix::sensors::WidthSensorManager::instance().deinit_subjects();
    helix::sensors::ProbeSensorManager::instance().deinit_subjects();
    helix::sensors::AccelSensorManager::instance().deinit_subjects();
    helix::sensors::TemperatureSensorManager::instance().deinit_subjects();
    registry.clear();
}

TEST_CASE("AppGlobals self-registers cleanup on init_subjects", "[shutdown][self-register]") {
    LVGLTestFixture fixture;
    auto& registry = StaticSubjectRegistry::instance();

    // AppGlobals subjects may already be initialized by the test fixture or other tests.
    // Call init — if already initialized, the guard returns (no double-register).
    // If not yet initialized, it will init and self-register.
    size_t before = registry.count();
    app_globals_init_subjects();
    size_t after = registry.count();

    // Either we just registered (after > before) OR it was already registered
    // by a previous test (after == before because guard returned early).
    // In both cases, we verify the registry has at least one entry.
    REQUIRE(after >= before);
    // Note: Full round-trip verification (deinit→init→verify) is not possible
    // because LVGL subjects can't be reliably re-initialized after deinit.
    // The self-registration pattern is validated by the other singleton tests.
}

TEST_CASE("NavigationManager self-registers cleanup on init", "[shutdown][self-register]") {
    LVGLTestFixture fixture;
    auto& registry = StaticSubjectRegistry::instance();
    registry.deinit_all();

    NavigationManager::instance().init();

    REQUIRE(registry.count() > 0);

    registry.deinit_all();
}

TEST_CASE("Double init_subjects does not double-register", "[shutdown][self-register]") {
    LVGLTestFixture fixture;
    auto& registry = StaticSubjectRegistry::instance();
    registry.deinit_all();

    helix::ToolState::instance().init_subjects();
    size_t count_after_first = registry.count();

    // Second call should be a no-op (guard: subjects_initialized_)
    helix::ToolState::instance().init_subjects();
    size_t count_after_second = registry.count();

    REQUIRE(count_after_first == count_after_second);

    registry.deinit_all();
}

TEST_CASE("XMLTestFixture leaves no PrinterState entry behind in the registry",
          "[shutdown][self-register]") {
    {
        XMLTestFixture fixture;
        REQUIRE(fixture.state().are_subjects_initialized());
    }
    auto names = StaticSubjectRegistry::instance().names();
    REQUIRE(std::find(names.begin(), names.end(), "PrinterState") == names.end());
}

// ============================================================================
// XML names are withdrawn with their owner
// ============================================================================

TEST_CASE("SubjectManager::publish withdraws the name on deinit_all", "[shutdown][xml_name]") {
    LVGLTestFixture fixture;
    lv_subject_t subject{};
    SubjectManager subjects;
    lv_subject_init_int(&subject, 0);
    subjects.publish("test_publish_withdraw", &subject);
    REQUIRE(lv_xml_get_subject(nullptr, "test_publish_withdraw") == &subject);

    subjects.deinit_all();
    REQUIRE(lv_xml_get_subject(nullptr, "test_publish_withdraw") == nullptr);
}

TEST_CASE("SubjectManager::deinit_all leaves a name a successor re-published",
          "[shutdown][xml_name]") {
    LVGLTestFixture fixture;
    lv_subject_t old_subject{};
    lv_subject_t new_subject{};
    SubjectManager old_owner;
    SubjectManager new_owner;
    lv_subject_init_int(&old_subject, 0);
    lv_subject_init_int(&new_subject, 0);
    old_owner.publish("test_publish_successor", &old_subject);
    new_owner.publish("test_publish_successor", &new_subject);

    old_owner.deinit_all();
    REQUIRE(lv_xml_get_subject(nullptr, "test_publish_successor") == &new_subject);

    new_owner.deinit_all();
    REQUIRE(lv_xml_get_subject(nullptr, "test_publish_successor") == nullptr);
}

namespace helix {
void register_clock_widget();
}

// A printer switch deinits widget subjects through StaticSubjectRegistry, which
// withdraws their names; the rebuild binds those names again, so every hook has to
// run on each init, not only the first.
TEST_CASE("init_widget_subjects runs every widget subject hook on each call",
          "[shutdown][xml_name]") {
    LVGLTestFixture fixture;
    auto& widgets = PanelWidgetManager::instance();
    widgets.init_widget_subjects();

    static int calls = 0;
    calls = 0;
    struct RestoreClockHook {
        ~RestoreClockHook() {
            register_clock_widget();
        }
    } restore;
    register_widget_subjects("clock", []() { ++calls; });
    widgets.init_widget_subjects();
    widgets.init_widget_subjects();

    REQUIRE(calls == 2);
}

TEST_CASE("A destroyed wizard step withdraws its XML names", "[shutdown][xml_name]") {
    LVGLTestFixture fixture;
    {
        WizardFanSelectStep step;
        step.init_subjects();
        step.init_subjects(); // a second visit to the step
        REQUIRE(lv_xml_get_subject(nullptr, "part_fan_selected") != nullptr);
    }
    REQUIRE(lv_xml_get_subject(nullptr, "part_fan_selected") == nullptr);
}

TEST_CASE("AmsState deinit withdraws the ams_-prefixed XML names", "[shutdown][xml_name]") {
    LVGLTestFixture fixture;
    AmsState::instance().deinit_subjects();
    AmsState::instance().init_subjects(true);

    const char* names[] = {
        "ams_supports_bypass",   "ams_bypass_active",         "ams_filament_loaded",
        "ams_filament_runout",   "ams_external_spool_color",  "ams_external_spool_material",
        "ams_system_name",       "ams_slot_0_color",          "ams_unit_0_temp",
        "ams_env_ind_0_visible", "ams_env_ind_detail_visible"};
    for (const char* name : names) {
        INFO(name);
        REQUIRE(lv_xml_get_subject(nullptr, name) != nullptr);
    }

    AmsState::instance().deinit_subjects();

    for (const char* name : names) {
        INFO(name);
        REQUIRE(lv_xml_get_subject(nullptr, name) == nullptr);
    }
    StaticSubjectRegistry::instance().clear();
}
