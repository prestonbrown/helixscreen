// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_settings_subject_restoration.cpp
 * @brief HelixTestFixture must undo a settings sub-manager teardown.
 *
 * SubjectManager::deinit_all() (include/subject_managed_panel.h) withdraws each
 * subject's XML-scope name as it frees the subject — correct, and the whole
 * reason a torn-down owner cannot leave a name resolving to dead memory. What
 * makes it a cross-test hazard is the aggregate:
 * SettingsManager::init_subjects() is the only thing that ever calls
 * AudioSettingsManager / SafetySettingsManager init_subjects() in production
 * (ui_panel_settings.cpp:313), and it early-returns on its OWN
 * subjects_initialized_ flag, which a sub-manager teardown does not clear. So
 * one test calling AudioSettingsManager::instance().deinit_subjects() withdraws
 * settings_sounds_enabled & co. for the remaining lifetime of the binary, and
 * every later attempt to rebuild them is a silent no-op.
 *
 * The damage is not a crash. settings_display_sound_overlay.xml and
 * settings_safety_overlay.xml still parse; their toggles just bind to nothing
 * and log "No subject was found". A test that builds either one fails on a
 * value that looks like a broken binding, dozens of test cases downstream of
 * the teardown that actually caused it.
 *
 * These tests use raw fixture instances rather than TEST_CASE_METHOD because
 * the thing under test IS the fixture's ctor/dtor, so their boundaries have to
 * be inside the test body.
 */

#include "ui_notification_threshold.h"

#include "../lvgl_test_fixture.h"
#include "audio_settings_manager.h"
#include "config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"

#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

// Every name AudioSettingsManager::init_subjects() publishes.
const std::vector<const char*> AUDIO_SUBJECTS{
    "settings_sounds_enabled", "settings_ui_sounds_enabled", "settings_volume",
    "settings_completion_alert", "settings_audio_device_available"};

// Every name SafetySettingsManager::init_subjects() publishes.
const std::vector<const char*> SAFETY_SUBJECTS{"settings_estop_confirm",
                                               "settings_cancel_escalation_enabled",
                                               "settings_cancel_escalation_timeout",
                                               "settings_macro_confirm",
                                               "settings_allow_cold_extrude",
                                               "settings_min_toast_severity"};

void require_all_resolve(const std::vector<const char*>& names) {
    for (const char* name : names) {
        INFO("XML subject name: " << name);
        REQUIRE(lv_xml_get_subject(nullptr, name) != nullptr);
    }
}

void require_none_resolve(const std::vector<const char*>& names) {
    for (const char* name : names) {
        INFO("XML subject name: " << name);
        REQUIRE(lv_xml_get_subject(nullptr, name) == nullptr);
    }
}

void check_all_resolve(const std::vector<const char*>& names) {
    for (const char* name : names) {
        INFO("XML subject name: " << name);
        CHECK(lv_xml_get_subject(nullptr, name) != nullptr);
    }
}

} // namespace

TEST_CASE("Fixture teardown restores settings subject names a test withdrew",
          "[settings][test_isolation]") {
    // Stand-in for a test that tears a sub-manager down inside its body
    // (test_audio_settings_manager.cpp) or in a derived fixture's destructor
    // (AbortManagerTestFixture, test_abort_manager.cpp:197 — the base
    // destructor runs after the derived one, so both are covered here).
    {
        LVGLTestFixture polluter;

        helix::SettingsManager::instance().init_subjects();
        require_all_resolve(AUDIO_SUBJECTS);
        require_all_resolve(SAFETY_SUBJECTS);

        helix::AudioSettingsManager::instance().deinit_subjects();
        helix::SafetySettingsManager::instance().deinit_subjects();
        require_none_resolve(AUDIO_SUBJECTS);
        require_none_resolve(SAFETY_SUBJECTS);

        // The precondition that makes the fixture the only place this can be
        // repaired: the aggregate init is latched, so calling it — exactly what
        // SettingsPanel::init_subjects() does before building either overlay —
        // cannot bring the names back. If SettingsManager ever learns to clear
        // its flag when a sub-manager is torn down, this is where that shows up.
        helix::SettingsManager::instance().init_subjects();
        require_none_resolve(AUDIO_SUBJECTS);
        require_none_resolve(SAFETY_SUBJECTS);
    } // ~HelixTestFixture -> reset_all() must put the names back

    {
        LVGLTestFixture next_test;

        // What a panel/overlay test does on the way in. Still a no-op — the
        // fixture, not this call, is what has to have restored the names.
        helix::SettingsManager::instance().init_subjects();

        check_all_resolve(AUDIO_SUBJECTS);
        check_all_resolve(SAFETY_SUBJECTS);
    }
}

