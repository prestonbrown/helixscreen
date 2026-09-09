// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Controls panel's Save Z-Offset button, end to end: the XML binding that
// shows it and the handler that runs when it is clicked both ask
// helix::zoffset::save_available().
//
// The pair is the point. A button shown over a state the handler refuses is a
// dead tap; a handler that saves a state the button hides is an offset the user
// was never told about. Either half asking its own question reintroduces that.

#include "ui_modal.h"
#include "ui_panel_controls.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "tool_state.h"
#include "z_offset_utils.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterState;
using helix::ToolState;
using helix::ui::UpdateQueue;
using nlohmann::json;

namespace {

class ControlsSaveZOffsetFixture : public LVGLUITestFixture {
  public:
    ControlsSaveZOffsetFixture() : panel(state(), nullptr) {
        ToolState::instance().deinit_subjects();
        ToolState::instance().init_subjects(true);

        helix::PrinterDiscovery hw;
        json objects = json::array({"gcode_move"});
        hw.parse_objects(objects);
        ToolState::instance().init_tools(hw);

        helix::zoffset::deinit_save_available_subject();
        helix::zoffset::init_save_available_subject(true);

        panel.init_subjects();
        panel_obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "controls_panel", nullptr));
        REQUIRE(panel_obj != nullptr);
        panel.setup(panel_obj, test_screen());
        lv_obj_update_layout(test_screen());
        process_lvgl(20);
        panel.on_activate();
        settle();
    }

    ~ControlsSaveZOffsetFixture() override {
        ModalStack::instance().clear();
        if (panel_obj) {
            panel.on_deactivate(DeactivateReason::NavigateAway);
            lv_obj_delete(panel_obj);
            panel_obj = nullptr;
        }
        UpdateQueue::instance().drain();
        panel.deinit_subjects();
        helix::zoffset::deinit_save_available_subject();
        ToolState::instance().deinit_subjects();
        UpdateQueue::instance().drain();
    }

    void settle() {
        UpdateQueue::instance().drain();
        process_lvgl(20);
    }

    lv_obj_t* save_button() {
        lv_obj_t* btn = lv_obj_find_by_name(panel_obj, "btn_save_z_offset");
        REQUIRE(btn != nullptr);
        return btn;
    }

    bool save_button_visible() {
        return !lv_obj_has_flag(save_button(), LV_OBJ_FLAG_HIDDEN);
    }

    void click_save() {
        lv_obj_send_event(save_button(), LV_EVENT_CLICKED, nullptr);
        settle();
    }

    void dirty_a_tool() {
        ToolState::instance().set_tool_offset_local(0, helix::Axis::Z, 60);
        settle();
    }

    void set_global_offset_mm(double mm) {
        state().update_from_status(
            json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, mm, 0.0}}}}});
        settle();
    }

    ControlsPanel panel;
    lv_obj_t* panel_obj = nullptr;
};

} // namespace

TEST_CASE_METHOD(ControlsSaveZOffsetFixture, "Controls save button is hidden with nothing dirty",
                 "[controls][zoffset][save-rule]") {
    CHECK_FALSE(save_button_visible());

    click_save();
    CHECK(ModalStack::instance().stack_empty());
}

TEST_CASE_METHOD(ControlsSaveZOffsetFixture,
                 "Controls save button appears for the machine-wide offset",
                 "[controls][zoffset][save-rule]") {
    set_global_offset_mm(-0.15);

    CHECK(save_button_visible());
    click_save();
    CHECK(ModalStack::instance().top_dialog() != nullptr);
}

TEST_CASE_METHOD(ControlsSaveZOffsetFixture,
                 "Controls save button appears for a dirty tool with the global at zero",
                 "[controls][zoffset][save-rule]") {
    // The case a machine-wide-only binding hides. The handler saves tool offsets,
    // so hiding the button here loses them at the next Klipper restart with
    // nothing on screen to say so.
    dirty_a_tool();

    CHECK(save_button_visible());
    click_save();
    CHECK(ModalStack::instance().top_dialog() != nullptr);
}

TEST_CASE_METHOD(ControlsSaveZOffsetFixture,
                 "Controls save button hides again once the offset is back to zero",
                 "[controls][zoffset][save-rule]") {
    set_global_offset_mm(-0.15);
    REQUIRE(save_button_visible());

    set_global_offset_mm(0.0);
    CHECK_FALSE(save_button_visible());
}
