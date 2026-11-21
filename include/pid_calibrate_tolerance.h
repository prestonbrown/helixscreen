// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file pid_calibrate_tolerance.h
/// @brief How closely PID_CALIBRATE's cycles must agree before it finishes.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace helix {
class PrinterDiscovery;
}

namespace helix::pid {

/// The TOLERANCE to calibrate @p heater with, or nothing on a firmware whose
/// PID_CALIBRATE runs a fixed number of cycles. A bed gets a looser one than a
/// hotend: each of its cycles takes minutes, and its readings rarely settle to
/// the firmware's default.
[[nodiscard]] std::optional<double> calibrate_tolerance(const PrinterDiscovery& hw,
                                                        std::string_view heater);

/// The PID_CALIBRATE command for @p heater at @p target_c, with its TOLERANCE
/// where the firmware takes one.
[[nodiscard]] std::string calibrate_command(const PrinterDiscovery& hw, std::string_view heater,
                                            int target_c);

} // namespace helix::pid
