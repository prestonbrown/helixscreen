// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The fixed tokens the outcome events (print_outcome, filament_op,
// calibration_outcome) send in place of the printer's own words. Each event
// says why something ended without carrying the text that said so: the device
// reads Klipper's message, a filament system's fault or a step's failure and
// sends only the class it falls in. Pure; no LVGL.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace helix {
class PrinterDiscovery;
// ams_types.h is kept out: headers that reach X11 include this one.
enum class AmsAction;
} // namespace helix

namespace helix::telemetry {

/// Why a print or a calibration step failed.
enum class FailureClass : uint8_t {
    NotFailed,      ///< nothing failed, or a cancel nothing explains
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

/// How a calibration step ended, as calibration_outcome reports it.
enum class CalibrationOutcome : uint8_t { Completed, Aborted, Failed };
[[nodiscard]] const char* calibration_outcome_name(CalibrationOutcome outcome);

/// A load, unload or tool change, as filament_op reports it.
enum class FilamentOp : uint8_t { Load, Unload, ToolChange };
enum class FilamentOpOutcome : uint8_t { Succeeded, Failed, Aborted };

/// Why a filament operation failed.
enum class FilamentOpError : uint8_t {
    NoError,        ///< it did not fail
    SensorTimeout,  ///< a sensor never saw the filament arrive or leave
    Jam,            ///< a jam or clog
    Hub,            ///< the hub or its sensor
    ToolheadSensor, ///< the toolhead's filament sensor
    BackendError,   ///< the filament system reported a fault none of the above names
    Other,          ///< it failed with nothing said
};

[[nodiscard]] const char* filament_op_name(FilamentOp op);
[[nodiscard]] const char* filament_op_outcome_name(FilamentOpOutcome outcome);
[[nodiscard]] const char* filament_op_error_name(FilamentOpError error);

/// The class of a filament system's fault text @p text (never sent).
[[nodiscard]] FilamentOpError classify_filament_op_error(std::string_view text);

/// Turns the filament system's action edges into whole operations. An
/// operation starts at the first action that moves or prepares filament and
/// ends at IDLE (success), ERROR or PAUSED (failed), or either after a cancel
/// (aborted). One that unloaded and then loaded is a tool change. CHECKING,
/// and an operation that only selected or heated, report nothing.
class FilamentOpTracker {
  public:
    using Clock = std::chrono::steady_clock;

    struct Finished {
        FilamentOp op = FilamentOp::Load;
        FilamentOpOutcome outcome = FilamentOpOutcome::Succeeded;
        int duration_sec = 0;
        bool during_print = false;
    };

    /// Feed the action now published. @p printing: a print job is running.
    [[nodiscard]] std::optional<Finished> on_action(AmsAction action, Clock::time_point now,
                                                    bool printing);
    /// The user asked the filament system to cancel what it is doing.
    void note_cancel_requested();
    [[nodiscard]] bool active() const {
        return active_;
    }

  private:
    bool active_ = false;
    bool loaded_ = false;
    bool unloaded_ = false;
    bool cancel_requested_ = false;
    bool during_print_ = false;
    Clock::time_point start_{};
};

} // namespace helix::telemetry
