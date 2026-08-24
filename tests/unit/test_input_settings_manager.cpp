// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_input_settings_manager.cpp
 * @brief InputSettingsManager: load-time defaults and clamps, setter clamps,
 *        persistence round-trips, and which settings are live vs restart-required.
 */

#include "../lvgl_test_fixture.h"
#include "app_constants.h"
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
    SECTION("scroll_throw defaults to 25") {
        REQUIRE(input().get_scroll_throw() == 25);
    }

    SECTION("scroll_limit defaults to 10") {
        REQUIRE(input().get_scroll_limit() == 10);
    }

    SECTION("jitter_threshold defaults to 5") {
        REQUIRE(input().get_jitter_threshold() == 5);
    }

    SECTION("scroll_guard defaults to off") {
        REQUIRE(input().get_scroll_guard() == false);
    }

    SECTION("debug_touches defaults to off, and RuntimeConfig agrees") {
        REQUIRE(input().get_debug_touches() == false);
        REQUIRE(RuntimeConfig::debug_touches() == false);
    }

    SECTION("restart_pending defaults to false") {
        REQUIRE(input().is_restart_pending() == false);
    }
}

// ============================================================================
// scroll_throw / scroll_limit
// ============================================================================

TEST_CASE_METHOD(InputSettingsFixture, "InputSettingsManager scroll_throw set/get",
                 "[input_settings]") {
    SECTION("set/get round trip") {
        input().set_scroll_throw(30);
        REQUIRE(input().get_scroll_throw() == 30);

        input().set_scroll_throw(5);
        REQUIRE(input().get_scroll_throw() == 5);

        input().set_scroll_throw(50);
        REQUIRE(input().get_scroll_throw() == 50);
    }

    SECTION("clamping below minimum") {
        input().set_scroll_throw(1);
        REQUIRE(input().get_scroll_throw() == 5);
    }

    SECTION("clamping above maximum") {
        input().set_scroll_throw(100);
        REQUIRE(input().get_scroll_throw() == 50);
    }
}

TEST_CASE_METHOD(InputSettingsFixture, "InputSettingsManager scroll_limit set/get",
                 "[input_settings]") {
    SECTION("set/get round trip") {
        input().set_scroll_limit(15);
        REQUIRE(input().get_scroll_limit() == 15);

        input().set_scroll_limit(1);
        REQUIRE(input().get_scroll_limit() == 1);

        input().set_scroll_limit(20);
        REQUIRE(input().get_scroll_limit() == 20);
    }

    SECTION("clamping below minimum") {
        input().set_scroll_limit(0);
        REQUIRE(input().get_scroll_limit() == 1);
    }

    SECTION("clamping above maximum") {
        input().set_scroll_limit(99);
        REQUIRE(input().get_scroll_limit() == 20);
    }
}

TEST_CASE_METHOD(InputSettingsFixture, "InputSettingsManager restart pending flag",
                 "[input_settings]") {
    SECTION("restart pending after scroll_throw change") {
        REQUIRE(input().is_restart_pending() == false);

        input().set_scroll_throw(30);
        REQUIRE(input().is_restart_pending() == true);
    }

    SECTION("restart pending after scroll_limit change") {
        REQUIRE(input().is_restart_pending() == false);

        input().set_scroll_limit(15);
        REQUIRE(input().is_restart_pending() == true);
    }

    SECTION("clear_restart_pending resets flag") {
        input().set_scroll_throw(30);
        REQUIRE(input().is_restart_pending() == true);

        input().clear_restart_pending();
        REQUIRE(input().is_restart_pending() == false);
    }
}

TEST_CASE_METHOD(InputSettingsFixture, "InputSettingsManager subject values match getters",
                 "[input_settings]") {
    SECTION("scroll_throw subject reflects setter") {
        input().set_scroll_throw(35);
        REQUIRE(lv_subject_get_int(input().subject_scroll_throw()) == 35);
    }

    SECTION("scroll_limit subject reflects setter") {
        input().set_scroll_limit(8);
        REQUIRE(lv_subject_get_int(input().subject_scroll_limit()) == 8);
    }
}

