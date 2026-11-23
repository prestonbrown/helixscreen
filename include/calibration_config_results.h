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
#include <vector>

namespace helix::calibration {

/// Step id to a short summary of the value found ("-1.250 mm", "PID").
using ConfigResults = std::map<std::string, std::string>;
/// Heater name ("extruder", "heater_bed") to its tuned control: "pid" or "mpc".
using HeaterControls = std::map<std::string, std::string>;

/// The results in a configfile.settings object. Section names arrive
/// lowercased. A step whose value is unset, or at the value a stock config
/// starts from, is absent; heater tuning a vendor shipped is present.
[[nodiscard]] ConfigResults results_in_config(const nlohmann::json& settings);
/// Every heater in a configfile.settings object that runs a tuned control;
/// watermark has nothing to tune and is absent.
[[nodiscard]] HeaterControls heater_controls_in_config(const nlohmann::json& settings);

// One spelling per measured value, shared by the config results above and the
// guided run's results, so a tile reads the same before and after a run.

/// A Z offset or endstop position: "-1.300 mm".
[[nodiscard]] std::string format_z_offset(double mm);
/// One axis's shaper: "MZV 36.7".
[[nodiscard]] std::string format_shaper_axis(std::string_view type, double freq_hz);
/// Both axes' shapers: "X: MZV 36.7 · Y: EI 47.6"; an empty axis is left out.
[[nodiscard]] std::string join_shaper_axes(const std::string& x, const std::string& y);
/// A join_shaper_axes() result with one axis per line: "X: MZV 36.7\nY: EI 47.6".
/// Any other text comes back unchanged.
[[nodiscard]] std::string shaper_axes_on_lines(std::string_view joined);
/// PID gains to three significant figures: "22.1 / 1.08 / 113".
[[nodiscard]] std::string format_pid(double kp, double ki, double kd);
/// An MPC model by its block heat capacity: "MPC 18.4 J/K".
[[nodiscard]] std::string format_mpc(double block_heat_capacity);
/// One heater's tuning method, for the bed or the nozzle.
struct HeaterControl {
    bool bed = false;
    std::string control; ///< "pid" or "mpc"
};
/// Heaters' tuning in a few words, in @p heaters' order: "nozzle MPC · bed PID".
[[nodiscard]] std::string format_heater_controls(const std::vector<HeaterControl>& heaters);
/// A bed mesh by its flatness: "0.18 mm range".
[[nodiscard]] std::string format_mesh_range(double range_mm);
/// The range of a stored profile's points (rows of numbers), nullopt for none.
[[nodiscard]] std::optional<double> mesh_points_range(const nlohmann::json& points);
/// The Z value a calibration staged for SAVE_CONFIG, from
/// configfile.save_config_pending_items: a probe's z_offset (or its model's
/// offset), else stepper_z's position_endstop.
[[nodiscard]] std::optional<double> staged_z_offset(const nlohmann::json& pending_items);
/// PID gains as Klipper's PID_CALIBRATE staged them for @p heater.
struct PidGains {
    double kp = 0.0;
    double ki = 0.0;
    double kd = 0.0;
};
/// The gains PID_CALIBRATE staged for @p heater in
/// configfile.save_config_pending_items, or nothing when it staged none.
[[nodiscard]] std::optional<PidGains> staged_pid(const nlohmann::json& pending_items,
                                                 const std::string& heater);
/// The gains staged for @p heater in @p now that were not already pending in
/// @p before: an earlier, unsaved calibration's gains are not this one's.
[[nodiscard]] std::optional<PidGains> newly_staged_pid(const nlohmann::json& before,
                                                       const nlohmann::json& now,
                                                       const std::string& heater);

} // namespace helix::calibration
