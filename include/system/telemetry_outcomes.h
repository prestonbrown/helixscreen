// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The fixed tokens the outcome events (print_outcome, filament_op,
// calibration_outcome) send in place of the printer's own words. Each event
// says why something ended without carrying the text that said so: the device
// reads Klipper's message, a filament system's fault or a step's failure and
// sends only the class it falls in. Pure; no LVGL.

#include <cstdint>
#include <string_view>

namespace helix {
class PrinterDiscovery;
} // namespace helix

namespace helix::telemetry {

/// Why a print or a calibration step failed.
enum class FailureClass : uint8_t {
    None,           ///< nothing failed, or a cancel nothing explains
    ThermalRunaway, ///< a heater that stopped heating at the expected rate
    Heater,         ///< any other heater or thermistor fault
    McuShutdown,    ///< an MCU shut down, restarted or stopped answering
    EmergencyStop,  ///< M112 or an emergency stop request
    Driver,         ///< a stepper driver fault
    HomingProbe,    ///< homing or probing failed
    Move,           ///< a move or extrude Klipper refused
    Accelerometer,  ///< an accelerometer Klipper could not read
    Config,         ///< a Klipper config error
    GcodeError,     ///< a command or macro error
    Runout,         ///< a filament sensor reported runout
    FilamentSystem, ///< the multi-material system raised a fault
    Timeout,        ///< something timed out
    KlippyShutdown, ///< Klipper left ready with nothing more specific said
    Other,          ///< a message none of the above covers
    Unknown,        ///< a failure with no message at all
};

/// The token sent for @p c: "thermal_runaway", "mcu_shutdown", ...
[[nodiscard]] const char* failure_class_name(FailureClass c);

/// What the device knows when a print or step ends.
struct FailureEvidence {
    /// Klipper's account of it: print_stats.message, else the state message
    /// of a Klipper that left ready, else the last `!!` line. Never sent.
    std::string_view text;
    /// The filament system was in a fault when it ended.
    bool filament_system_fault = false;
    /// Klipper was not ready when it ended.
    bool klippy_down = false;
    /// The job failed rather than being canceled: with no evidence a failure
    /// is Unknown and a cancel is None.
    bool failed = false;
};

/// The class of @p e. Hardware faults Klipper words outrank a filament-system
/// fault, which outranks softer errors; @p hw adds a firmware's own wording
/// rows (error_wording::classify()).
[[nodiscard]] FailureClass classify_failure(const FailureEvidence& e,
                                            const PrinterDiscovery* hw = nullptr);

} // namespace helix::telemetry
