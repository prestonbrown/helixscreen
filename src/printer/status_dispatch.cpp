// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "status_dispatch.h"

#include "app_globals.h"
#include "led/led_controller.h"
#include "material_z_offset_state.h"
#include "printer_state.h"
#include "sensor_manager.h"
#include "tool_state.h"

#include <algorithm>
#include <string>
#include <vector>

namespace helix {

void dispatch_status_frame(const StatusFrame& frame, std::optional<uint64_t> klippy_epoch) {
    const json& status = *frame.status;
    get_printer_state().update_from_status(status, frame.eventtime, frame.from_cached_snapshot,
                                           klippy_epoch, frame.whole_objects);
    ToolState::instance().update_from_status(status);
    material_zoffset::MaterialZOffsetState::instance().update_from_status(status);

    // The fan chips follow the tool the nozzle reading follows, and the aux chip
    // leaves out every tool's own fans.
    auto& ps = get_printer_state();
    const auto& tools = ToolState::instance();
    const std::string& heater = ps.temperature_state().active_extruder_name();
    ps.fan_state().set_active_tool_fans(heater, tools.own_part_fan_for_extruder(heater));
    std::vector<std::string> tool_part_fans, tool_heaters;
    const auto add = [](std::vector<std::string>& to, const std::string& name) {
        if (!name.empty() && std::find(to.begin(), to.end(), name) == to.end())
            to.push_back(name);
    };
    for (const auto& tool : tools.tools()) {
        if (!tool.extruder_name)
            continue;
        add(tool_part_fans, tools.own_part_fan_for_extruder(*tool.extruder_name));
        if (tools.has_multiple_extruders())
            add(tool_heaters, *tool.extruder_name);
    }
    ps.fan_state().set_tool_fans(std::move(tool_part_fans), std::move(tool_heaters));

    auto& led_ctrl = led::LedController::instance();
    if (led_ctrl.is_initialized()) {
        led_ctrl.update_from_status(status);
    }

    for (auto* m : sensors::sensor_managers()) {
        m->update_from_status(status);
    }
}

void dispatch_status(const json& status) {
    StatusFrame frame;
    frame.status = &status;
    dispatch_status_frame(frame, std::nullopt);
}

} // namespace helix
