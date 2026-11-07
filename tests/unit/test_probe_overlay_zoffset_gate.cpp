// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_probe_overlay_zoffset_gate.cpp
 * @brief The Probe overlay's Z-Offset button follows the same gate as the
 *        Advanced row and the Controls tile.
 *
 * Builds the real probe_overlay.xml and drives the subjects its visibility
 * binding reads: the button steps aside where the firmware keeps a Z offset
 * per head, and where the tool offset routine also sets the reference tool's Z.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "lvgl/lvgl.h"
#include "setting_group.h"

#include "../catch_amalgamated.hpp"

namespace {

class ProbeOverlayZOffsetFixture : public XMLTestFixture {
  public:
    ProbeOverlayZOffsetFixture() : XMLTestFixture() {
        setting_group_register();
        REQUIRE(register_component("header_bar"));
        REQUIRE(register_component("setting_section_header"));
        REQUIRE(register_component("setting_action_row"));
        REQUIRE(register_component("probe_overlay"));

        overlay_ = create_component("probe_overlay");
        REQUIRE(overlay_ != nullptr);
        button_ = lv_obj_find_by_name(overlay_, "btn_zoffset");
        REQUIRE(button_ != nullptr);
        process_lvgl(10);
    }

    ~ProbeOverlayZOffsetFixture() override {
        set("z_offset_reapplied_per_head", 0);
        set("printer_has_tool_offset_cal", 0);
        set("hide_manual_z_calibration", 0);
    }

    void set(const char* name, int value) {
        lv_subject_t* subj = lv_xml_get_subject(nullptr, name);
        REQUIRE(subj != nullptr);
        lv_subject_set_int(subj, value);
        process_lvgl(10);
    }

    bool button_hidden() const {
        return lv_obj_has_flag(button_, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* button_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(ProbeOverlayZOffsetFixture,
                 "Probe Z-Offset button steps aside for per-head offsets or a full tool routine",
                 "[probe][xml][per_head][tool_offsets]") {
    set("printer_has_tool_offset_cal", 0);
    set("hide_manual_z_calibration", 0);
    set("z_offset_reapplied_per_head", 0);
    CHECK_FALSE(button_hidden());

    set("z_offset_reapplied_per_head", 1);
    CHECK(button_hidden());
    set("z_offset_reapplied_per_head", 0);

    set("printer_has_tool_offset_cal", 1);
    set("hide_manual_z_calibration", 1);
    CHECK(button_hidden());
    set("printer_has_tool_offset_cal", 0);
    CHECK_FALSE(button_hidden());
}
