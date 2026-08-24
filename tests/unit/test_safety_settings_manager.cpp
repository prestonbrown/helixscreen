// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_notification_threshold.h"

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "safety_settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// SafetySettingsManager Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "SafetySettingsManager default values after init",
                 "[safety_settings]") {
    Config::get_instance();
    // Reset config keys to defaults so prior test persistence doesn't contaminate
    Config::get_instance()->set<bool>("/safety/estop_require_confirmation", true);
    Config::get_instance()->set<bool>("/safety/cancel_escalation_enabled", false);
    Config::get_instance()->set<int>("/safety/cancel_escalation_timeout_seconds", 30);
    Config::get_instance()->set<bool>("/safety/allow_cold_extrude", false);
    Config::get_instance()->set<int>("/notifications/min_toast_severity", 0);
    SafetySettingsManager::instance().deinit_subjects();
    SafetySettingsManager::instance().init_subjects();

    SECTION("estop_require_confirmation defaults to true") {
        REQUIRE(SafetySettingsManager::instance().get_estop_require_confirmation() == true);
    }

    SECTION("cancel_escalation_enabled defaults to false") {
        REQUIRE(SafetySettingsManager::instance().get_cancel_escalation_enabled() == false);
    }

    SECTION("cancel_escalation_timeout defaults to 30s") {
        REQUIRE(SafetySettingsManager::instance().get_cancel_escalation_timeout_seconds() == 30);
    }

    SECTION("allow_cold_extrude defaults to false") {
        // Gating filament load/unload on min_extrude_temp is the safe default;
        // bypassing it is opt-in for users whose macros heat the nozzle (#978).
        REQUIRE(SafetySettingsManager::instance().get_allow_cold_extrude() == false);
    }

    SECTION("min_toast_severity defaults to 0 (All) - no change for existing users") {
        // #1213: the gate must not suppress anything out of the box. Defaults to
        // the "All" rung so existing toast behaviour is unchanged until the user
        // opts in to a quieter setting.
        REQUIRE(SafetySettingsManager::instance().get_min_toast_severity() == 0);
        REQUIRE(helix::ui::notifications::get_min_toast_severity_cache() == 0);
    }

    SafetySettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "SafetySettingsManager set/get round trips",
                 "[safety_settings]") {
    Config::get_instance();
    SafetySettingsManager::instance().init_subjects();

    SECTION("estop_require_confirmation set/get") {
        SafetySettingsManager::instance().set_estop_require_confirmation(true);
        REQUIRE(SafetySettingsManager::instance().get_estop_require_confirmation() == true);

        SafetySettingsManager::instance().set_estop_require_confirmation(false);
        REQUIRE(SafetySettingsManager::instance().get_estop_require_confirmation() == false);
    }

    SECTION("cancel_escalation_enabled set/get") {
        SafetySettingsManager::instance().set_cancel_escalation_enabled(true);
        REQUIRE(SafetySettingsManager::instance().get_cancel_escalation_enabled() == true);

        SafetySettingsManager::instance().set_cancel_escalation_enabled(false);
        REQUIRE(SafetySettingsManager::instance().get_cancel_escalation_enabled() == false);
    }

    SECTION("cancel_escalation_timeout set/get with valid values") {
        SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(15);
        REQUIRE(SafetySettingsManager::instance().get_cancel_escalation_timeout_seconds() == 15);

        SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(30);
        REQUIRE(SafetySettingsManager::instance().get_cancel_escalation_timeout_seconds() == 30);

        SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(60);
        REQUIRE(SafetySettingsManager::instance().get_cancel_escalation_timeout_seconds() == 60);

        SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(120);
        REQUIRE(SafetySettingsManager::instance().get_cancel_escalation_timeout_seconds() == 120);
    }

    SECTION("allow_cold_extrude set/get") {
        SafetySettingsManager::instance().set_allow_cold_extrude(true);
        REQUIRE(SafetySettingsManager::instance().get_allow_cold_extrude() == true);

        SafetySettingsManager::instance().set_allow_cold_extrude(false);
        REQUIRE(SafetySettingsManager::instance().get_allow_cold_extrude() == false);
    }

    SECTION("min_toast_severity round-trips and pushes to the toast gate cache") {
        // #1213: each valid index sets the subject, persists, and updates the
        // header-only cache the toast sites read. Out-of-range clamps to 0.
        SafetySettingsManager::instance().set_min_toast_severity(2);
        REQUIRE(SafetySettingsManager::instance().get_min_toast_severity() == 2);
        REQUIRE(helix::ui::notifications::get_min_toast_severity_cache() == 2);

        SafetySettingsManager::instance().set_min_toast_severity(1);
        REQUIRE(SafetySettingsManager::instance().get_min_toast_severity() == 1);
        REQUIRE(helix::ui::notifications::get_min_toast_severity_cache() == 1);

        SafetySettingsManager::instance().set_min_toast_severity(0);
        REQUIRE(SafetySettingsManager::instance().get_min_toast_severity() == 0);
        REQUIRE(helix::ui::notifications::get_min_toast_severity_cache() == 0);

        // A corrupt value clamps to "All" rather than silently suppressing.
        SafetySettingsManager::instance().set_min_toast_severity(7);
        REQUIRE(SafetySettingsManager::instance().get_min_toast_severity() == 0);
        REQUIRE(helix::ui::notifications::get_min_toast_severity_cache() == 0);
    }

    SECTION("cancel_escalation_timeout snaps to bucket by threshold") {
        // Bucket logic: <=15->15, <=30->30, <=60->60, >60->120
        SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(10);
        REQUIRE(SafetySettingsManager::instance().get_cancel_escalation_timeout_seconds() == 15);

        SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(20);
        REQUIRE(SafetySettingsManager::instance().get_cancel_escalation_timeout_seconds() == 30);

        SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(45);
        REQUIRE(SafetySettingsManager::instance().get_cancel_escalation_timeout_seconds() == 60);

        SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(90);
        REQUIRE(SafetySettingsManager::instance().get_cancel_escalation_timeout_seconds() == 120);

        SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(200);
        REQUIRE(SafetySettingsManager::instance().get_cancel_escalation_timeout_seconds() == 120);
    }

    SafetySettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "SafetySettingsManager subject values match getters",
                 "[safety_settings]") {
    Config::get_instance();
    SafetySettingsManager::instance().init_subjects();

    SECTION("estop subject reflects setter") {
        SafetySettingsManager::instance().set_estop_require_confirmation(true);
        REQUIRE(lv_subject_get_int(
                    SafetySettingsManager::instance().subject_estop_require_confirmation()) == 1);

        SafetySettingsManager::instance().set_estop_require_confirmation(false);
        REQUIRE(lv_subject_get_int(
                    SafetySettingsManager::instance().subject_estop_require_confirmation()) == 0);
    }

    SECTION("cancel_escalation_enabled subject reflects setter") {
        SafetySettingsManager::instance().set_cancel_escalation_enabled(true);
        REQUIRE(lv_subject_get_int(
                    SafetySettingsManager::instance().subject_cancel_escalation_enabled()) == 1);
    }

    SECTION("cancel_escalation_timeout subject is dropdown index") {
        // Subject stores dropdown index (0-3), not seconds
        SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(60);
        // 60s -> index 2
        REQUIRE(lv_subject_get_int(
                    SafetySettingsManager::instance().subject_cancel_escalation_timeout()) == 2);
    }

    SECTION("allow_cold_extrude subject reflects setter") {
        SafetySettingsManager::instance().set_allow_cold_extrude(true);
        REQUIRE(lv_subject_get_int(
                    SafetySettingsManager::instance().subject_allow_cold_extrude()) == 1);

        SafetySettingsManager::instance().set_allow_cold_extrude(false);
        REQUIRE(lv_subject_get_int(
                    SafetySettingsManager::instance().subject_allow_cold_extrude()) == 0);
    }

    SafetySettingsManager::instance().deinit_subjects();
}

// Backward compat test removed: forwarding wrappers in SettingsManager have been eliminated.
// All consumers now use SafetySettingsManager directly.
