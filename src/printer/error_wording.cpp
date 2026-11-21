// SPDX-License-Identifier: GPL-3.0-or-later
#include "error_wording.h"

#include "core/text_io.h"
#include "exception_policy.h"
#include "helix_regex.h"
#include "lvgl.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>

#include <array>

namespace helix::error_wording {

/// One message: the anchored, case-sensitive prefix of its reason line, an
/// optional second literal the reason must contain, and an optional capture
/// pattern that must match too. errmap-klipper.md is the source of every key.
struct Row {
    const char* id;
    const char* prefix;
    /// nullptr, or a literal the reason line must also contain.
    const char* contains;
    /// nullptr, or a helix::Regex whose groups become captures 1..n. The row
    /// matches only where it matches.
    const char* capture;
    /// Search @ref capture over the whole message rather than the reason line:
    /// for a detail Klipper adds on a later line.
    bool capture_in_body;
    Category category;
    Severity severity;
    Recovery recovery;
    /// English source; summary nullptr for a row that only classifies. "{0}"
    /// is the shutdown prefix's board, "{1}" on the row's captures.
    const char* summary;
    const char* cause;
    const char* fix;
};

namespace {

// Fixes several rows share.
constexpr const char* kHomeFix = TR_NOOP("Home all axes, then try again.");
constexpr const char* kEstopFix = TR_NOOP("When it is safe, restart the firmware.");
constexpr const char* kStepRateSummary = TR_NOOP("Motors were asked to step too fast");
constexpr const char* kStepRateCause =
    TR_NOOP("The controller board could not keep up with the step rate it was asked for, "
            "usually because high microsteps meet a high speed, or the host stalled.");
constexpr const char* kStepRateFix =
    TR_NOOP("Lower the microsteps or the maximum speed in the printer's configuration, then "
            "restart the firmware.");
constexpr const char* kAdcFix =
    TR_NOOP("Check the sensor's wiring and connector, and that the room is not colder than the "
            "sensor's minimum. Let the printer cool, then restart the firmware.");
constexpr const char* kTmcPowerSummary = TR_NOOP("Stepper driver {1} lost motor power");
constexpr const char* kTmcPowerCause =
    TR_NOOP("The driver for {1} saw its motor power drop or reset while the board kept running: "
            "the motor power supply is off, an emergency-stop relay is open, or a power "
            "connector is loose.");
constexpr const char* kTmcPowerFix =
    TR_NOOP("Check that motor power is on and its connectors are tight, then restart the "
            "firmware.");
constexpr const char* kHomeSummary = TR_NOOP("The printer needs homing first");
constexpr const char* kHomeCause =
    TR_NOOP("A move was sent before the axes were homed, often because the motors were turned "
            "off after the printer sat idle.");
constexpr const char* kMoveRangeSummary = TR_NOOP("Move would go past the printer's limits");
constexpr const char* kMoveRangeFix =
    TR_NOOP("Check that the model fits the bed and that the slicer's bed size matches this "
            "printer, and check macros that park or purge.");
constexpr const char* kBltouchCause =
    TR_NOOP("The BLTouch pin did not report the state the firmware expected: a bent or stuck "
            "pin, a self-test fault (blinking red), loose wiring, or a clone that needs its "
            "touch-mode options.");

using C = Category;
using S = Severity;
using R = Recovery;

// The generic rows: every Klipper and Kalico printer. Order matters where two
// rows share a prefix: the more specific comes first.
constexpr Row kKlipperRows[] = {
    // ---- MCU and communications ----
    {"MCU-1", "Timer too close", nullptr, nullptr, false, C::Mcu, S::FollowPrint,
     R::FirmwareRestart, TR_NOOP("The printer's computer fell behind"),
     TR_NOOP("The host computer did not send moves to the controller board in time, usually "
             "because it is overloaded: high CPU or swap use, a slow SD card, an unstable power "
             "supply, or a webcam stream."),
     TR_NOOP("Reduce the load on the host (stop webcams, timelapses and extra services), check "
             "its power supply and SD card, then restart the firmware. If it happens at the same "
             "spot again, lower the speed or the microsteps.")},
    {"MCU-2", "Missed scheduling of next ", nullptr, nullptr, false, C::Mcu, S::FollowPrint,
     R::FirmwareRestart, TR_NOOP("A controller board missed a timed update"),
     TR_NOOP("A heater or fan update from the host arrived too late. The USB or CAN connection to "
             "the board most likely dropped out for a moment, or the host stalled."),
     TR_NOOP("Check the USB or CAN cable and its termination, keep it away from motor wires, "
             "check the host's load, then restart the firmware.")},
    {"MCU-3", "Rescheduled timer in the past", nullptr, nullptr, false, C::Mcu, S::FollowPrint,
     R::FirmwareRestart, kStepRateSummary, kStepRateCause, kStepRateFix},
    {"MCU-3", "Stepper too far in past", nullptr, nullptr, false, C::Mcu, S::FollowPrint,
     R::FirmwareRestart, kStepRateSummary, kStepRateCause, kStepRateFix},
    {"MCU-4", "Lost communication with MCU '", nullptr, "^Lost communication with MCU '([^']+)'",
     false, C::Mcu, S::FollowPrint, R::FirmwareRestart, TR_NOOP("Lost connection to board '{1}'"),
     TR_NOOP("The host heard nothing from this board for several seconds: a loose or failed USB "
             "or CAN cable, the board losing power, or a CAN bus without termination."),
     TR_NOOP("Check the cable, connectors and power of that board (often the toolhead board) and "
             "the CAN termination, then restart the firmware.")},
    {"MCU-5", "MCU '", " spontaneous restart", "^MCU '([^']+)' spontaneous restart", false, C::Mcu,
     S::FollowPrint, R::FirmwareRestart, TR_NOOP("Board '{1}' restarted on its own"),
     TR_NOOP("The board rebooted by itself, usually from a dip in its power supply or a short on "
             "a fan or heater wire."),
     TR_NOOP("Check the board's power supply and look for damaged or shorted wiring, then restart "
             "the firmware.")},
    {"MCU-10", "Shutdown due to M112 command", nullptr, nullptr, false, C::EmergencyStop,
     S::FollowPrint, R::FirmwareRestart, TR_NOOP("Emergency stop"),
     TR_NOOP("An M112 emergency stop from the console, a macro or the slicer shut the printer "
             "down."),
     kEstopFix},
    {"MCU-11", "Shutdown due to webhooks request", nullptr, nullptr, false, C::EmergencyStop,
     S::FollowPrint, R::FirmwareRestart, TR_NOOP("Emergency stop pressed"),
     TR_NOOP("The emergency stop was pressed on a screen or in a web interface, and the printer "
             "shut down."),
     kEstopFix},
    {"MCU-13", "mcu '", "': Unable to connect", "^mcu '([^']+)': Unable to connect", false,
     C::McuConnect, S::FollowPrint, R::FirmwareRestart, TR_NOOP("Controller board '{1}' not found"),
     TR_NOOP("Klipper could not connect to this board at startup: a wrong serial path or CAN "
             "UUID in the configuration, the board unpowered or not flashed, or its cable "
             "unplugged."),
     TR_NOOP("Check the board's power and cable, and the serial or canbus_uuid setting in the "
             "printer's configuration, then restart the firmware.")},
    {"MCU-14", "MCU Protocol error", nullptr, nullptr, false, C::Mcu, S::FollowPrint,
     R::FirmwareRestart, TR_NOOP("Board firmware needs updating"),
     TR_NOOP("Klipper on the host was updated, and the firmware on one or more controller boards "
             "is older. Klipper's message below lists the boards."),
     TR_NOOP("Build and flash the firmware for each board listed, then restart the firmware.")},

    // ---- Heaters ----
    {"HT-1", "Heater ", " not heating at expected rate",
     "^Heater (.+) not heating at expected rate", false, C::Heater, S::FollowPrint,
     R::FirmwareRestart, TR_NOOP("Heater {1} is not heating as expected"),
     TR_NOOP("The heater did not warm up as fast as expected, or its temperature stayed below the "
             "target for too long. A loose heater or thermistor, a fan blowing on the heater "
             "block or a missing silicone sock can cause it."),
     TR_NOOP("Check the heater and thermistor wiring and that the thermistor is seated, keep fans "
             "off the heater block and run PID tuning again, then restart the firmware.")},
    {"HT-2", "ADC out of range", nullptr,
     "Sensor '([^']+)' temperature ([-0-9.?]+) not in range ([-0-9.]+):([-0-9.]+)", true, C::Heater,
     S::FollowPrint, R::FirmwareRestart, TR_NOOP("{1} temperature reading is out of range"),
     TR_NOOP("The {1} sensor read {2} °C, outside its allowed range of {3} to {4} °C, so the "
             "firmware stopped every heater to be safe."),
     kAdcFix},
    {"HT-2", "ADC out of range", nullptr, nullptr, false, C::Heater, S::FollowPrint,
     R::FirmwareRestart, TR_NOOP("Temperature reading is out of range"),
     TR_NOOP("A temperature sensor read a value outside its configured minimum or maximum, so the "
             "firmware stopped every heater to be safe."),
     kAdcFix},
    {"HT-4", "Requested temperature (", nullptr,
     "^Requested temperature \\(([-0-9.]+)\\) out of range \\(([-0-9.]+):([-0-9.]+)\\)", false,
     C::Heater, S::FollowPrint, R::None, TR_NOOP("Temperature outside the heater's limits"),
     TR_NOOP("A target of {1} °C was asked for, outside this heater's allowed range of {2} to "
             "{3} °C. Slicer start G-code for a hotter filament usually asks for it."),
     TR_NOOP("Lower the temperature in the slicer's filament profile, or raise max_temp only if "
             "the hotend is rated for it.")},
    // Kalico's pid_calibrate sends this as `!!` though it only warns.
    {"HT-K1", "Target calibration temperature of ", nullptr, nullptr, false, C::Heater, S::Warning,
     R::None, nullptr, nullptr, nullptr},

    // ---- Stepper drivers: the worst flag in the line decides the wording ----
    {"TMC-1-short", "TMC '", "(ShortTo", "^TMC '([^']+)' reports error: ", false, C::Driver,
     S::FollowPrint, R::FirmwareRestart, TR_NOOP("Stepper driver {1} detected a short"),
     TR_NOOP("The driver for {1} saw a short in the motor wiring: a damaged motor cable, "
             "connector or motor, or a failed driver."),
     TR_NOOP("Power the printer off, inspect the motor cable and connector for {1}, swap the "
             "motor to test it, then restart the firmware.")},
    {"TMC-1-overtemp", "TMC '", "(OvertempError", "^TMC '([^']+)' reports error: ", false,
     C::Driver, S::FollowPrint, R::FirmwareRestart, TR_NOOP("Stepper driver {1} overheated"),
     TR_NOOP("The driver for {1} got too hot, usually from a run current set too high or too "
             "little cooling over the drivers."),
     TR_NOOP("Let it cool, lower run_current for {1} or improve the cooling over the drivers, "
             "then restart the firmware.")},
    {"TMC-1-power", "TMC '", "(Undervoltage", "^TMC '([^']+)' reports error: ", false, C::Driver,
     S::FollowPrint, R::FirmwareRestart, kTmcPowerSummary, kTmcPowerCause, kTmcPowerFix},
    {"TMC-1-power", "TMC '", "(Reset", "^TMC '([^']+)' reports error: ", false, C::Driver,
     S::FollowPrint, R::FirmwareRestart, kTmcPowerSummary, kTmcPowerCause, kTmcPowerFix},
    {"TMC-1", "TMC '", "' reports error: ", "^TMC '([^']+)' reports error: ", false, C::Driver,
     S::FollowPrint, R::FirmwareRestart, TR_NOOP("Stepper driver {1} reported a fault"),
     TR_NOOP("The driver for {1} reported an error, and the firmware stopped the printer."),
     TR_NOOP("Check the motor cable, the driver's cooling and the motor power, then restart the "
             "firmware.")},
    {"TMC-2", "Unable to ", nullptr,
     "^Unable to (?:read|write) tmc (?:uart|spi) '([^']+)' register (\\w+)", false, C::Driver,
     S::FollowPrint, R::FirmwareRestart, TR_NOOP("Can't talk to stepper driver {1}"),
     TR_NOOP("The firmware could not read or write the driver for {1}. Motor power being off "
             "(UART drivers need it), a driver not seated or a wrong pin in the configuration "
             "can cause it."),
     TR_NOOP("Make sure motor power is on, reseat the driver and check its pin in the printer's "
             "configuration, then restart the firmware.")},

    // ---- Extrusion and moves ----
    {"EX-1", "Extrude below minimum temp", nullptr, nullptr, false, C::Move, S::FollowPrint,
     R::Retry, TR_NOOP("Nozzle too cold to extrude"),
     TR_NOOP("Filament can only move through the nozzle above its minimum extrusion temperature, "
             "and the nozzle is below it."),
     TR_NOOP("Heat the nozzle to the filament's printing temperature, wait for it to get there, "
             "then try again.")},
    {"EX-2", "Extrude only move too long", nullptr,
     "^Extrude only move too long \\(([0-9.]+)mm vs ([0-9.]+)mm\\)", false, C::Move, S::FollowPrint,
     R::Retry, TR_NOOP("Filament move too long"),
     TR_NOOP("One filament-only move of {1} mm is longer than the {2} mm the printer allows "
             "(max_extrude_only_distance)."),
     TR_NOOP("Load or unload in shorter steps, or raise max_extrude_only_distance in the extruder "
             "section. Check that the slicer and the start G-code agree on relative or absolute "
             "extrusion.")},
    {"EX-3", "Move exceeds maximum extrusion", nullptr,
     "^Move exceeds maximum extrusion \\(([0-9.]+)mm\\^2 vs ([0-9.]+)mm\\^2\\)", false, C::Move,
     S::FollowPrint, R::None, TR_NOOP("Too much filament for one move"),
     TR_NOOP("A move would push out far more filament than its length allows ({1} square mm "
             "against a limit of {2}), usually because the slicer and the start G-code disagree "
             "on relative or absolute extrusion."),
     TR_NOOP("Make the slicer's extrusion mode match the start G-code (relative, M83, is usual). "
             "Raise max_extrude_cross_section only for a deliberately thick purge line.")},
    {"MV-2", "Move out of range", nullptr,
     "^Move out of range: ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) \\[([-0-9.]+)\\]", false, C::Move,
     S::FollowPrint, R::None, kMoveRangeSummary,
     TR_NOOP("A move to X {1}, Y {2}, Z {3} is outside the printer's travel."), kMoveRangeFix},
    {"MV-2", "Move out of range", nullptr, nullptr, false, C::Move, S::FollowPrint, R::None,
     kMoveRangeSummary, TR_NOOP("A move went outside the printer's travel."), kMoveRangeFix},

    // ---- Homing and probing ----
    {"MV-1", "Must home axis first", nullptr, nullptr, false, C::Homing, S::FollowPrint, R::Home,
     kHomeSummary, kHomeCause, kHomeFix},
    {"MV-1", "Must home first", nullptr, nullptr, false, C::Homing, S::FollowPrint, R::Home,
     kHomeSummary, kHomeCause, kHomeFix},
    {"MV-3", "No trigger on ", nullptr, "^No trigger on (\\S+) after full movement", false,
     C::Homing, S::FollowPrint, R::None, TR_NOOP("{1} never triggered"),
     TR_NOOP("The axis moved its full travel without {1} triggering: an unplugged or broken "
             "switch, a wrong pin or inverted signal, a sensorless homing threshold set too low, "
             "or a probe that did not deploy."),
     TR_NOOP("Check the switch with QUERY_ENDSTOPS (or QUERY_PROBE) and its wiring and pin. For "
             "sensorless homing, tune the driver's stall threshold.")},
    {"MV-4", "Endstop ", " still triggered after retract",
     "^Endstop (\\S+) still triggered after retract", false, C::Homing, S::FollowPrint, R::None,
     TR_NOOP("Endstop {1} is stuck triggered"),
     TR_NOOP("After backing off, the {1} endstop still reads as triggered: the switch is stuck, "
             "its signal is inverted, or the retract distance is too short."),
     TR_NOOP("Move the axis away from the switch and run QUERY_ENDSTOPS. If it still reads "
             "TRIGGERED, invert the pin or replace the switch.")},
    {"PR-1", "Probe triggered prior to movement", nullptr, nullptr, false, C::Probe, S::FollowPrint,
     R::None, TR_NOOP("Probe already triggered"),
     TR_NOOP("The probe read as triggered before it started moving: it did not retract, the "
             "nozzle is touching the bed, or its signal is inverted."),
     TR_NOOP("Raise Z, then run QUERY_PROBE with the probe clear of the bed. If it still reads "
             "TRIGGERED, fix the probe or its pin inversion.")},
    {"PR-2", "Probe samples exceed", nullptr, nullptr, false, C::Probe, S::FollowPrint, R::Retry,
     TR_NOOP("Probe readings don't agree"),
     TR_NOOP("Repeated probe samples at one point differed by more than the allowed tolerance: "
             "ooze on the nozzle, a loose probe mount, a wobbly bed, or probing too fast."),
     TR_NOOP("Clean the nozzle, tighten the probe mount, lower the probing speed, or allow more "
             "retries (samples_tolerance_retries).")},
    {"PR-4", "BLTouch failed to ", nullptr, nullptr, false, C::Probe, S::FollowPrint, R::Retry,
     TR_NOOP("BLTouch did not respond"), kBltouchCause,
     TR_NOOP("Run BLTOUCH_DEBUG COMMAND=reset and check that the pin moves freely and the wiring "
             "is sound. Clones may need pin_up_touch_mode_reports_triggered set to False.")},
    {"PR-10", "Aborting quad_gantry_level required adjustment ", nullptr,
     "^Aborting quad_gantry_level required adjustment ([-0-9.]+) is greater than max_adjust "
     "([-0-9.]+)",
     false, C::Probe, S::FollowPrint, R::Retry, TR_NOOP("Gantry leveling failed"),
     TR_NOOP("The gantry needs a {1} mm correction, more than the {2} mm allowed. It is far out "
             "of level (after a crash, or sagging while powered off), or the probe points or the "
             "Z motor order are wrong."),
     TR_NOOP("With the power off, level the gantry roughly by hand, check the probe points and the "
             "Z motor order in the quad_gantry_level section, then try again.")},
    {"PR-11", "Retries aborting: Probed points range is increasing", nullptr, nullptr, false,
     C::Probe, S::FollowPrint, R::None, TR_NOOP("Leveling made things worse"),
     TR_NOOP("Each correction increased the difference between the probe points: the Z motors "
             "are mapped to the wrong corners, or the probe is not repeatable."),
     TR_NOOP("Check that stepper_z to stepper_z3 match their corners in the configuration, and "
             "check the probe with PROBE_ACCURACY.")},
    {"IS-1", "Invalid ", nullptr, "^Invalid (adxl345|lis2dw|lis3dh|mpu) id", false,
     C::Accelerometer, S::FollowPrint, R::Retry, TR_NOOP("Accelerometer not connected"),
     TR_NOOP("The {1} accelerometer did not answer with its expected ID. It is most likely "
             "unplugged, its cable is damaged, or its SPI pins are wrong."),
     TR_NOOP("Check the accelerometer cable and the cs_pin in its configuration section, then run "
             "ACCELEROMETER_QUERY.")},
    {"BM-1", "bed_mesh: Unknown profile [", nullptr, "^bed_mesh: Unknown profile \\[([^\\]]+)\\]",
     false, C::Command, S::FollowPrint, R::None, TR_NOOP("Bed mesh profile not found"),
     TR_NOOP("The bed mesh profile '{1}' was never saved on this printer."),
     TR_NOOP("Run a bed mesh calibration and save it, or fix the profile name in the start "
             "G-code.")},

    // ---- Configuration (startup) ----
    {"CF-1", "Option '", "' must be specified",
     "^Option '([^']+)' in section '([^']+)' must be specified", false, C::Config, S::FollowPrint,
     R::RestartAfterFix, TR_NOOP("A required setting is missing"),
     TR_NOOP("Section [{2}] needs the setting '{1}', and the configuration does not have it."),
     TR_NOOP("Add '{1}' under [{2}] in the printer's configuration, then restart Klipper.")},
    {"CF-2", "Option '", "' is not valid in section '",
     "^Option '([^']+)' is not valid in section '([^']+)'", false, C::Config, S::FollowPrint,
     R::RestartAfterFix, TR_NOOP("Unknown setting in the configuration"),
     TR_NOOP("Klipper does not know the setting '{1}' in section [{2}]: a typo, an option "
             "renamed in a Klipper update, or an option from another firmware."),
     TR_NOOP("Fix the spelling or remove the line from the printer's configuration, then restart "
             "Klipper.")},
    {"CF-3", "Section '", "' is not a valid config section",
     "^Section '([^']+)' is not a valid config section", false, C::Config, S::FollowPrint,
     R::RestartAfterFix, TR_NOOP("Unknown configuration section"),
     TR_NOOP("Klipper does not know the section [{1}]: a typo, or a plugin that is not "
             "installed."),
     TR_NOOP("Install the plugin it belongs to, or rename or remove the section, then restart "
             "Klipper.")},
    {"SC-1", "SAVE_CONFIG section '", "' conflicts with included value",
     "^SAVE_CONFIG section '([^']+)' option '([^']+)' conflicts with included value", false,
     C::Config, S::FollowPrint, R::None, TR_NOOP("Can't save: the value is set in another file"),
     TR_NOOP("SAVE_CONFIG wants to save '{2}' in section [{1}], but an included configuration "
             "file already sets it."),
     TR_NOOP("Remove or comment out '{2}' under [{1}] in the included file, then save again.")},

    // ---- Macros ----
    {"GC-5", "Error evaluating '", nullptr, "^Error evaluating '([^']+)': (\\w+): (.*)", false,
     C::Command, S::FollowPrint, R::None, TR_NOOP("Error in macro {1}"),
     TR_NOOP("The macro failed while running: {3}. A macro usually fails like this when it uses "
             "a parameter the slicer did not pass or an object the printer does not have."),
     TR_NOOP("Pass the missing parameter from the slicer's start G-code, or give it a default in "
             "the macro.")},
};

struct RowSet {
    const Row* rows;
    size_t count;
    /// nullptr: every printer. Otherwise only printers it detects.
    bool (*detect)(const PrinterDiscovery& hw);
};

const std::array<RowSet, 1> kRowSets{{
    {kKlipperRows, std::size(kKlipperRows), nullptr},
}};

bool starts_with(std::string_view s, std::string_view p) {
    return s.substr(0, p.size()) == p;
}

/// The message with its `!!` prefix, `MCU '<name>' shutdown: ` prefix and
/// Klipper's restart trailer removed.
struct Normalized {
    std::string mcu;
    std::string body;
    std::string reason;
};

Normalized normalize(std::string_view text) {
    Normalized n;
    std::string_view t = text_io::trim(text);
    if (starts_with(t, "!!")) {
        t = text_io::trim(t.substr(2));
    }
    for (std::string_view lead : {std::string_view("MCU '"), std::string_view("Previous MCU '")}) {
        if (!starts_with(t, lead)) {
            continue;
        }
        constexpr std::string_view kShutdown = "' shutdown: ";
        const size_t end = t.find(kShutdown, lead.size());
        const size_t eol = t.find('\n');
        if (end != std::string_view::npos && (eol == std::string_view::npos || end < eol)) {
            n.mcu = std::string(t.substr(lead.size(), end - lead.size()));
            t = t.substr(end + kShutdown.size());
        }
        break;
    }
    if (const size_t trailer = t.find("\nOnce the underlying issue is corrected");
        trailer != std::string_view::npos) {
        t = t.substr(0, trailer);
    }
    n.body = std::string(t);
    const size_t eol = t.find_first_of("\r\n");
    n.reason = std::string(text_io::trim(t.substr(0, eol)));
    return n;
}

bool row_matches(const Row& row, const Normalized& n, std::vector<std::string>& captures) {
    if (!starts_with(n.reason, row.prefix)) {
        return false;
    }
    if (row.contains && n.reason.find(row.contains) == std::string::npos) {
        return false;
    }
    captures.assign(1, n.mcu);
    if (!row.capture) {
        return true;
    }
    // Compiled here rather than cached: only a row whose prefix matched gets
    // this far, and errors are rare enough that a compile per match is cheap.
    const Regex re(row.capture);
    RegexMatch m;
    if (!regex_search(row.capture_in_body ? n.body : n.reason, m, re)) {
        return false;
    }
    for (size_t g = 1; g < m.size(); ++g) {
        captures.push_back(m.str(g));
    }
    return true;
}

/// A translated format string over the captures. A mistranslated placeholder
/// falls back to the English source rather than aborting.
std::string fill(const char* source, const std::vector<std::string>& captures) {
    std::array<std::string, 6> a;
    for (size_t i = 0; i < a.size() && i < captures.size(); ++i) {
        a[i] = captures[i];
    }
    const auto apply = [&a](const char* f) {
        return fmt::format(fmt::runtime(f), a[0], a[1], a[2], a[3], a[4], a[5]);
    };
    std::string out;
    if (!contain_exceptions("[ErrorWording] format", [&] { out = apply(lv_tr(source)); })) {
        contain_exceptions("[ErrorWording] format source", [&] { out = apply(source); });
    }
    return out;
}

} // namespace

std::optional<Classification> classify(std::string_view text, const PrinterDiscovery* hw) {
    const Normalized n = normalize(text);
    if (n.reason.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> captures;
    for (const RowSet& set : kRowSets) {
        if (set.detect && (!hw || !set.detect(*hw))) {
            continue;
        }
        for (size_t i = 0; i < set.count; ++i) {
            const Row& row = set.rows[i];
            if (!row_matches(row, n, captures)) {
                continue;
            }
            Classification c;
            c.id = row.id;
            c.category = row.category;
            c.severity = row.severity;
            c.recovery = row.recovery;
            c.captures = std::move(captures);
            c.reason = n.reason;
            c.worded = row.summary != nullptr;
            c.row = &row;
            return c;
        }
    }
    return std::nullopt;
}

std::string Worded::detail() const {
    return cause + "\n\n" + fix;
}

std::optional<Worded> word(const Classification& c) {
    if (!c.row || !c.row->summary) {
        return std::nullopt;
    }
    Worded w;
    w.summary = fill(c.row->summary, c.captures);
    w.cause = fill(c.row->cause, c.captures);
    w.fix = fill(c.row->fix, c.captures);
    return w;
}

std::string reason_summary(const std::string& text, const PrinterDiscovery* hw) {
    const auto c = classify(text, hw);
    const auto w = c ? word(*c) : std::nullopt;
    return w ? w->summary : text;
}

const char* journal_class(Category category) {
    switch (category) {
    case Category::Heater:
        return "heater";
    case Category::Mcu:
    case Category::McuConnect:
        return "mcu";
    case Category::Homing:
    case Category::Probe:
        return "homing";
    case Category::Move:
        return "move";
    case Category::Command:
        return "command";
    case Category::Filament:
        return "filament";
    case Category::Driver:
    case Category::Accelerometer:
    case Category::Config:
    case Category::EmergencyStop:
        return "other";
    }
    return "other";
}

} // namespace helix::error_wording
