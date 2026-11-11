// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "temperature_sensor_types.h"

#include <cstdint>
#include <vector>

namespace helix {

/// Which default a temperature graph series from TemperatureSensorManager follows.
enum class TempGraphSensorGroup : uint8_t {
    Auxiliary,     ///< off by default; a chip turns it on for this session
    StepperDriver, ///< on in the all-series view
    Mcu,           ///< opt-in, remembered per printer (SettingsManager)
    Host,          ///< opt-in, remembered per printer (SettingsManager)
};

/// The group a sensor's role puts its series in. CHAMBER is graphed as the
/// chamber series, never through this path.
[[nodiscard]] constexpr TempGraphSensorGroup
temp_graph_sensor_group(sensors::TemperatureSensorRole role) {
    switch (role) {
    case sensors::TemperatureSensorRole::STEPPER_DRIVER:
        return TempGraphSensorGroup::StepperDriver;
    case sensors::TemperatureSensorRole::MCU:
        return TempGraphSensorGroup::Mcu;
    case sensors::TemperatureSensorRole::HOST:
        return TempGraphSensorGroup::Host;
    default:
        return TempGraphSensorGroup::Auxiliary;
    }
}

struct TempGraphSensorSeries {
    const sensors::TemperatureSensorConfig* sensor = nullptr; ///< points into the input list
    TempGraphSensorGroup group = TempGraphSensorGroup::Auxiliary;
};

/// The sensors the graph offers a series for, in @p sensors order: every enabled
/// sensor except the chamber's, which the graph shows as its chamber series.
[[nodiscard]] inline std::vector<TempGraphSensorSeries>
temp_graph_sensor_series(const std::vector<sensors::TemperatureSensorConfig>& sensors) {
    std::vector<TempGraphSensorSeries> out;
    for (const auto& sensor : sensors) {
        if (!sensor.enabled || sensor.role == sensors::TemperatureSensorRole::CHAMBER) {
            continue;
        }
        out.push_back({&sensor, temp_graph_sensor_group(sensor.role)});
    }
    return out;
}

/// The opt-in choices the graph remembers.
struct TempGraphSensorPrefs {
    bool show_mcu = false;
    bool show_host = false;
};

/**
 * @brief Whether a sensor series starts visible.
 *
 * @param all_series_view the graph-only view; the nozzle, bed and chamber views
 *        start with only their own heater
 */
[[nodiscard]] constexpr bool temp_graph_sensor_default_visible(TempGraphSensorGroup group,
                                                               bool all_series_view,
                                                               TempGraphSensorPrefs prefs) {
    switch (group) {
    case TempGraphSensorGroup::StepperDriver:
        return all_series_view;
    case TempGraphSensorGroup::Mcu:
        return prefs.show_mcu;
    case TempGraphSensorGroup::Host:
        return prefs.show_host;
    case TempGraphSensorGroup::Auxiliary:
        return false;
    }
    return false;
}

} // namespace helix
