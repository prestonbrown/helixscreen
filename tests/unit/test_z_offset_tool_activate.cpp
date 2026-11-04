// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_z_offset_tool_activate.cpp
 * @brief A tool offset written for the mounted tool is applied to the head at
 *        once, by both writers: the live adjust and the save.
 *
 * MedusaHC's controller sets a tool's offset only on a swap, so a write to its
 * `_TOOL_OFFSET` store changes nothing on the head until the next one; the
 * writers follow it with tool_offsets::activate_mounted_offset_gcode().
 */

#include "../busy_guard_fixture.h"
#include "core/update_queue.h"
#include "printer_discovery.h"
#include "printer_profile_state.h"
#include "save_config_restart.h"
#include "tool_state.h"
#include "z_offset_utils.h"

#include <algorithm>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

namespace {

PrinterDiscovery stock_medusahc() {
    PrinterDiscovery hw;
    hw.parse_objects(
        json::array({"gcode_move", "toolhead", "extruder", "extruder1", "extruder2", "extruder3",
                     "pin_watch io", "medusahc", "save_variables", "gcode_macro _TOOL_OFFSET"}));
    return hw;
}

class ToolActivateFixture : public BusyGuardFixture {
  public:
    ToolActivateFixture() {
        state.set_hardware(stock_medusahc());
        auto& ts = ToolState::instance();
        ts.deinit_subjects();
        ts.init_subjects(true);
        ts.init_tools(state.get_discovery());
        for (int tool = 0; tool < 4; ++tool) {
            ts.set_tool_offset_local(tool, Axis::Z, 100);
        }
        ts.set_backend_active_tool(1); // T1 on the carriage
        mock_client.clear_gcode_script_history();
    }
    ~ToolActivateFixture() override {
        ToolState::instance().clear_backend_active_tool();
        ToolState::instance().deinit_subjects();
        ui::UpdateQueue::instance().drain();
    }

    [[nodiscard]] bool sent(const std::string& line) const {
        const auto& history = mock_client.gcode_script_history();
        return std::any_of(history.begin(), history.end(), [&line](const std::string& script) {
            return script.find(line) != std::string::npos;
        });
    }
};

} // namespace

TEST_CASE_METHOD(ToolActivateFixture, "Adjusting the mounted tool's offset applies it at once",
                 "[zoffset][tool_offsets][medusahc]") {
    REQUIRE(zoffset::adjust_tool_offset(api.get(), &state, 1, 0.05));
    CHECK(sent("MHC_TOOL_OFFSET T=1 MOVE=0"));

    mock_client.clear_gcode_script_history();
    REQUIRE(zoffset::adjust_tool_offset(api.get(), &state, 2, 0.05));
    CHECK_FALSE(sent("MHC_TOOL_OFFSET")); // a docked tool takes it on its next pick
}

TEST_CASE_METHOD(ToolActivateFixture, "Saving the mounted tool's offset applies it at once",
                 "[zoffset][tool_offsets][medusahc]") {
    ui::SaveConfigWatch watch;
    zoffset::save_dirty_offsets(api.get(), watch, ZOffsetCalibrationStrategy::FIRMWARE_MANAGED,
                                state.get_discovery(), /*global_dirty=*/false, nullptr, nullptr,
                                &state);
    CHECK(sent("MHC_TOOL_OFFSET T=1 MOVE=0"));
    CHECK_FALSE(sent("MHC_TOOL_OFFSET T=2"));
}
