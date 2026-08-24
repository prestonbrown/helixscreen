// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_rotation_setting.cpp
 * @brief Screen rotation setting: persistence, validation, and the settings row
 *
 * A K1C shipped with its panel sideways (debug bundle AXSKJ3GH): the first-boot
 * probe registered no taps, saved 0, and set /display/rotation_probed so it
 * could never offer itself again. There was no UI for /display/rotate, so the
 * only fix was editing settings.json over SSH. These tests cover the control
 * that replaces that.
 */

#include "ui_settings_display_sound.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "config.h"
#include "display_settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Index <-> degrees mapping
// ============================================================================

TEST_CASE("DisplayRotation: dropdown index maps to degrees", "[display_settings][rotation]") {
    REQUIRE(DisplaySettingsManager::index_to_rotation_degrees(0) == 0);
    REQUIRE(DisplaySettingsManager::index_to_rotation_degrees(1) == 90);
    REQUIRE(DisplaySettingsManager::index_to_rotation_degrees(2) == 180);
    REQUIRE(DisplaySettingsManager::index_to_rotation_degrees(3) == 270);

    // Out of range falls back to unrotated rather than indexing off the end.
    REQUIRE(DisplaySettingsManager::index_to_rotation_degrees(-1) == 0);
    REQUIRE(DisplaySettingsManager::index_to_rotation_degrees(4) == 0);
    REQUIRE(DisplaySettingsManager::index_to_rotation_degrees(9999) == 0);

    REQUIRE(DisplaySettingsManager::rotation_degrees_to_index(0) == 0);
    REQUIRE(DisplaySettingsManager::rotation_degrees_to_index(90) == 1);
    REQUIRE(DisplaySettingsManager::rotation_degrees_to_index(180) == 2);
    REQUIRE(DisplaySettingsManager::rotation_degrees_to_index(270) == 3);

    // A config value the startup path would not apply selects "Normal", so the
    // dropdown agrees with what the screen is actually doing.
    REQUIRE(DisplaySettingsManager::rotation_degrees_to_index(45) == 0);
    REQUIRE(DisplaySettingsManager::rotation_degrees_to_index(-90) == 0);
    REQUIRE(DisplaySettingsManager::rotation_degrees_to_index(360) == 0);
}

