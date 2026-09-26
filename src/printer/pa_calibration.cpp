// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_calibration.h"

#include "printer_discovery.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace helix::pacal {
namespace {

/// One firmware that can measure pressure advance without a human judging a
/// printed part.
struct Provider {
    const char* name;

    /// Macro whose presence identifies the firmware. This is evidence, not a
    /// guess: a printer that does not define it cannot run the procedure, and
    /// the capability is reported absent rather than attempted and refused.
    /// Null when the firmware is detected by object instead.
    const char* detect_macro;

    /// Printer object whose presence identifies the firmware, for commands
    /// registered by a klippy Python extra: those never appear in the object
    /// list as "gcode_macro X". Null when the firmware is detected by macro.
    const char* detect_object;

    /// Command template. {tool} is the tool index and {temp} the nozzle
    /// temperature; a template may use either, both or neither.
    const char* gcode;

    /// Whether each tool is calibrated separately.
    bool per_tool;

    /// See Procedure: regexes over the firmware's console output.
    const char* result_pattern;
    const char* attempt_pattern;
    const char* failure_pattern;
    int expected_attempts;

    /// See Procedure.
    bool applies_result;
    int estimate_minutes;

    /// The extruder kind this firmware's machines use, which is what makes the
    /// plausibility band meaningful. Detected extruder geometry does not exist
    /// in PrinterDiscovery, and inferring it from kinematics would be a guess:
    /// the firmware entry knows its own hardware.
    SaneRange range;

    uint32_t timeout_ms;
};

/// Klipper acknowledges SET_PRESSURE_ADVANCE by echoing the applied value in
/// this shape. A firmware that installs candidates through that command prints
/// it once per candidate, so it can mark progress but never the result.
constexpr const char* KLIPPER_PA_ECHO = R"(pressure_advance:\s*([0-9]*\.?[0-9]+))";

/// Direct-drive extruders, the geometry on every machine below. The band is
/// the one every Klipper tuning guide quotes; a result outside it is not
/// rejected, only flagged as worth measuring again.
constexpr SaneRange DIRECT_DRIVE{0.02f, 0.08f, "direct drive"};

// --- Snapmaker U1 flow calibrator --------------------------------------------
//
// The U1 carries an eddy-current inductance coil per toolhead and uses it to
// measure extrusion back-pressure directly. FLOW_CALIBRATE (the U1's
// klippy/extras/flow_calibrator.py) purges in a repeating slow/fast pattern for
// each candidate K on the MOUNTED extruder, asks its native solver for the
// signed flow mismatch, root-finds the zero crossing through about five points,
// then applies the winner and saves it to flow_calibrator.json. It sets K
// through the extruder stepper directly, so no SET_PRESSURE_ADVANCE echo ever
// reaches the console. Its own lines, from that source:
//   measure k: 0.02000           once per candidate
//   Got pressure advance: 0.0412 the result
//   flow k is out of range, use default value:0.02
//   abort calibration: <reason>
//
// SM_PRINT_FLOW_CALIBRATE is the print-job wrapper around it and returns
// without a word outside a print, so it cannot drive this screen.
//
// The flow calibrator has no status object, so the firmware is recognised by
// filament_parameters, the Snapmaker extra it cannot run without.
//
// Snapmaker calls this "flow calibration", but the number it computes IS the
// Klipper pressure-advance K. See docs/devel/printers/SNAPMAKER_U1_SUPPORT.md
// § Pre-Print Flow Calibration.
constexpr Provider SNAPMAKER_U1{
    "Snapmaker U1 flow calibrator",
    /*detect_macro=*/nullptr,
    "filament_parameters",
    "FLOW_CALIBRATE TEMP={temp}",
    /*per_tool=*/true,
    R"(Got pressure advance:\s*([0-9]*\.?[0-9]+))",
    R"(measure k:\s*([0-9]*\.?[0-9]+))",
    R"(flow k is out of range|abort calibration:)",
    /*expected_attempts=*/5,
    /*applies_result=*/true,
    /*estimate_minutes=*/3,
    DIRECT_DRIVE,
    /*timeout_ms=*/600000, // 10 min: several purge cycles plus the heat-up
};

// --- FlashForge eBoard PA calibrator -----------------------------------------
//
// The Creator 5 Pro's closed eBoard MCU scores each candidate K live from a
// transducer stream while FF_PA_CALIBRATE (klippy/extras/ff_pa.py in our
// firmware repo) replays the stock app's sweep: seven scrambled candidates per
// pass, smallest passing verdict wins, mean of three pass winners. The command
// reports, it does not apply:
//   ff_pa: T0 pressure_advance = 0.021667   (mean of 3 sweep winners: ...)
//
// The '=' result shape is deliberate. The sweep installs every candidate via
// SET_PRESSURE_ADVANCE, whose "pressure_advance: <k>" echo would satisfy the
// ':' shape on the FIRST candidate, minutes before the real result. Matching
// '=' skips those echoes, and they double as the per-candidate progress
// lines, each carrying the K just tried.
//
// FF_PA_CALIBRATE is registered by the [ff_pa] Python extra, so it appears in
// the object list as "ff_pa", never as a gcode_macro: hence detect_object.
//
// The sweep structurally cannot report outside its candidate table, so the
// plausibility band is that table's span (defaults 0.0100..0.0400).
constexpr SaneRange FF_CANDIDATE_SPAN{0.01f, 0.04f, "direct drive"};

constexpr Provider FLASHFORGE_EBOARD{
    "FlashForge eBoard PA calibrator",
    /*detect_macro=*/nullptr,
    "ff_pa",
    "FF_PA_CALIBRATE TOOL={tool}",
    /*per_tool=*/true,
    R"(pressure_advance\s*=\s*([0-9]*\.?[0-9]+))",
    KLIPPER_PA_ECHO,
    /*failure_pattern=*/"",
    /*expected_attempts=*/21, // 3 winning sweeps x 7 candidates, typically
    /*applies_result=*/false,
    /*estimate_minutes=*/5, // up to 5 sweeps of about a minute, plus heat-up
    FF_CANDIDATE_SPAN,
    /*timeout_ms=*/600000, // 10 min: up to 5 sweeps (~1 min each) plus heat-up
};

/// The table. One entry per firmware that can do this.
constexpr const Provider* PROVIDERS[] = {&SNAPMAKER_U1, &FLASHFORGE_EBOARD};

const Provider* match(const PrinterDiscovery& hw) {
    for (const Provider* p : PROVIDERS) {
        if (p->detect_macro && hw.has_macro(p->detect_macro)) {
            return p;
        }
        if (p->detect_object) {
            const auto& objects = hw.printer_objects();
            if (std::find(objects.begin(), objects.end(), p->detect_object) != objects.end()) {
                return p;
            }
        }
    }
    return nullptr;
}

} // namespace

