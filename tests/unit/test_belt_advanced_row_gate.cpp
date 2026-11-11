// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_belt_advanced_row_gate.cpp
 * @brief The Advanced panel's calibration rows follow printer support, not
 *        beta features (prestonbrown/helixscreen#1736).
 *
 * Builds the real advanced_panel.xml with beta features off and drives the
 * subjects each row's visibility binding reads. Belt Tension needs
 * printer_has_accelerometer and printer_supports_belt_compare (1 only on
 * corexy/limited_corexy); Tool Offsets needs printer_has_tool_offset_cal;
 * Z-Offset Calibration needs a probe and steps aside where the tool offset
 * routine also sets the reference tool's Z, or where the firmware keeps a
 * Z offset per head. The G-code Console row needs a build with the console
 * (platform_console_supported, 0 in the K-Touch feature profile).
 */

#include "ui_status_pill.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "lvgl/lvgl.h"
#include "setting_group.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// Builds the real advanced_panel.xml so the Belt Tension row's visibility
/// binding can be exercised against a live tree, in the same shape
/// production's xml_registration.xml uses (see AdvancedPowerGroupFixture in
/// test_host_power_availability.cpp for the sibling POWER-group case).
class AdvancedBeltRowFixture : public XMLTestFixture {
  public:
    AdvancedBeltRowFixture() : XMLTestFixture() {
        setting_group_register();
        ui_status_pill_register_widget();
        REQUIRE(register_component("setting_group_header"));
        REQUIRE(register_component("setting_action_row"));
        REQUIRE(register_component("advanced_panel"));

        // The XML subject registry is process-global and never forgets an
        // entry, so every subject below is a function-local STATIC: a fixture
        // member would dangle after this test and any later consumer of the
        // name dereferences freed memory.
        ensure_subject("show_beta_features", 0);
        ensure_subject("printer_has_accelerometer", 1);
        ensure_subject("printer_supports_belt_compare", 0);
        ensure_subject("printer_has_tool_offset_cal", 0);
        ensure_subject("printer_has_probe", 1);
        ensure_subject("hide_manual_z_calibration", 0);
        ensure_subject("z_offset_reapplied_per_head", 0);
        ensure_subject("platform_console_supported", 1);
        beta_ = subject("show_beta_features");
        beta_before_ = lv_subject_get_int(beta_);
        lv_subject_set_int(beta_, 0);

        panel_ = create_component("advanced_panel");
        REQUIRE(panel_ != nullptr);
        row_ = lv_obj_find_by_name(panel_, "row_belt_tension");
        REQUIRE(row_ != nullptr);
        process_lvgl(50);
    }

    ~AdvancedBeltRowFixture() override {
        lv_subject_set_int(beta_, beta_before_);
    }

    /// Hidden itself or under a hidden ancestor, up to the panel root.
    bool hidden_in_panel(lv_obj_t* obj) const {
        for (; obj && obj != panel_; obj = lv_obj_get_parent(obj)) {
            if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
                return true;
            }
        }
        return false;
    }

    lv_obj_t* row(const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(panel_, name);
        REQUIRE(obj != nullptr);
        return obj;
    }

    lv_subject_t* subject(const char* name) {
        lv_subject_t* subj = lv_xml_get_subject(nullptr, name);
        REQUIRE(subj != nullptr);
        return subj;
    }

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* row_ = nullptr;
    lv_subject_t* beta_ = nullptr;
    int beta_before_ = 0;

  private:
    /// Registers `name` in the global XML scope at `value` when absent,
    /// leaving any earlier registration (this test's or another fixture's)
    /// untouched.
    static void ensure_subject(const char* name, int value) {
        if (!lv_xml_get_subject(nullptr, name)) {
            static lv_subject_t subjects[8];
            static size_t next = 0;
            REQUIRE(next < 8);
            lv_subject_init_int(&subjects[next], value);
            lv_xml_register_subject(nullptr, name, &subjects[next]);
            next++;
        }
    }
};

} // namespace

