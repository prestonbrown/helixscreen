// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Calibration results the printer's Klipper config already holds. A printer
// tuned before HelixScreen kept any history still has its Z offset, heater
// tuning, saved meshes and shaper in its config; the Calibration screen shows
// those, without a date, for steps that have no history entry.

#include "json_fwd.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace helix::calibration {

/// Step id to a short summary of the value found ("-1.250 mm", "PID").
using ConfigResults = std::map<std::string, std::string>;

/// The results in a configfile.settings object. Section names arrive
/// lowercased. A step whose value is unset, or at the value a stock config
/// starts from, is absent; heater tuning a vendor shipped is present.
[[nodiscard]] ConfigResults results_in_config(const nlohmann::json& settings);

// One spelling per measured value, shared by the config results above and the
// guided run's results, so a tile reads the same before and after a run.

/// A Z offset or endstop position: "-1.300 mm".
[[nodiscard]] std::string format_z_offset(double mm);
/// One axis's shaper: "MZV 36.7".
[[nodiscard]] std::string format_shaper_axis(std::string_view type, double freq_hz);
/// Both axes' shapers: "MZV 36.7 · EI 47.6 Hz"; an empty axis is left out.
[[nodiscard]] std::string join_shaper_axes(const std::string& x, const std::string& y);
/// PID gains to three significant figures: "22.1 / 1.08 / 113".
[[nodiscard]] std::string format_pid(double kp, double ki, double kd);
/// An MPC model by its block heat capacity: "MPC 18.4 J/K".
[[nodiscard]] std::string format_mpc(double block_heat_capacity);
/// A bed mesh by its flatness: "0.18 mm range".
[[nodiscard]] std::string format_mesh_range(double range_mm);
/// The range of a stored profile's points (rows of numbers), nullopt for none.
[[nodiscard]] std::optional<double> mesh_points_range(const nlohmann::json& points);
/// The Z value a calibration staged for SAVE_CONFIG, from
/// configfile.save_config_pending_items: a probe's z_offset (or its model's
/// offset), else stepper_z's position_endstop.
[[nodiscard]] std::optional<double> staged_z_offset(const nlohmann::json& pending_items);

} // namespace helix::calibration
