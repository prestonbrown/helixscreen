// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Where the head goes before a Z-offset calibration starts.
//
// Every calibration command takes its reading at the head's CURRENT XY:
// PROBE_CALIBRATE probes there, Z_ENDSTOP_CALIBRATE and the scan-probe
// calibrations start their paper test there. Klipper's docs ask for the head
// at Z >= 5 over the bed centre first, and a printer that is already homed
// sits wherever its last command left it (a Z tilt ends over a corner), so
// the approach is always sent, homed or not.
//
// Pure: no LVGL, no I/O.

#include "axis_move.h"
#include "printer_detector.h"
#include "printer_motion_state.h"
#include "printer_profile_state.h"
#include "probe_sensor_types.h"

#include <optional>
#include <string>
#include <string_view>

namespace helix::zoffset_cal {

/// Which part of the head the calibration reads at the starting XY.
enum class ReadingPoint {
    Nozzle, ///< Paper test at the nozzle (endstop, scan probes, manual gcode offset)
    Probe,  ///< Automatic probe at the probe's XY, then the nozzle moves over it
};

/// The command a strategy and probe run, and where it reads.
struct CalibrationCommand {
    std::string gcode; ///< Empty for FIRMWARE_MANAGED, which runs no calibrate command
    ReadingPoint point;
};

/// Whether @p probe_type is a scanning probe whose calibrate command builds its
/// own height model (Cartographer, Beacon, Klipper's probe_eddy_current).
bool is_scanning_probe(sensors::ProbeSensorType probe_type);

/**
 * @brief The strategy a calibration run on the Z-offset panel follows
 *
 * A scanning probe has to run its own calibrate command: its paper test and
 * coil sweep are the calibration, and a gcode offset cannot stand in for them.
 * ACCEPT stages the new model and SAVE_CONFIG alone keeps it, so the run never
 * folds the gcode offset into the probe, which is what FIRMWARE_MANAGED guards
 * against. A scanning probe therefore runs PROBE_CALIBRATE where the printer's
 * strategy is FIRMWARE_MANAGED (#1994). Every other pairing keeps @p strategy.
 */
ZOffsetCalibrationStrategy run_strategy(ZOffsetCalibrationStrategy strategy,
                                        sensors::ProbeSensorType probe_type);

/// @param chip the probe's sensor name (ProbeSensorConfig::sensor_name), which
///             names the chip for a probe whose calibrate command takes one
CalibrationCommand calibration_command(ZOffsetCalibrationStrategy strategy,
                                       sensors::ProbeSensorType probe_type,
                                       std::string_view chip = {});

/// Whether a calibration may start with the tool on the carriage.
enum class ToolGate {
    Proceed,
    ConfirmFirst, ///< The reference tool is not known to be mounted: ask before starting
};

/**
 * @brief Gate a calibration on the reference tool being mounted
 *
 * Where the probe's z_offset belongs to one tool (tool_offsets::
 * probe_reference_tool()), a calibrate command run with another tool mounted
 * measures the wrong nozzle on a rig with one shared probe. A rig with a probe
 * per tool, or a tool number remapped by ASSIGN_TOOL, calibrates correctly with
 * any tool, so the answer is a question for the user, never a refusal. An
 * unknown mounted tool (-1) asks too. FIRMWARE_MANAGED runs no calibrate command
 * of ours and always proceeds.
 *
 * @param reference_tool the tool number the probe offset belongs to, or nullopt
 * @param active_tool    the mounted tool number, -1 when none or unknown
 */
ToolGate reference_tool_gate(ZOffsetCalibrationStrategy strategy, std::optional<int> reference_tool,
                             int active_tool);

/// Approach height: Klipper's minimum clearance for starting a calibration.
inline constexpr double kApproachZMm = 5.0;

struct ApproachInputs {
    bool all_homed = false;
    AxisBounds plate;        ///< preset_area(), G-code space
    AxisBounds gcode_travel; ///< PrinterMotionState::get_gcode_axis_bounds()
    bool circular_bed = false;
    ReadingPoint point = ReadingPoint::Nozzle;
    std::optional<ProbeXYOffset> probe_offset; ///< nullopt reads as a probe at the nozzle
};

struct Approach {
    /// Homing when needed, then the lift and XY move. Every line ends in '\n'.
    std::string gcode;
    /// Nozzle target in G-code space; nullopt when the plate is unknown, in
    /// which case no lift or move is in `gcode`.
    std::optional<AxisTarget> nozzle;
};

/**
 * @brief G-code that homes if needed and puts the reading point over the plate centre
 *
 * The plate centre is motion_preset_target(Center). For ReadingPoint::Probe the
 * nozzle goes to centre minus the probe offset, so the probe lands on centre.
 * The nozzle target is clamped into G-code travel, which only moves the probe
 * off centre when its offset reaches past the edge of travel.
 */
Approach calibration_approach(const ApproachInputs& in);

} // namespace helix::zoffset_cal
