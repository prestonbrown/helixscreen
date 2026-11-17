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
 */

#include "ui_panel_controls.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/controls_panel_test_access.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using TA = helix::ui::ControlsPanelTestAccess;

namespace {

struct FanSliderHarness {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPI api{client, state};
    std::unique_ptr<ControlsPanel> panel;

    FanSliderHarness(const std::vector<std::string>& fans, const std::string& part_fan) {
        state.init_subjects(false);
        helix::FanRoleConfig roles;
        roles.part_fan = part_fan;
        state.fan_state().init_fans(fans, roles);
        panel = std::make_unique<ControlsPanel>(state, &api);
        TA::init_and_observe(*panel);
        helix::ui::UpdateQueue::instance().drain();
        client.clear_gcode_script_history();
    }

    ~FanSliderHarness() {
        helix::ui::UpdateQueue::instance().drain();
        panel.reset();
        helix::ui::UpdateQueue::instance().drain();
    }

    [[nodiscard]] bool sent(const std::string& needle) const {
        const auto& h = client.gcode_script_history();
        return std::any_of(h.begin(), h.end(), [&](const std::string& l) {
            return l.find(needle) != std::string::npos;
        });
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Controls part fan slider drives the printer's part fan",
                 "[controls][fan]") {
    SECTION("a named part fan gets its own command, not M106") {
        FanSliderHarness h({"fan_generic part_fan", "heater_fan hotend_fan"},
                           "fan_generic part_fan");
        TA::part_fan_slider_changed(*h.panel, 60);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(h.sent("SET_FAN_SPEED FAN=part_fan SPEED=0.60"));
        CHECK_FALSE(h.sent("M106"));
    }

    SECTION("the bare [fan] still gets M106") {
        FanSliderHarness h({"fan", "heater_fan hotend_fan"}, "");
        TA::part_fan_slider_changed(*h.panel, 60);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(h.sent("M106 S153"));
    }
}
