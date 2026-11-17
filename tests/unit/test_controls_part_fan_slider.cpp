// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The Controls Part Fan slider commands the fan the card shows, which can be a
// named fan (fan_generic, output_pin) rather than Klipper's bare [fan]. Driven
// the way the screen drives it: a slider's value_changed reaching the XML callback.

#include "ui_panel_controls.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture, "Controls part fan slider drives the printer's part fan",
                 "[controls][fan]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI api{client, state()};
    auto& panel = get_global_controls_panel();
    panel.init_subjects();
    panel.set_api(&api);

    auto slide_to_60 = [&](const std::vector<std::string>& fans, const std::string& part_fan) {
        helix::FanRoleConfig roles;
        roles.part_fan = part_fan;
        state().fan_state().init_fans(fans, roles);
        client.clear_gcode_script_history();
        lv_obj_t* slider = lv_slider_create(test_screen());
        lv_slider_set_range(slider, 0, 100);
        lv_obj_add_event_cb(slider, lv_xml_get_event_cb(nullptr, "on_controls_fan_slider"),
                            LV_EVENT_VALUE_CHANGED, nullptr);
        lv_slider_set_value(slider, 60, LV_ANIM_OFF);
        lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_delete(slider);
        helix::ui::UpdateQueue::instance().drain();
        const auto& h = client.gcode_script_history();
        return std::vector<std::string>(h.begin(), h.end());
    };
    auto has = [](const std::vector<std::string>& h, const std::string& needle) {
        return std::any_of(h.begin(), h.end(), [&](const std::string& l) {
            return l.find(needle) != std::string::npos;
        });
    };

    auto named =
        slide_to_60({"fan_generic part_fan", "heater_fan hotend_fan"}, "fan_generic part_fan");
    CHECK(has(named, "SET_FAN_SPEED FAN=part_fan SPEED=0.60"));
    CHECK_FALSE(has(named, "M106"));
    CHECK(has(slide_to_60({"fan", "heater_fan hotend_fan"}, ""), "M106 S153"));

    panel.set_api(nullptr);
    panel.deinit_subjects();
    state().fan_state().init_fans({});
    helix::ui::UpdateQueue::instance().drain();
}
