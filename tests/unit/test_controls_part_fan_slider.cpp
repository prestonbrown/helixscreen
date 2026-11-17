// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_controls_part_fan_slider.cpp
 * @brief The Controls Part Fan slider commands the fan the card shows.
 *
 * The card's reading follows the printer's part cooling fan, which can be a
 * named fan (fan_generic, output_pin) rather than Klipper's bare [fan]. The
 * slider must drive that same fan: an M106 aimed at a [fan] the printer does
 * not have moves nothing, or errors, while the card shows the new speed.
 *
 * Driven the way the screen drives it: a slider moved to a value, its
 * value_changed event delivered to the callback the XML names.
 */

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

namespace {

class PartFanSliderFixture : public LVGLUITestFixture {
  public:
    PartFanSliderFixture() {
        get_global_controls_panel().init_subjects();
        get_global_controls_panel().set_api(&api);
    }

    ~PartFanSliderFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        get_global_controls_panel().set_api(nullptr);
        get_global_controls_panel().deinit_subjects();
        state().fan_state().init_fans({});
        helix::ui::UpdateQueue::instance().drain();
    }

    void use_fans(const std::vector<std::string>& fans, const std::string& part_fan) {
        helix::FanRoleConfig roles;
        roles.part_fan = part_fan;
        state().fan_state().init_fans(fans, roles);
        client.clear_gcode_script_history();
    }

    /// Move a slider wired to the panel's XML callback to @p pct.
    void slide_to(int pct) {
        lv_event_cb_t cb = lv_xml_get_event_cb(nullptr, "on_controls_fan_slider");
        REQUIRE(cb != nullptr);
        lv_obj_t* slider = lv_slider_create(test_screen());
        lv_slider_set_range(slider, 0, 100);
        lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_slider_set_value(slider, pct, LV_ANIM_OFF);
        lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_delete(slider);
        helix::ui::UpdateQueue::instance().drain();
    }

    [[nodiscard]] bool sent(const std::string& needle) const {
        const auto& h = client.gcode_script_history();
        return std::any_of(h.begin(), h.end(), [&](const std::string& l) {
            return l.find(needle) != std::string::npos;
        });
    }

    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI api{client, state()};
};

} // namespace

TEST_CASE_METHOD(PartFanSliderFixture, "Controls part fan slider drives the printer's part fan",
                 "[controls][fan]") {
    SECTION("a named part fan gets its own command, not M106") {
        use_fans({"fan_generic part_fan", "heater_fan hotend_fan"}, "fan_generic part_fan");
        slide_to(60);
        CHECK(sent("SET_FAN_SPEED FAN=part_fan SPEED=0.60"));
        CHECK_FALSE(sent("M106"));
    }

    SECTION("the bare [fan] still gets M106") {
        use_fans({"fan", "heater_fan hotend_fan"}, "");
        slide_to(60);
        CHECK(sent("M106 S153"));
    }
}