// ============================================================================
// debug_touches / jitter_threshold / scroll_guard
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

    SECTION("subject reflects setter") {
        input().set_debug_touches(true);
        REQUIRE(lv_subject_get_int(input().subject_debug_touches()) == 1);
        input().set_debug_touches(false);
        REQUIRE(lv_subject_get_int(input().subject_debug_touches()) == 0);
    }

    SECTION("a persisted true is live-applied at load, not just on the setter") {
        input().set_debug_touches(true);
        RuntimeConfig::set_debug_touches(false); // simulate a fresh process
        restart();
        REQUIRE(RuntimeConfig::debug_touches() == true);
    }
}

TEST_CASE_METHOD(InputSettingsFixture, "InputSettingsManager jitter_threshold clamps and persists",
                 "[input_settings]") {
    SECTION("values above 30 clamp to 30") {
        input().set_jitter_threshold(50);
        REQUIRE(input().get_jitter_threshold() == 30);
    }

    SECTION("values below 0 clamp to 0") {
        input().set_jitter_threshold(-5);
        REQUIRE(input().get_jitter_threshold() == 0);
    }

    SECTION("in-range round trip") {
        input().set_jitter_threshold(15);
        REQUIRE(input().get_jitter_threshold() == 15);
    }

    SECTION("marks restart_pending") {
        input().set_jitter_threshold(20);
        REQUIRE(input().is_restart_pending() == true);
    }
}

TEST_CASE_METHOD(InputSettingsFixture,
                 "InputSettingsManager scroll_guard persists and is restart-required",
                 "[input_settings]") {
    SECTION("round trip true/false") {
        input().set_scroll_guard(true);
        REQUIRE(input().get_scroll_guard() == true);
        input().set_scroll_guard(false);
        REQUIRE(input().get_scroll_guard() == false);
    }

    SECTION("subject reflects setter") {
        input().set_scroll_guard(true);
        REQUIRE(lv_subject_get_int(input().subject_scroll_guard()) == 1);
    }

    SECTION("marks restart_pending") {
        input().set_scroll_guard(true);
        REQUIRE(input().is_restart_pending() == true);
    }
}

// ============================================================================
// long_press_time (#1245) — the global hold threshold behind the Touch & Input
// slider. Governs every long-press in the app, so the clamps matter: a value
// outside 300-1500 either makes edit mode unreachable or fires it instantly.
// ============================================================================

TEST_CASE_METHOD(InputSettingsFixture, "InputSettingsManager long_press_time defaults and clamps",
                 "[input_settings][long_press][1245]") {
    SECTION("defaults to the documented 500ms") {
        // Pin both the constant and the wiring: a change to either is a change
        // to how easily a resting finger opens edit mode.
        CHECK(static_cast<int>(AppConstants::Input::LONG_PRESS_MS) == 500);
        CHECK(input().get_long_press_time() == 500);
        CHECK(input().get_long_press_time() ==
              static_cast<int>(AppConstants::Input::LONG_PRESS_MS));
    }

    SECTION("setter clamps below the 300ms minimum") {
        input().set_long_press_time(100);
        CHECK(input().get_long_press_time() == 300);

        input().set_long_press_time(299);
        CHECK(input().get_long_press_time() == 300);
    }

    SECTION("setter clamps above the 1500ms maximum") {
        input().set_long_press_time(5000);
        CHECK(input().get_long_press_time() == 1500);

        input().set_long_press_time(1501);
        CHECK(input().get_long_press_time() == 1500);
    }

    SECTION("in-range values pass through untouched, bounds included") {
        input().set_long_press_time(300);
        CHECK(input().get_long_press_time() == 300);

        input().set_long_press_time(900);
        CHECK(input().get_long_press_time() == 900);

        input().set_long_press_time(1500);
        CHECK(input().get_long_press_time() == 1500);
    }

    SECTION("subject reflects the clamped value, not the raw one") {
        // The XML slider binds the subject, so an unclamped subject would show
        // the user a value the manager is not actually using.
        input().set_long_press_time(9999);
        CHECK(lv_subject_get_int(input().subject_long_press_time()) == 1500);

        input().set_long_press_time(650);
        CHECK(lv_subject_get_int(input().subject_long_press_time()) == 650);
    }

    SECTION("live-applied: no restart is demanded") {
        input().set_long_press_time(1200);
        CHECK(input().is_restart_pending() == false);
    }
}

