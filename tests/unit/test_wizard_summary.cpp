// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wizard_summary.cpp
 * @brief Unit tests for wizard summary step subject initialization
 *
 * Tests that the wizard summary correctly initializes subjects with config
 * values and doesn't suffer from undefined behavior in the subject macros.
 *
 * Bug context: The original code passed the same buffer pointer as both
 * `buffer` and `initial_value` to UI_SUBJECT_INIT_AND_REGISTER_STRING,
 * which caused snprintf(buf, size, "%s", buf) - undefined behavior.
 * On some platforms this corrupted the data, causing blank summary screens.
 */

#include "ui_subject_registry.h"
#include "ui_wizard_summary.h"
#include "ui_wizard_telemetry.h"

#include "../lvgl_ui_test_fixture.h"
#include "config.h"
#include "core/subject.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "system_settings_manager.h"

#include <cstring>
#include <string>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Subject Macro Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "UI_SUBJECT_INIT_AND_REGISTER_STRING copies a separate source into the buffer",
                 "[wizard][summary][subjects]") {
    // Config values arrive as std::string::c_str(), never the buffer itself.
    static helix::Subject test_subject;
    static char buffer[128];
    for (const std::string value : {"FlashForge Adventurer 5M Pro", "", "Unnamed Printer",
                                    "Heater: extruder, Sensor: heater_bed"}) {
        CAPTURE(value);
        test_subject.reset();
        UI_SUBJECT_INIT_AND_REGISTER_STRING(test_subject, buffer, value.c_str(), "test_subject");
        CHECK(std::string(buffer) == value);
        CHECK(std::string(lv_subject_get_string(&test_subject)) == value);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "UI_SUBJECT_INIT_AND_REGISTER_INT works correctly",
                 "[wizard][summary][subjects]") {
    static helix::Subject test_subject;

    UI_SUBJECT_INIT_AND_REGISTER_INT(test_subject, 42, "test_int_subject");

    REQUIRE(lv_subject_get_int(&test_subject) == 42);
}

// ============================================================================
// Summary screen
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "Summary asks for telemetry only on the first printer",
                 "[wizard][summary]") {
    auto* cfg = Config::get_instance();
    const nlohmann::json printers_before = cfg->get<nlohmann::json>("/printers", nlohmann::json());
    auto* step = get_wizard_summary_step();
    step->register_callbacks();

    auto row_shown = [&](const char* name) {
        lv_obj_t* root = step->create(test_screen());
        REQUIRE(root != nullptr);
        lv_obj_t* obj = lv_obj_find_by_name(root, name);
        REQUIRE(obj != nullptr);
        const bool shown = !lv_obj_is_hidden(obj);
        step->cleanup();
        lv_obj_delete(root);
        return shown;
    };

    nlohmann::json one = nlohmann::json::object();
    one["default"] = nlohmann::json::object();
    cfg->set<nlohmann::json>("/printers", one);
    CHECK(row_shown("telemetry_opt_in_rows"));
    CHECK(row_shown("telemetry_divider"));

    nlohmann::json two = one;
    two["second"] = nlohmann::json::object();
    cfg->set<nlohmann::json>("/printers", two);
    CHECK_FALSE(row_shown("telemetry_opt_in_rows"));
    CHECK_FALSE(row_shown("telemetry_divider"));

    cfg->set<nlohmann::json>("/printers", printers_before);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "The wizard's update channel row opens on the stored channel, wired to Settings'",
                 "[wizard][summary][update_channel]") {
    auto* cfg = Config::get_instance();
    const nlohmann::json printers_before = cfg->get<nlohmann::json>("/printers", nlohmann::json());
    nlohmann::json one = nlohmann::json::object();
    one["default"] = nlohmann::json::object();
    cfg->set<nlohmann::json>("/printers", one);
    auto& system = helix::SystemSettingsManager::instance();
    const int channel_before = system.get_update_channel();
    system.set_update_channel(1); // Beta

    auto check_step = [&](helix::wizard::Step* step, const char* holder) {
        CAPTURE(holder != nullptr);
        // Each step registers the row's callback itself, whichever page opened first.
        lv_xml_register_event_cb(nullptr, "on_about_update_channel_changed", nullptr);
        REQUIRE(lv_xml_get_event_cb(nullptr, "on_about_update_channel_changed") == nullptr);
        step->register_callbacks();
        CHECK(lv_xml_get_event_cb(nullptr, "on_about_update_channel_changed") != nullptr);
        lv_obj_t* root = step->create(test_screen());
        REQUIRE(root != nullptr);
        if (holder) {
            // Asked beside the telemetry opt-in, which the first printer shows.
            lv_obj_t* box = lv_obj_find_by_name(root, holder);
            REQUIRE(box != nullptr);
            CHECK_FALSE(lv_obj_is_hidden(box));
        }
        lv_obj_t* row = lv_obj_find_by_name(root, "row_update_channel");
        REQUIRE(row != nullptr);
        CHECK(lv_dropdown_get_selected(lv_obj_find_by_name(row, "dropdown")) == 1);
        step->cleanup();
        lv_obj_delete(root);
    };
    check_step(get_wizard_summary_step(), "summary_update_channel");
    check_step(get_wizard_telemetry_step(), nullptr);

    system.set_update_channel(channel_before);
    cfg->set<nlohmann::json>("/printers", printers_before);
}
