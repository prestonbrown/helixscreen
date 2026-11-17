// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "status_dispatch.h"

#include "app_globals.h"
#include "led/led_controller.h"
#include "printer_state.h"
#include "sensor_manager.h"
#include "tool_state.h"

namespace helix {

void dispatch_status_frame(const StatusFrame& frame, std::optional<uint64_t> klippy_epoch) {
    const json& status = *frame.status;
    get_printer_state().update_from_status(status, frame.eventtime, frame.from_cached_snapshot,
                                           klippy_epoch, frame.whole_objects);
    ToolState::instance().update_from_status(status);

    // The fan chips follow the tool the nozzle reading follows.
    auto& ps = get_printer_state();
    const std::string& heater = ps.temperature_state().active_extruder_name();
    ps.fan_state().set_active_tool_fans(heater,
                                        ToolState::instance().own_part_fan_for_extruder(heater));

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
