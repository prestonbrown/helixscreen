// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_input_settings_manager.cpp
 * @brief InputSettingsManager behaviour the persisted-settings table cannot express:
 *        reaching LVGL pointers, live vs restart-required settings, debug_touches'
 *        RuntimeConfig mirror, and null values on disk. Defaults, clamps and
 *        round-trips for every input key live in test_persisted_setting.cpp.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_pointer_indev.h"
#include "config.h"
#include "input_settings_manager.h"
#include "input_settings_test_helpers.h"
#include "runtime_config.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/**
 * @brief Gives every case a manager freshly loaded from an empty /input node.
 *
 * Without the teardown-then-init, init_subjects() early-returns whenever a
 * co-tenant test (or the SettingsManager cascade) already initialized the
 * singleton, and every "defaults to X" assertion below silently reads that other
 * test's value instead. Wiping /input first is the other half: the setters
 * persist, so one case's set_scroll_throw(30) would otherwise become the next
 * case's "default".
 *
 * Teardown restores the same clean state rather than leaving the manager
 * deinitialized — see reload_input_settings() for why a torn-down manager is a
 * hazard for later tests that build the Touch & Input overlay.
 */
class InputSettingsFixture : public LVGLTestFixture {
  public:
    InputSettingsFixture() {
        helix_test::reset_input_settings_to_defaults();
    }
    ~InputSettingsFixture() override {
        // Also puts RuntimeConfig::debug_touches() back to false: init_subjects()
        // live-applies whatever the (now empty) /input node says.
        helix_test::reset_input_settings_to_defaults();
    }

    static InputSettingsManager& input() {
        return InputSettingsManager::instance();
    }

    /// Re-read Config without wiping it — the seam a persistence test needs,
    /// standing in for the next app start.
    static void restart() {
        helix_test::reload_input_settings();
    }
};

} // namespace

// ============================================================================
// Defaults
// ============================================================================

TEST_CASE_METHOD(InputSettingsFixture, "InputSettingsManager default values after init",
                 "[input_settings]") {
    SECTION("debug_touches defaults to off, and RuntimeConfig agrees") {
        REQUIRE(input().get_debug_touches() == false);
        REQUIRE(RuntimeConfig::debug_touches() == false);
    }

    SECTION("restart_pending defaults to false") {
        REQUIRE(input().is_restart_pending() == false);
    }
}

// ============================================================================
// Live-applied vs restart-required
// ============================================================================

TEST_CASE_METHOD(InputSettingsFixture, "InputSettingsManager restart pending flag",
                 "[input_settings]") {
    SECTION("live-applied settings demand no restart") {
        input().set_scroll_throw(30);
        input().set_scroll_limit(15);
        input().set_long_press_time(1200);
        input().set_home_edit_mode_enabled(false);
        REQUIRE(input().is_restart_pending() == false);
    }

    SECTION("scroll_guard is restart-required, and clear_restart_pending resets the flag") {
        input().set_scroll_guard(true);
        REQUIRE(input().is_restart_pending() == true);

        input().clear_restart_pending();
        REQUIRE(input().is_restart_pending() == false);
    }
}

TEST_CASE_METHOD(InputSettingsFixture,
                 "InputSettingsManager applies scroll and long-press to pointers",
                 "[input_settings][scroll]") {
    // The firmware creates its touch indev outside DisplayManager, so the
    // manager itself has to reach every pointer, at load and on each setter.
    helix_test::ScopedPointerIndev pointer;
    lv_indev_t* indev = pointer.indev();

    SECTION("loading applies the persisted values to an existing pointer") {
        Config::get_instance()->set<int>("/input/scroll_throw", 40);
        Config::get_instance()->set<int>("/input/scroll_limit", 7);
        Config::get_instance()->set<int>("/input/long_press_time", 800);
        restart();
        CHECK(indev->scroll_throw == 40);
        CHECK(indev->scroll_limit == 7);
        CHECK(indev->long_press_time == 800);
    }

    SECTION("setters reach the live pointer") {
        input().set_scroll_throw(45);
        input().set_scroll_limit(3);
        input().set_long_press_time(1000);
        CHECK(indev->scroll_throw == 45);
        CHECK(indev->scroll_limit == 3);
        CHECK(indev->long_press_time == 1000);
    }

    SECTION("the clamped value is what the pointer gets") {
        input().set_scroll_throw(99);
        CHECK(indev->scroll_throw == 50);
    }

    SECTION("a pointer created after the settings applied gets them on request") {
        input().set_long_press_time(1000);
        input().set_scroll_limit(3);
        helix_test::ScopedPointerIndev late;
        REQUIRE(late.indev()->long_press_time != 1000);
        input().apply_to_pointer(late.indev());
        CHECK(late.indev()->long_press_time == 1000);
        CHECK(late.indev()->scroll_limit == 3);
    }
}

// ============================================================================
// debug_touches
// ============================================================================

TEST_CASE_METHOD(InputSettingsFixture, "InputSettingsManager debug_touches live-apply contract",
                 "[input_settings]") {
    SECTION("set_debug_touches(true) flips RuntimeConfig immediately") {
        input().set_debug_touches(true);
        REQUIRE(RuntimeConfig::debug_touches() == true);
        REQUIRE(input().get_debug_touches() == true);
    }

    SECTION("set_debug_touches does NOT mark restart_pending") {
        // Live-apply is the whole point — restart prompt would be wrong here.
        input().set_debug_touches(true);
        REQUIRE(input().is_restart_pending() == false);
    }

    SECTION("a persisted true is live-applied at load, not just on the setter") {
        input().set_debug_touches(true);
        RuntimeConfig::set_debug_touches(false); // simulate a fresh process
        restart();
        REQUIRE(RuntimeConfig::debug_touches() == true);
    }
}

// ============================================================================
// Null values on disk
// ============================================================================

TEST_CASE_METHOD(InputSettingsFixture, "InputSettingsManager null values load as the default",
                 "[input_settings][long_press][edit_mode][1245]") {
    Config* config = Config::get_instance();

    SECTION("a null long_press_time falls back to the default rather than zero") {
        config->set<json>("/input/long_press_time", json());
        restart();
        CHECK(input().get_long_press_time() == 500);
    }

    SECTION("a null home_edit_mode_enabled falls back to enabled, not to a zeroed false") {
        // An unset/unreadable key reading as false would suppress every
        // long-press with no way for the user to tell why edit mode stopped
        // working.
        config->set<json>("/input/home_edit_mode_enabled", json());
        restart();
        CHECK(input().get_home_edit_mode_enabled() == true);
    }
}