bool is_supported(const PrinterDiscovery& hw) {
    return match(hw) != nullptr;
}

std::string provider_name(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->name : std::string{};
}

bool is_per_tool(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->per_tool;
}

std::optional<Procedure> procedure_for(const PrinterDiscovery& hw, int tool_index, int temp_c) {
    const Provider* p = match(hw);
    if (!p) {
        return std::nullopt;
    }

    Procedure proc;
    proc.provider = p->name;
    proc.start_gcode =
        fmt::format(fmt::runtime(p->gcode), fmt::arg("tool", tool_index), fmt::arg("temp", temp_c));
    proc.result_pattern = p->result_pattern;
    proc.attempt_pattern = p->attempt_pattern ? p->attempt_pattern : "";
    proc.failure_pattern = p->failure_pattern ? p->failure_pattern : "";
    proc.expected_attempts = p->expected_attempts;
    proc.applies_result = p->applies_result;
    proc.estimate_minutes = p->estimate_minutes;
    proc.timeout_ms = p->timeout_ms;

    // The command word is the first token of the template, which is also what
    // Klipper quotes back in "Unknown command:".
    proc.command_word = proc.start_gcode.substr(0, proc.start_gcode.find(' '));

    return proc;
}

SaneRange sane_range(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->range : DIRECT_DRIVE;
}

bool is_plausible(const PrinterDiscovery& hw, float k) {
    const SaneRange r = sane_range(hw);
    return k >= r.low && k <= r.high;
}

} // namespace helix::pacal
