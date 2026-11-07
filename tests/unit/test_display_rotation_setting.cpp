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

#include "ui_settings_display.h"
#include "ui_test_utils.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "config.h"
#include "display_settings_manager.h"
#include "rotation_probe.h"
#include "screensaver_registry.h"
#include "test_helpers/config_test_access.h"

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

    // The return value is what drives the restart prompt in the
    // on_display_rotation_changed callback.
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
        lv_xml_create(lv_screen_active(), "settings_display_overlay", nullptr));
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
        lv_xml_create(lv_screen_active(), "settings_display_overlay", nullptr));
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
    helix::settings::get_display_settings_overlay().register_callbacks();
    REQUIRE(lv_xml_get_event_cb(nullptr, "on_display_rotation_changed") != nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "DisplayRotation: tapping the Normal it already shows pins the probe",
                 "[display_settings][rotation][ui]") {
    // An unanswered first boot shows Normal, the value the screen is using for the session.
    // Tapping that same item is how a user stops the probe from returning every boot.
    Config* config = Config::get_instance();
    std::string& path = ConfigTestAccess::path(*config);
    const std::string saved_path = path;
    const json saved_data = ConfigTestAccess::data(*config);
    path.clear();
    json& display = config->get_json("/display");
    if (!display.is_object()) {
        display = json::object();
    }
    display.erase("rotate");
    display.erase("rotation_probed");
    REQUIRE(rotation_probe_unanswered(*config));

    helix::settings::get_display_settings_overlay().register_callbacks();
    lv_obj_t* overlay = static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), "settings_display_overlay", nullptr));
    REQUIRE(overlay != nullptr);
    lv_obj_t* row = lv_obj_find_by_name(overlay, "row_display_rotation");
    REQUIRE(row != nullptr);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    REQUIRE(dropdown != nullptr);
    REQUIRE(lv_dropdown_get_selected(dropdown) == 0);
    lv_obj_scroll_to_view_recursive(dropdown, LV_ANIM_OFF);
    lv_obj_update_layout(lv_screen_active());

    UITest::init(lv_screen_active());
    lv_area_t box;
    lv_obj_get_coords(dropdown, &box);
    REQUIRE(UITest::click_at((box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2));
    REQUIRE(lv_dropdown_is_open(dropdown));

    lv_obj_t* list = lv_dropdown_get_list(dropdown);
    REQUIRE(list != nullptr);
    lv_obj_update_layout(list);
    lv_obj_t* label = lv_obj_get_child(list, 0);
    REQUIRE(label != nullptr);
    lv_area_t first;
    lv_obj_get_coords(label, &first);
    REQUIRE(UITest::click_at((first.x1 + first.x2) / 2, first.y1 + 4));
    UITest::cleanup();

    CHECK_FALSE(lv_dropdown_is_open(dropdown));
    CHECK(lv_dropdown_get_selected(dropdown) == 0);
    CHECK(config->get<int>("/display/rotate", -1) == 0);
    CHECK(config->get<bool>("/display/rotation_probed", false));
    CHECK_FALSE(rotation_probe_unanswered(*config));

    lv_obj_delete(overlay);
    ConfigTestAccess::data(*config) = saved_data;
    path = saved_path;
}