TEST_CASE("Fixture construction restores settings subject names withdrawn between tests",
          "[settings][test_isolation]") {
    {
        LVGLTestFixture setup;
        helix::SettingsManager::instance().init_subjects();
        require_all_resolve(AUDIO_SUBJECTS);
        require_all_resolve(SAFETY_SUBJECTS);
    }

    // No fixture alive. LVGL stays initialized for the whole binary
    // (LVGLTestFixture::ensure_lvgl_initialized is std::call_once and nothing
    // calls lv_deinit), so a teardown here really does withdraw the names
    // rather than hitting SubjectManager's !lv_is_initialized() shortcut.
    helix::AudioSettingsManager::instance().deinit_subjects();
    helix::SafetySettingsManager::instance().deinit_subjects();
    require_none_resolve(AUDIO_SUBJECTS);
    require_none_resolve(SAFETY_SUBJECTS);

    {
        LVGLTestFixture next_test; // ctor -> reset_all()
        check_all_resolve(AUDIO_SUBJECTS);
        check_all_resolve(SAFETY_SUBJECTS);
    }
}

TEST_CASE("Restored settings subjects come back on defaults, not the previous test's values",
          "[settings][test_isolation]") {
    // The restore re-reads Config, and reset_all() clears Config first, so the
    // rebuilt subjects must hold the compiled-in defaults. Without that, a test
    // that raises the volume or quietens the toast gate hands its value to
    // every later test through a process-wide singleton.
    {
        LVGLTestFixture polluter;

        // Direct, not through SettingsManager: the aggregate is latched by now,
        // so this is the only call that reliably leaves both managers live to
        // be written to.
        helix::AudioSettingsManager::instance().init_subjects();
        helix::SafetySettingsManager::instance().init_subjects();

        helix::AudioSettingsManager::instance().set_volume(13);
        helix::AudioSettingsManager::instance().set_sounds_enabled(true);
        helix::SafetySettingsManager::instance().set_min_toast_severity(2);
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(true);

        REQUIRE(helix::AudioSettingsManager::instance().get_volume() == 13);
        REQUIRE(helix::SafetySettingsManager::instance().get_min_toast_severity() == 2);

        helix::AudioSettingsManager::instance().deinit_subjects();
        helix::SafetySettingsManager::instance().deinit_subjects();
    }

    {
        LVGLTestFixture next_test;

        CHECK(helix::AudioSettingsManager::instance().get_volume() == 80);
        CHECK(helix::AudioSettingsManager::instance().get_sounds_enabled() == false);
        CHECK(helix::AudioSettingsManager::instance().get_ui_sounds_enabled() == true);
        CHECK(helix::SafetySettingsManager::instance().get_min_toast_severity() == 0);
        CHECK(helix::SafetySettingsManager::instance().get_allow_cold_extrude() == false);
        // set_min_toast_severity() pushes an inline cache the toast sites read
        // directly; the restore has to walk it back too, or notifications stay
        // suppressed for every later test in the binary.
        CHECK(helix::ui::notifications::get_min_toast_severity_cache() == 0);
    }
}
