// SPDX-License-Identifier: GPL-3.0-or-later

#include "z_offset_calibration_approach.h"

#include "motion_presets.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

#include "hv/json.hpp"

namespace helix::zoffset_cal {

CalibrationCommand calibration_command(ZOffsetCalibrationStrategy strategy,
                                       sensors::ProbeSensorType probe_type) {
    switch (strategy) {
    case ZOffsetCalibrationStrategy::FIRMWARE_MANAGED:
        return {"", ReadingPoint::Nozzle};
    case ZOffsetCalibrationStrategy::ENDSTOP:
        return {"Z_ENDSTOP_CALIBRATE", ReadingPoint::Nozzle};
    case ZOffsetCalibrationStrategy::PROBE_CALIBRATE:
        break;
    }
    // Scan-probe calibrations start with a paper test at the nozzle, then
    // move the sensor over that point themselves.
    switch (probe_type) {
    case sensors::ProbeSensorType::CARTOGRAPHER:
        return {"CARTOGRAPHER_SCAN_CALIBRATE", ReadingPoint::Nozzle};
    case sensors::ProbeSensorType::BEACON:
        return {"BEACON_CALIBRATE", ReadingPoint::Nozzle};
    default:
        return {"PROBE_CALIBRATE", ReadingPoint::Probe};
    }
}

namespace {

/// Keeps the approach lift strictly inside a short Z axis at one decimal.
constexpr double kZTopMarginMm = 0.1;

double clamp_to(double v, bool known, float lo, float hi) {
    if (!known || lo >= hi) {
        return v;
    }
    return std::clamp(v, lo + kGcodeEdgeMarginMm, hi - kGcodeEdgeMarginMm);
}

} // namespace

Approach calibration_approach(const ApproachInputs& in) {
    Approach out;
    if (!in.all_homed) {
        out.gcode = "G28\n";
    }

    const auto centre = motion_preset_target(MotionPreset::Center, in.plate, in.circular_bed);
    if (!centre || !centre->x || !centre->y) {
        return out;
    }

    double x = *centre->x;
    double y = *centre->y;
    if (in.point == ReadingPoint::Probe && in.probe_offset) {
        x -= in.probe_offset->x;
        y -= in.probe_offset->y;
    }
    const auto& t = in.gcode_travel;
    x = clamp_to(x, t.has_x, t.x_min, t.x_max);
    y = clamp_to(y, t.has_y, t.y_min, t.y_max);

    double z = kApproachZMm;
    if (t.has_z && t.z_max > t.z_min) {
        z = std::min(z, static_cast<double>(t.z_max) - kZTopMarginMm);
    }

    out.nozzle = AxisTarget{x, y, std::nullopt};
    out.gcode += fmt::format("G90\nG1 Z{:.1f} F600\nG1 X{:.1f} Y{:.1f} F6000\n", z, x, y);
    return out;
}

} // namespace helix::zoffset_cal
