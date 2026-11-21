// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file pid_calibrate_tolerance.h
/// @brief How closely PID_CALIBRATE's cycles must agree before it finishes.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace helix::pid {

/// The TOLERANCE to calibrate @p heater with, or nothing when the firmware's
/// PID_CALIBRATE runs a fixed number of cycles (@p takes_tolerance false; ask
/// PrinterDiscovery::pid_calibrate_takes_tolerance()). A bed gets a looser one
/// than a hotend: each of its cycles takes minutes, and its readings rarely
/// settle to the firmware's default.
[[nodiscard]] std::optional<double> calibrate_tolerance(bool takes_tolerance,
                                                        std::string_view heater);

/// The PID_CALIBRATE command for @p heater at @p target_c, with its TOLERANCE
/// where the firmware takes one.
[[nodiscard]] std::string calibrate_command(bool takes_tolerance, std::string_view heater,
                                            int target_c);

} // namespace helix::pid
