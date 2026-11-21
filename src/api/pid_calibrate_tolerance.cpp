// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pid_calibrate_tolerance.h"

#include <spdlog/fmt/fmt.h>

namespace helix::pid {

namespace {
// Kalico's own default (pid_calibrate.py TUNE_PID_TOL).
constexpr double kHotendTolerance = 0.02;
// Kalico's docs: noisy readings need 0.03 or more; a bed's cycles run for
// minutes each, so it settles for a looser fit.
constexpr double kBedTolerance = 0.05;
} // namespace

std::optional<double> calibrate_tolerance(bool takes_tolerance, std::string_view heater) {
    if (!takes_tolerance) {
        return std::nullopt;
    }
    return heater == "heater_bed" ? kBedTolerance : kHotendTolerance;
}

std::string calibrate_command(bool takes_tolerance, std::string_view heater, int target_c) {
    std::string cmd = fmt::format("PID_CALIBRATE HEATER={} TARGET={}", heater, target_c);
    if (const auto tolerance = calibrate_tolerance(takes_tolerance, heater)) {
        cmd += fmt::format(" TOLERANCE={:.2f}", *tolerance);
    }
    return cmd;
}

} // namespace helix::pid
