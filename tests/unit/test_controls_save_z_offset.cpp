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

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "tool_state.h"
#include "z_offset_utils.h"

#include <algorithm>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterState;
using helix::ToolState;
using helix::ui::UpdateQueue;
using nlohmann::json;

namespace {

class ControlsSaveZOffsetFixture : public LVGLUITestFixture {
  public:
    ControlsSaveZOffsetFixture()
        : client(MoonrakerClientMock::PrinterType::VORON_24), api(client, state()),
          panel(state(), &api) {
        MoonrakerClientTestAccess::force_connection_state(client, ConnectionState::CONNECTED);
        helix::zoffset::reset_shared_save();
        previous_api = get_moonraker_api();
        set_moonraker_api(&api);
        // The XML callbacks resolve to the global panel, not to `panel`.
        get_global_controls_panel().set_api(&api);
        state().set_klippy_state_sync(helix::KlippyState::READY);
        helix::PrinterStateTestAccess::pin_z_offset_strategy(
            state(), ZOffsetCalibrationStrategy::PROBE_CALIBRATE);
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
        helix::zoffset::reset_shared_save();
        get_global_controls_panel().set_api(nullptr);
        set_moonraker_api(previous_api);
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

    /// Confirm the modal the save button raised.
    void confirm_save() {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_t* btn = lv_obj_find_by_name(dialog, "btn_primary");
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    }

    /// How many times @p command went to the printer.
    int sent(const std::string& command) {
        const auto& history = client.gcode_script_history();
        return static_cast<int>(
            std::count_if(history.begin(), history.end(), [&](const std::string& script) {
                return script.find(command) != std::string::npos;
            }));
    }

    /// Pump until @p command has been sent, or give up after ~2s.
    bool wait_for_sent(const std::string& command) {
        for (int i = 0; i < 200 && sent(command) == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            settle();
        }

        for (const auto& script : client.gcode_script_history()) {
            UNSCOPED_INFO("sent: " << script);
        }
        return sent(command) > 0;
    }

    MoonrakerClientMock client;
    MoonrakerAPI api;
    IMoonrakerAPI* previous_api = nullptr;
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
    // This firmware persists tool offsets without a restart, so there is
    // nothing to warn about and the save runs on the tap.
    CHECK(ModalStack::instance().stack_empty());
}

TEST_CASE_METHOD(ControlsSaveZOffsetFixture,
                 "Controls save button hides again once the offset is back to zero",
                 "[controls][zoffset][save-rule]") {
    set_global_offset_mm(-0.15);
    REQUIRE(save_button_visible());

    set_global_offset_mm(0.0);
    CHECK_FALSE(save_button_visible());
}

TEST_CASE_METHOD(ControlsSaveZOffsetFixture,
                 "Controls save applies the offset and then saves the config once confirmed",
                 "[controls][zoffset][save-flow]") {
    set_global_offset_mm(-0.15);
    click_save();
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);

    // Nothing goes out until the user answers the restart warning.
    CHECK(sent("Z_OFFSET_APPLY_PROBE") == 0);

    confirm_save();
    REQUIRE(wait_for_sent("SAVE_CONFIG"));
    CHECK(sent("Z_OFFSET_APPLY_PROBE") == 1);
    CHECK(sent("SAVE_CONFIG") == 1);
}

TEST_CASE_METHOD(ControlsSaveZOffsetFixture,
                 "Controls save honours saving turned off while the confirm is open",
                 "[controls][zoffset][save-flow][save-toggle]") {
    // The positive control is the case above: the same flow, left on, sends
    // Z_OFFSET_APPLY_PROBE once confirmed.
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    settings.set_z_offset_save_mode(static_cast<int>(helix::zoffset::SaveMode::Show));
    set_global_offset_mm(-0.15);
    click_save();
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);

    settings.set_z_offset_save_mode(static_cast<int>(helix::zoffset::SaveMode::Hide));
    confirm_save();
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        settle();
    }
    CHECK(sent("Z_OFFSET_APPLY_PROBE") == 0);
    CHECK(sent("SAVE_CONFIG") == 0);
    settings.set_z_offset_save_mode(static_cast<int>(helix::zoffset::SaveMode::Auto));
}

TEST_CASE_METHOD(ControlsSaveZOffsetFixture, "Controls save cancelled sends nothing",
                 "[controls][zoffset][save-flow]") {
    set_global_offset_mm(-0.15);
    click_save();
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* cancel = lv_obj_find_by_name(dialog, "btn_secondary");
    REQUIRE(cancel != nullptr);
    lv_obj_send_event(cancel, LV_EVENT_CLICKED, nullptr);
    settle();

    CHECK(sent("Z_OFFSET_APPLY_PROBE") == 0);
    CHECK(sent("SAVE_CONFIG") == 0);
}

TEST_CASE_METHOD(ControlsSaveZOffsetFixture,
                 "Controls save ignores a second click while one is in flight",
                 "[controls][zoffset][save-flow]") {
    set_global_offset_mm(-0.15);
    click_save();
    confirm_save();

    // The first save is still running: a second tap, and its confirmation if a
    // dialog appears at all, must not start another apply.
    lv_obj_send_event(save_button(), LV_EVENT_CLICKED, nullptr);
    if (ModalStack::instance().top_dialog() != nullptr) {
        confirm_save();
    }
    REQUIRE(wait_for_sent("SAVE_CONFIG"));
    settle();

    CHECK(sent("Z_OFFSET_APPLY_PROBE") == 1);
}
