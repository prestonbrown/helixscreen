// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The XML event callbacks the Controls panel owns. The XML names them by
// string, so a callback that stops being registered fails silently: the widget
// stays on screen and does nothing.

#include "ui_modal.h"
#include "ui_panel_controls.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

namespace {

constexpr const char* kControlsCallbacks[] = {
    "on_calibration_bed_mesh",   "on_calibration_zoffset",   "on_calibration_tool_offsets",
    "on_calibration_pa",         "on_calibration_screws",    "on_calibration_motors",
    "on_controls_home_all",      "on_controls_home_x",       "on_controls_home_y",
    "on_controls_home_xy",       "on_controls_home_z",       "on_controls_qgl",
    "on_controls_z_tilt",        "on_controls_macro",        "on_controls_fan_slider",
    "on_controls_save_z_offset", "on_zoffset_tune",          "on_controls_quick_actions",
    "on_nozzle_temp_clicked",    "on_bed_temp_clicked",      "on_chamber_temp_clicked",
    "on_controls_cooling",       "on_controls_more_sensors", "on_controls_secondary_fans",
    "on_nozzle_target_edit",     "on_bed_target_edit",       "on_chamber_target_edit",
};

class ControlsCallbacksFixture : public LVGLUITestFixture {
  public:
    ControlsCallbacksFixture() : panel(state(), nullptr) {
        panel.init_subjects();
    }

    ~ControlsCallbacksFixture() override {
        ModalStack::instance().clear();
        helix::ui::UpdateQueue::instance().drain();
        panel.deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Fire a registered callback the way a tap on a widget naming it would.
    void fire(const char* name) {
        lv_event_cb_t cb = lv_xml_get_event_cb(nullptr, name);
        REQUIRE(cb != nullptr);
        lv_obj_t* btn = lv_obj_create(test_screen());
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        lv_obj_delete(btn);
        helix::ui::UpdateQueue::instance().drain();
    }

    ControlsPanel panel;
};

} // namespace

TEST_CASE_METHOD(ControlsCallbacksFixture, "Controls registers every callback its XML names",
                 "[controls][callbacks]") {
    for (const char* name : kControlsCallbacks) {
        INFO("callback: " << name);
        CHECK(lv_xml_get_event_cb(nullptr, name) != nullptr);
    }
}

TEST_CASE_METHOD(ControlsCallbacksFixture, "Controls Motors Off callback raises its confirmation",
                 "[controls][callbacks]") {
    REQUIRE(ModalStack::instance().stack_empty());

    fire("on_calibration_motors");

    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    // Answer it, so the global panel's dialog handle is released.
    lv_obj_t* cancel = lv_obj_find_by_name(dialog, "btn_secondary");
    REQUIRE(cancel != nullptr);
    lv_obj_send_event(cancel, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
}