TEST_CASE_METHOD(AdvancedBeltRowFixture,
                 "Advanced Belt Tension row needs belt-compare support, not beta features",
                 "[belt][advanced][xml]") {
    REQUIRE(lv_subject_get_int(beta_) == 0);
    auto* accel = subject("printer_has_accelerometer");
    auto* belt = subject("printer_supports_belt_compare");

    // Without belt-path kinematics the row stays hidden even with an
    // accelerometer.
    lv_subject_set_int(accel, 1);
    lv_subject_set_int(belt, 0);
    process_lvgl(10);
    CHECK(hidden_in_panel(row_));

    // Belt-path kinematics plus an accelerometer shows it.
    lv_subject_set_int(belt, 1);
    process_lvgl(10);
    CHECK_FALSE(hidden_in_panel(row_));

    // Losing support hides it again.
    lv_subject_set_int(belt, 0);
    process_lvgl(10);
    CHECK(hidden_in_panel(row_));

    // Support alone is not enough: no accelerometer, no row.
    lv_subject_set_int(belt, 1);
    lv_subject_set_int(accel, 0);
    process_lvgl(10);
    CHECK(hidden_in_panel(row_));
}

TEST_CASE_METHOD(AdvancedBeltRowFixture,
                 "Advanced Pressure Advance Tuning row shows with beta features off",
                 "[pa_tune][advanced][xml]") {
    REQUIRE(lv_subject_get_int(beta_) == 0);
    CHECK_FALSE(hidden_in_panel(row("row_pa_tuning")));
}

TEST_CASE_METHOD(AdvancedBeltRowFixture,
                 "Advanced Tool Offsets row follows tool offset support, not beta features",
                 "[tool_offsets][advanced][xml]") {
    REQUIRE(lv_subject_get_int(beta_) == 0);
    auto* tool_offset_cal = subject("printer_has_tool_offset_cal");
    lv_obj_t* tool_offsets = row("row_tool_offsets");

    lv_subject_set_int(tool_offset_cal, 0);
    process_lvgl(10);
    CHECK(hidden_in_panel(tool_offsets));

    lv_subject_set_int(tool_offset_cal, 1);
    process_lvgl(10);
    CHECK_FALSE(hidden_in_panel(tool_offsets));

    lv_subject_set_int(tool_offset_cal, 0);
}

TEST_CASE_METHOD(AdvancedBeltRowFixture,
                 "Advanced Z-Offset row steps aside for a full tool offset routine with beta off",
                 "[tool_offsets][advanced][xml]") {
    REQUIRE(lv_subject_get_int(beta_) == 0);
    auto* tool_offset_cal = subject("printer_has_tool_offset_cal");
    auto* hide_manual = subject("hide_manual_z_calibration");
    lv_subject_set_int(subject("printer_has_probe"), 1);
    lv_obj_t* z_offset = row("row_z_offset");

    lv_subject_set_int(tool_offset_cal, 1);
    lv_subject_set_int(hide_manual, 0);
    process_lvgl(10);
    CHECK_FALSE(hidden_in_panel(z_offset));

    lv_subject_set_int(hide_manual, 1);
    process_lvgl(10);
    CHECK(hidden_in_panel(z_offset));

    lv_subject_set_int(tool_offset_cal, 0);
    lv_subject_set_int(hide_manual, 0);
}

TEST_CASE_METHOD(AdvancedBeltRowFixture,
                 "Advanced Z-Offset row steps aside where the firmware keeps a Z offset per head",
                 "[tool_offsets][advanced][xml][per_head]") {
    auto* per_head = subject("z_offset_reapplied_per_head");
    lv_subject_set_int(subject("printer_has_probe"), 1);
    lv_subject_set_int(subject("printer_has_tool_offset_cal"), 0);
    lv_subject_set_int(subject("hide_manual_z_calibration"), 0);
    lv_obj_t* z_offset = row("row_z_offset");

    lv_subject_set_int(per_head, 0);
    process_lvgl(10);
    CHECK_FALSE(hidden_in_panel(z_offset));

    lv_subject_set_int(per_head, 1);
    process_lvgl(10);
    CHECK(hidden_in_panel(z_offset));

    lv_subject_set_int(per_head, 0);
}

TEST_CASE_METHOD(AdvancedBeltRowFixture,
                 "Advanced G-code Console row shows only in a build with the console",
                 "[console][advanced][xml]") {
    auto* supported = subject("platform_console_supported");
    const int before = lv_subject_get_int(supported);
    lv_obj_t* console = row("row_console");

    lv_subject_set_int(supported, 1);
    process_lvgl(10);
    CHECK_FALSE(hidden_in_panel(console));

    lv_subject_set_int(supported, 0);
    process_lvgl(10);
    CHECK(hidden_in_panel(console));

    lv_subject_set_int(supported, before);
}