// ============================================================================
// Persistence
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "DisplayRotation: each value persists to /display/rotate",
                 "[display_settings][rotation]") {
    Config* config = Config::get_instance();

    for (int degrees : {90, 180, 270, 0}) {
        INFO("degrees=" << degrees);
        DisplaySettingsManager::instance().set_display_rotation(degrees);
        REQUIRE(config->get<int>("/display/rotate", -1) == degrees);
        REQUIRE(DisplaySettingsManager::instance().get_display_rotation() == degrees);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "DisplayRotation: an explicit choice pins the first-boot probe",
                 "[display_settings][rotation]") {
    Config* config = Config::get_instance();
    config->set<bool>("/display/rotation_probed", false);

    DisplaySettingsManager::instance().set_display_rotation(270);

    // Application::run_rotation_probe_and_layout() probes only when
    // rotation_probed is false AND /display/rotate is absent. Both halves of
    // that gate must be closed, or the probe can run again and overwrite the
    // user's choice with a tap it failed to register.
    REQUIRE(config->get<bool>("/display/rotation_probed", false) == true);
    REQUIRE(config->exists("/display/rotate"));
}

TEST_CASE_METHOD(LVGLTestFixture, "DisplayRotation: probe is pinned even when choosing 0",
                 "[display_settings][rotation]") {
    Config* config = Config::get_instance();
    config->set<bool>("/display/rotation_probed", false);

    // 0 is a real answer, not "unset" - a user whose probe wrongly saved 90 must
    // be able to say "no rotation" and have it stick.
    DisplaySettingsManager::instance().set_display_rotation(0);

    REQUIRE(config->get<int>("/display/rotate", -1) == 0);
    REQUIRE(config->get<bool>("/display/rotation_probed", false) == true);
}

// ============================================================================
// Validation and no-op handling
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "DisplayRotation: out-of-range values are rejected, not stored",
                 "[display_settings][rotation]") {
    Config* config = Config::get_instance();
    DisplaySettingsManager::instance().set_display_rotation(180);

    for (int bad : {45, -90, 360, 1, 271}) {
        INFO("bad=" << bad);
        REQUIRE(DisplaySettingsManager::instance().set_display_rotation(bad) == false);
        // The previously saved good value survives.
        REQUIRE(config->get<int>("/display/rotate", -1) == 180);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "DisplayRotation: a corrupt stored value reads back as 0",
                 "[display_settings][rotation]") {
    Config* config = Config::get_instance();

    // Hand-edited settings.json. DisplayManager only applies 90/180/270, so the
    // getter must report what the screen is really doing, not the garbage.
    for (int bad : {45, -90, 360, 12345}) {
        INFO("bad=" << bad);
        config->set<int>("/display/rotate", bad);
        REQUIRE(DisplaySettingsManager::instance().get_display_rotation() == 0);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "DisplayRotation: restart is signalled only on a real change",
                 "[display_settings][rotation]") {
    DisplaySettingsManager::instance().set_display_rotation(0);

    // The return value is what drives the restart prompt in
    // DisplaySoundSettingsOverlay::handle_display_rotation_changed().
    REQUIRE(DisplaySettingsManager::instance().set_display_rotation(90) == true);
    REQUIRE(DisplaySettingsManager::instance().set_display_rotation(90) == false);
    REQUIRE(DisplaySettingsManager::instance().set_display_rotation(270) == true);
    REQUIRE(DisplaySettingsManager::instance().set_display_rotation(270) == false);
    REQUIRE(DisplaySettingsManager::instance().set_display_rotation(0) == true);
    REQUIRE(DisplaySettingsManager::instance().set_display_rotation(0) == false);
}

// ============================================================================
// The settings row itself
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "DisplayRotation: the settings row is present and populated",
                 "[display_settings][rotation][ui]") {
    lv_obj_t* overlay = static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), "settings_display_sound_overlay", nullptr));
    REQUIRE(overlay != nullptr);

    lv_obj_t* row = lv_obj_find_by_name(overlay, "row_display_rotation");
    REQUIRE(row != nullptr);

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    REQUIRE(dropdown != nullptr);

    // Exactly the four orientations DisplayManager can apply, in the order
    // index_to_rotation_degrees() expects.
    const char* options = lv_dropdown_get_options(dropdown);
    REQUIRE(options != nullptr);
    int newlines = 0;
    for (const char* p = options; *p; ++p) {
        if (*p == '\n')
            ++newlines;
    }
    REQUIRE(newlines == 3);

    lv_obj_delete(overlay);
}

TEST_CASE_METHOD(LVGLUITestFixture, "DisplayRotation: the row is gated on backend support",
                 "[display_settings][rotation][ui]") {
    lv_subject_t* available = lv_xml_get_subject(nullptr, "settings_rotation_available");
    REQUIRE(available != nullptr);
    REQUIRE(lv_subject_get_int(available) ==
            (DisplaySettingsManager::rotation_setting_available() ? 1 : 0));

    lv_obj_t* overlay = static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), "settings_display_sound_overlay", nullptr));
    REQUIRE(overlay != nullptr);
    lv_obj_t* row = lv_obj_find_by_name(overlay, "row_display_rotation");
    REQUIRE(row != nullptr);

    // The bind_flag_if_eq must resolve, not silently no-op: an inert control on
    // a backend that ignores /display/rotate is worse than no control at all.
    bool hidden = lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN);
    REQUIRE(hidden == !DisplaySettingsManager::rotation_setting_available());

    lv_obj_delete(overlay);
}

TEST_CASE_METHOD(LVGLUITestFixture, "DisplayRotation: the dropdown callback is registered",
                 "[display_settings][rotation][ui]") {
    // Registering the XML row without adding the handler to register_callbacks()
    // fails silently - the dropdown moves and nothing is saved.
    helix::settings::get_display_sound_settings_overlay().register_callbacks();
    REQUIRE(lv_xml_get_event_cb(nullptr, "on_display_rotation_changed") != nullptr);
}
