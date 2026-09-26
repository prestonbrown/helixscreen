// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Automatic pressure-advance calibration.
//
// Stock Klipper has no automatic pressure-advance calibration: the documented
// method is TUNING_TOWER plus a human judging a printed part by eye. Some
// firmwares DO measure it - they extrude a test pattern while sweeping K,
// sense the flow mismatch, root-find the value that zeroes it, and report a
// number. That is the only kind of calibration this screen can drive, because
// the screen's whole premise is that the printer decides, not the user.
//
// This module is the ONLY place that knows which firmwares can do that, what
// command each one takes, and how to read its console output. Generic code -
// the panel, the capability subject, MoonrakerAdvancedAPI - asks these
// functions and never names a firmware.
//
// Adding a firmware means adding one Provider to the table in
// pa_calibration.cpp. No call site changes.

#include <cstdint>
#include <optional>
#include <string>

namespace helix {
class PrinterDiscovery;
}

namespace helix::pacal {

/// Everything the network layer needs to run one calibration and read its
/// result, with the firmware already resolved. Built by procedure_for(); the
/// API layer treats it as opaque configuration and names no firmware itself.
struct Procedure {
    /// Human-readable firmware name, for logs and the panel's provenance line.
    std::string provider;

    /// The fully-formed command to send, tool index already substituted.
    std::string start_gcode;

    /// The bare command word, for matching Klipper's "Unknown command: X"
    /// refusal - the one error that means "this firmware cannot do this".
    std::string command_word;

    /// ECMAScript regex whose first capture group is the measured K, matched
    /// against each console line.
    std::string result_pattern;

    /// ECMAScript regex whose first capture group is the current attempt
    /// number. Empty when the firmware reports no per-attempt progress.
    std::string attempt_pattern;

    /// How many attempts a typical run makes, for the progress display. 0 when
    /// the firmware does not report attempts.
    int expected_attempts = 0;

    /// RPC timeout. Generous: like PID_CALIBRATE, the console result line is
    /// the real authority and a slow heat-up must not read as a failure.
    uint32_t timeout_ms = 600000; // 10 min
};

/// Whether this printer can measure pressure advance on its own. False for the
/// overwhelming majority of Klipper machines, whose only option is a tuning
/// tower - which this screen deliberately does not pretend to offer.
bool is_supported(const PrinterDiscovery& hw);

/// Name of the matched firmware, or empty when unsupported. Logging only.
std::string provider_name(const PrinterDiscovery& hw);

/// Whether the calibration is addressed per-tool. False means the firmware
/// calibrates whatever tool is currently active, and the panel must not offer
/// a tool choice it cannot honour.
bool is_per_tool(const PrinterDiscovery& hw);

/// The procedure for calibrating `tool_index` on this printer, or nullopt when
/// the printer cannot do it. `tool_index` is ignored when !is_per_tool().
std::optional<Procedure> procedure_for(const PrinterDiscovery& hw, int tool_index);

/// The band a measured K is expected to land in on this extruder kind. Bowden
/// machines sit an order of magnitude above direct-drive ones, so "is this
/// value sane" is meaningless without knowing which this is - and it is the one
/// judgement the machine cannot make for the user.
struct SaneRange {
    /// The band a healthy result actually falls in, and the one quoted on
    /// screen. Two decimals, because that is how tuning guides state it.
    float low;
    float high;
    /// "direct drive" / "Bowden" - names the kind in the sanity sentence.
    const char* extruder_kind;
};

/// The expected band for this printer. Bowden machines sit an order of
/// magnitude higher than direct-drive ones, so the check is meaningless
/// without knowing which this is.
SaneRange sane_range(const PrinterDiscovery& hw);

/// Whether `k` falls inside sane_range() for this printer.
bool is_plausible(const PrinterDiscovery& hw, float k);

} // namespace helix::pacal