// ============================================================================
// The dim-timeout row: Screen Dim, Screensaver after, or neither
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "DimRow: settings_dim_row_kind shows at most one row",
                 "[display_settings][dim_row][ui]") {
    lv_subject_t* kind = lv_xml_get_subject(nullptr, "settings_dim_row_kind");
    REQUIRE(kind != nullptr);

    lv_obj_t* overlay = static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), "settings_display_overlay", nullptr));
    REQUIRE(overlay != nullptr);
    lv_obj_t* dim = lv_obj_find_by_name(overlay, "row_display_dim");
    lv_obj_t* saver_after = lv_obj_find_by_name(overlay, "row_screensaver_after");
    REQUIRE(dim != nullptr);
    REQUIRE(saver_after != nullptr);

    using Kind = DisplaySettingsManager::DimRowKind;
    const int before = lv_subject_get_int(kind);
    struct Case {
        Kind kind;
        bool dim_visible;
        bool saver_after_visible;
    };
    for (const Case& c : {Case{Kind::Hidden, false, false}, Case{Kind::ScreenDim, true, false},
                          Case{Kind::ScreensaverAfter, false, true}}) {
        lv_subject_set_int(kind, static_cast<int>(c.kind));
        CAPTURE(static_cast<int>(c.kind));
        CHECK(!lv_obj_has_flag(dim, LV_OBJ_FLAG_HIDDEN) == c.dim_visible);
        CHECK(!lv_obj_has_flag(saver_after, LV_OBJ_FLAG_HIDDEN) == c.saver_after_visible);
    }
    lv_subject_set_int(kind, before);

    lv_obj_delete(overlay);
}

#if HELIX_ENABLE_SCREENSAVER
// On a device without a dimmable backlight the overlay's dim-timeout row tracks the
// screensaver choice live: Off leaves nothing for the timeout to start.
TEST_CASE_METHOD(LVGLUITestFixture, "DimRow: Screensaver after follows the screensaver choice",
                 "[display_settings][dim_row][ui]") {
    DisplaySettingsManager& mgr = DisplaySettingsManager::instance();
    REQUIRE_FALSE(mgr.has_dimming_control());
    Config* config = Config::get_instance();
    const std::string stored_name = config->get<std::string>("/display/screensaver_type", "");
    const int before = mgr.get_screensaver_type();

    lv_obj_t* overlay = static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), "settings_display_overlay", nullptr));
    REQUIRE(overlay != nullptr);
    lv_obj_t* dim = lv_obj_find_by_name(overlay, "row_display_dim");
    lv_obj_t* saver_after = lv_obj_find_by_name(overlay, "row_screensaver_after");
    REQUIRE(dim != nullptr);
    REQUIRE(saver_after != nullptr);

    mgr.set_screensaver_type(static_cast<int>(ScreensaverType::OFF));
    CHECK(lv_obj_has_flag(saver_after, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(dim, LV_OBJ_FLAG_HIDDEN));

    mgr.set_screensaver_type(static_cast<int>(ScreensaverType::STARFIELD));
    CHECK_FALSE(lv_obj_has_flag(saver_after, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(dim, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(overlay);
    mgr.set_screensaver_type(before);
    if (stored_name.empty()) {
        config->get_json("/display").erase("screensaver_type");
    } else {
        config->set<std::string>("/display/screensaver_type", stored_name);
    }
}
#endif

// The picker and its Test button follow the build's screensaver support and the chosen
// saver from XML; nothing in C++ hides them.
TEST_CASE_METHOD(LVGLUITestFixture, "Screensaver picker visibility comes from its subjects",
                 "[display_settings][ui]") {
    lv_subject_t* has_saver = lv_xml_get_subject(nullptr, "settings_has_screensaver");
    lv_subject_t* type = lv_xml_get_subject(nullptr, "settings_screensaver_type");
    REQUIRE(has_saver != nullptr);
    REQUIRE(type != nullptr);
    const int has_before = lv_subject_get_int(has_saver);
    const int type_before = lv_subject_get_int(type);

    lv_obj_t* overlay = static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), "settings_display_overlay", nullptr));
    REQUIRE(overlay != nullptr);
    lv_obj_t* row = lv_obj_find_by_name(overlay, "row_screensaver");
    lv_obj_t* test_btn = lv_obj_find_by_name(overlay, "container_test_screensaver");
    REQUIRE(row != nullptr);
    REQUIRE(test_btn != nullptr);

    lv_subject_set_int(has_saver, 0);
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(has_saver, 1);
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    lv_subject_set_int(type, 0);
    CHECK(lv_obj_has_flag(test_btn, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(type, 2);
    CHECK_FALSE(lv_obj_has_flag(test_btn, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(overlay);
    lv_subject_set_int(has_saver, has_before);
    lv_subject_set_int(type, type_before);
}