TEST_CASE_METHOD(InputSettingsFixture, "InputSettingsManager long_press_time persistence",
                 "[input_settings][long_press][1245]") {
    Config* config = Config::get_instance();
    REQUIRE(config != nullptr);

    SECTION("the setter writes Config, and the next load reads it back") {
        input().set_long_press_time(750);
        CHECK(config->get<int>("/input/long_press_time", -1) == 750);

        restart();
        CHECK(input().get_long_press_time() == 750);
    }

    SECTION("what is persisted is the clamped value") {
        input().set_long_press_time(20000);
        CHECK(config->get<int>("/input/long_press_time", -1) == 1500);
    }

    SECTION("a hand-edited value above the range is clamped at load") {
        // Distinct code path from the setter: input_settings_manager.cpp:50
        // clamps what comes off disk, so a stale or hand-edited settings.json
        // cannot install an unreachable hold time.
        config->set<int>("/input/long_press_time", 99999);
        restart();
        CHECK(input().get_long_press_time() == 1500);
    }

    SECTION("a hand-edited value below the range is clamped at load") {
        config->set<int>("/input/long_press_time", 1);
        restart();
        CHECK(input().get_long_press_time() == 300);
    }

    SECTION("a null value falls back to the default rather than zero") {
        config->set<json>("/input/long_press_time", json());
        restart();
        CHECK(input().get_long_press_time() == 500);
    }

    SECTION("a missing key falls back to the default") {
        helix_test::forget_input_settings();
        REQUIRE_FALSE(config->exists("/input/long_press_time"));
        restart();
        CHECK(input().get_long_press_time() == 500);
    }
}

// ============================================================================
// home_edit_mode_enabled (#1245) — the kill switch. Its effect on the long-press
// handler is covered in test_home_edit_mode_kill_switch.cpp; this pins the
// persistence and default behaviour that feeds it.
// ============================================================================

TEST_CASE_METHOD(InputSettingsFixture, "InputSettingsManager home_edit_mode_enabled persistence",
                 "[input_settings][edit_mode][1245]") {
    Config* config = Config::get_instance();
    REQUIRE(config != nullptr);

    SECTION("defaults to enabled on a fresh install") {
        REQUIRE_FALSE(config->exists("/input/home_edit_mode_enabled"));
        CHECK(input().get_home_edit_mode_enabled() == true);
        CHECK(lv_subject_get_int(input().subject_home_edit_mode_enabled()) == 1);
    }

    SECTION("disabling round-trips through Config and survives a restart") {
        input().set_home_edit_mode_enabled(false);
        CHECK(input().get_home_edit_mode_enabled() == false);
        CHECK(config->get<bool>("/input/home_edit_mode_enabled", true) == false);

        restart();
        CHECK(input().get_home_edit_mode_enabled() == false);
    }

    SECTION("re-enabling round-trips too — the off state is not sticky") {
        input().set_home_edit_mode_enabled(false);
        restart();
        REQUIRE(input().get_home_edit_mode_enabled() == false);

        input().set_home_edit_mode_enabled(true);
        CHECK(config->get<bool>("/input/home_edit_mode_enabled", false) == true);

        restart();
        CHECK(input().get_home_edit_mode_enabled() == true);
    }

    SECTION("subject tracks the setter in both directions") {
        input().set_home_edit_mode_enabled(false);
        CHECK(lv_subject_get_int(input().subject_home_edit_mode_enabled()) == 0);
        input().set_home_edit_mode_enabled(true);
        CHECK(lv_subject_get_int(input().subject_home_edit_mode_enabled()) == 1);
    }

    SECTION("a null value falls back to enabled, not to a zeroed false") {
        // The failure mode this guards: an unset/unreadable key reading as
        // false would suppress every long-press with no way for the user to
        // tell why edit mode stopped working.
        config->set<json>("/input/home_edit_mode_enabled", json());
        restart();
        CHECK(input().get_home_edit_mode_enabled() == true);
    }

    SECTION("live-applied: no restart is demanded") {
        input().set_home_edit_mode_enabled(false);
        CHECK(input().is_restart_pending() == false);
    }
}
