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
// This module is the ONLY place that knows which firmwares can do that and
// how. Generic code - the panel, the capability subject, the connect steps -
// asks these functions and never names a firmware.
//
// Firmwares differ in who runs the procedure, so each table row names a kind:
//   - one firmware command that runs the whole procedure and prints the
//     result (Snapmaker U1 FLOW_CALIBRATE, Klipper4FlashForge FF_PA_CALIBRATE);
//   - a sweep this app drives step by step over commands the firmware only
//     forwards (stock FlashForge Creator 5: PA_ACTION / PA_GET).
// Every kind runs behind pacal::Run (pa_calibration_run.h). Adding a firmware
// of a known kind is one table row in pa_calibration.cpp; a new kind is one
// more case in start_run(). No call site changes.

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

    /// ECMAScript regex for a console line that ends the run without a
    /// measurement, in the firmware's own words. Empty when the firmware only
    /// fails through Klipper errors.
    std::string failure_pattern;

    /// Whether the firmware applies and keeps the measured K itself. When it
    /// does not, the number only matters once it is copied into a slicer.
    bool applies_result = false;

    /// Typical wall-clock length of a run, heat-up included, for the copy that
    /// sets expectations before it starts.
    int estimate_minutes = 3;

    /// RPC timeout. Generous: like PID_CALIBRATE, the console result line is
    /// the real authority and a slow heat-up must not read as a failure.
    uint32_t timeout_ms = 600000; // 10 min
};

/// What happens to the measured K once the run reports it.
enum class Kept : uint8_t {
    Nothing,     ///< reported only: it matters once it is copied into a slicer
    ThisSession, ///< live on the extruder until Klipper restarts
    Permanently, ///< the firmware applied it and saves it
};

/// What generic code needs to know about this printer's calibration, without
/// knowing which firmware it is.
struct Traits {
    std::string provider; ///< for logs
    bool per_tool = false;
    Kept kept = Kept::Nothing;
    /// The firmware runs the whole procedure as one command: it may set heater
    /// targets itself (the panel hands the heater over with
    /// CalibrationOwnedHeat::let_firmware_set), and a stop only stops this app
    /// listening while the firmware finishes. False when this app drives the
    /// procedure, stays the only one heating, and can end it between steps.
    bool firmware_runs_procedure = false;
    int expected_attempts = 0;
    int estimate_minutes = 3;
    /// How long a run may take before the panel gives up on it.
    uint32_t timeout_ms = 600000;
};

/// The traits of this printer's calibration, or nullopt when it has none.
std::optional<Traits> traits(const PrinterDiscovery& hw);

/// Whether this printer can measure pressure advance on its own. False for the
/// overwhelming majority of Klipper machines, whose only option is a tuning
/// tower - which this screen deliberately does not pretend to offer.
bool is_supported(const PrinterDiscovery& hw);

/// Name of the matched firmware, or empty when unsupported. Logging only.
std::string provider_name(const PrinterDiscovery& hw);

/// Whether each tool is calibrated separately, so the panel offers a tool
/// choice. False means the run covers the machine as a whole and a choice would
/// silently do nothing.
bool is_per_tool(const PrinterDiscovery& hw);

/// The procedure for calibrating `tool_index` at `temp_c` on this printer, or
/// nullopt when the printer cannot do it. Firmware that calibrates the mounted
/// tool ignores `tool_index`; the panel mounts the chosen tool before a run.
std::optional<Procedure> procedure_for(const PrinterDiscovery& hw, int tool_index, int temp_c);

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
