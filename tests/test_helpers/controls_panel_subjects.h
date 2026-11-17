// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "led/led_controller.h"
#include "printer_state.h"
#include "temperature_sensor_manager.h"
#include "tool_state.h"

namespace helix::test {

/// Initializes every subject a ControlsPanel observes from its constructor on, as
/// the app has by the time it builds one. Each init is a no-op while already live.
inline PrinterState& init_controls_panel_subjects(PrinterState& state) {
    state.init_subjects(false);
    ToolState::instance().init_subjects(false);
    sensors::TemperatureSensorManager::instance().init_subjects();
    led::LedController::instance().init(nullptr, nullptr);
    return state;
}

} // namespace helix::test
