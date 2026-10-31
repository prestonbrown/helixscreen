// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_settings_hardware_health_row.cpp
 * @brief The Hardware Health row's live label and severity tint
 *
 * The row is built from settings_hardware_overlay.xml but wired from C++,
 * so a lookup that silently finds nothing leaves a static row that still
 * renders and still navigates. Only the bound values reveal the break.
 *
 * @see include/ui_settings_hardware.h - bind_hardware_health_row
 */

#include "ui_settings_hardware.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"
#include "app_globals.h"
#include "hardware_validator.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct HardwareHealthRowFixture : public LVGLUITestFixture {
    HardwareHealthRowFixture() {
        root_ = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "settings_hardware_overlay", nullptr));
        if (root_) {
            helix::settings::bind_hardware_health_row(root_);
        }
    }

    ~HardwareHealthRowFixture() override {
        if (root_ && helix::test::widget_in_tree(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
    }

    lv_obj_t* find(const char* name) const {
        lv_obj_t* row = lv_obj_find_by_name(root_, "row_hardware_health");
        REQUIRE(row != nullptr);
        lv_obj_t* found = lv_obj_find_by_name(row, name);
        REQUIRE(found != nullptr);
        return found;
    }

    void set_level(int level) {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "hardware_status_level");
        REQUIRE(subject != nullptr);
        lv_subject_set_int(subject, level);
        process_lvgl(5);
    }

    lv_obj_t* root_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(HardwareHealthRowFixture, "hardware health row: icon colour tracks status level",
                 "[hardware][ui]") {
    REQUIRE(root_ != nullptr);
    lv_obj_t* icon = find("row_icon");

    set_level(static_cast<int>(HardwareStatusLevel::OK));
    const lv_color_t untinted = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    SECTION("attention takes the warning colour") {
        set_level(static_cast<int>(HardwareStatusLevel::ATTENTION));

        REQUIRE(lv_color_eq(lv_obj_get_style_text_color(icon, LV_PART_MAIN),
                            theme_manager_get_color("warning")));
    }

    SECTION("critical takes the danger colour") {
        set_level(static_cast<int>(HardwareStatusLevel::CRITICAL));

        REQUIRE(lv_color_eq(lv_obj_get_style_text_color(icon, LV_PART_MAIN),
                            theme_manager_get_color("danger")));
    }

    SECTION("returning to OK restores the row's own colour") {
        set_level(static_cast<int>(HardwareStatusLevel::CRITICAL));
        set_level(static_cast<int>(HardwareStatusLevel::OK));

        REQUIRE(lv_color_eq(lv_obj_get_style_text_color(icon, LV_PART_MAIN), untinted));
    }

    SECTION("the untinted colour is neither severity colour") {
        REQUIRE_FALSE(lv_color_eq(untinted, theme_manager_get_color("warning")));
        REQUIRE_FALSE(lv_color_eq(untinted, theme_manager_get_color("danger")));
    }
}

TEST_CASE_METHOD(HardwareHealthRowFixture, "hardware health row: label reports the live count",
                 "[hardware][ui]") {
    REQUIRE(root_ != nullptr);
    lv_obj_t* label = find("label");

    SECTION("a clean result reads as no issues") {
        get_printer_state().hardware_validation_state().set_hardware_validation_result(
            HardwareValidationResult{});
        process_lvgl(5);

        REQUIRE(std::string(lv_label_get_text(label)) == "No Hardware Issues");
    }

    SECTION("discovered hardware is counted into the row") {
        HardwareValidationResult result;
        result.newly_discovered.push_back(
            HardwareIssue::info("neopixel chamber", HardwareType::LED, "Detected"));
        result.newly_discovered.push_back(
            HardwareIssue::info("fan_generic exhaust", HardwareType::FAN, "Detected"));
        get_printer_state().hardware_validation_state().set_hardware_validation_result(result);
        process_lvgl(5);

        REQUIRE(std::string(lv_label_get_text(label)) == "2 Hardware Issues");
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "hardware health row: create() performs the wiring",
                 "[hardware][ui]") {
    // A binder nothing calls is the exact shape of the defect this row had:
    // the row still renders and still navigates, only its values go stale.
    helix::settings::HardwareSettingsOverlay overlay;
    overlay.register_callbacks();
    lv_obj_t* root = overlay.create(test_screen());
    REQUIRE(root != nullptr);

    lv_obj_t* row = lv_obj_find_by_name(root, "row_hardware_health");
    REQUIRE(row != nullptr);
    lv_obj_t* label = lv_obj_find_by_name(row, "label");
    REQUIRE(label != nullptr);

    HardwareValidationResult result;
    result.newly_discovered.push_back(
        HardwareIssue::info("neopixel chamber", HardwareType::LED, "Detected"));
    get_printer_state().hardware_validation_state().set_hardware_validation_result(result);
    process_lvgl(5);

    REQUIRE(std::string(lv_label_get_text(label)) == "1 Hardware Issue");
}
