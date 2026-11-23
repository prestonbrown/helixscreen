// SPDX-License-Identifier: GPL-3.0-or-later
#include "system/telemetry_outcomes.h"

#include "ams_types.h"
#include "error_wording.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace helix::telemetry {

namespace {

/// @p haystack contains @p needle, ignoring ASCII case. @p needle is lower case.
bool contains_word(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    const auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
    return it != haystack.end();
}

/// The class a hardware fault Klipper words falls in, else NotFailed.
FailureClass hardware_class(const error_wording::Classification& c) {
    using error_wording::Category;
    switch (c.category) {
    case Category::Heater:
        return c.id == "HT-1" ? FailureClass::ThermalRunaway : FailureClass::Heater;
    case Category::Mcu:
    case Category::McuLost:
    case Category::McuConnect:
        return FailureClass::McuShutdown;
    case Category::EmergencyStop:
        return FailureClass::EmergencyStop;
    case Category::Driver:
        return FailureClass::Driver;
    case Category::Accelerometer:
        return FailureClass::Accelerometer;
    case Category::Config:
        return FailureClass::Config;
    default:
        return FailureClass::NotFailed;
    }
}

/// The class of the softer faults Klipper words, else NotFailed.
FailureClass soft_class(const error_wording::Classification& c) {
    using error_wording::Category;
    switch (c.category) {
    case Category::Filament:
        return FailureClass::Runout;
    case Category::Homing:
    case Category::Probe:
        return FailureClass::HomingProbe;
    case Category::Move:
        return FailureClass::Move;
    case Category::Command:
        return FailureClass::GcodeError;
    default:
        return FailureClass::NotFailed;
    }
}

} // namespace

const char* failure_class_name(FailureClass c) {
    switch (c) {
    case FailureClass::NotFailed:
        return "none";
    case FailureClass::ThermalRunaway:
        return "thermal_runaway";
    case FailureClass::Heater:
        return "heater";
    case FailureClass::McuShutdown:
        return "mcu_shutdown";
    case FailureClass::EmergencyStop:
        return "emergency_stop";
    case FailureClass::Driver:
        return "driver";
    case FailureClass::HomingProbe:
        return "homing_probe";
    case FailureClass::Move:
        return "move";
    case FailureClass::Accelerometer:
        return "accelerometer";
    case FailureClass::Config:
        return "config";
    case FailureClass::GcodeError:
        return "gcode_error";
    case FailureClass::Runout:
        return "runout";
    case FailureClass::FilamentSystem:
        return "filament_system";
    case FailureClass::Timeout:
        return "timeout";
    case FailureClass::KlippyShutdown:
        return "klippy_shutdown";
    case FailureClass::Other:
        return "other";
    case FailureClass::Unknown:
        return "unknown";
    }
    return "unknown";
}

FailureClass classify_failure(const FailureEvidence& e, const PrinterDiscovery* hw) {
    const auto worded = e.text.empty() ? std::nullopt : error_wording::classify(e.text, hw);
    if (worded) {
        if (const FailureClass c = hardware_class(*worded); c != FailureClass::NotFailed) {
            return c;
        }
    }
    if (e.filament_system_fault) {
        return FailureClass::FilamentSystem;
    }
    if (worded) {
        if (const FailureClass c = soft_class(*worded); c != FailureClass::NotFailed) {
            return c;
        }
    }
    if (contains_word(e.text, "runout")) {
        return FailureClass::Runout;
    }
    if (contains_word(e.text, "timeout") || contains_word(e.text, "timed out")) {
        return FailureClass::Timeout;
    }
    if (e.klippy_down) {
        return FailureClass::KlippyShutdown;
    }
    if (!e.text.empty()) {
        return FailureClass::Other;
    }
    return e.failed ? FailureClass::Unknown : FailureClass::NotFailed;
}

const char* calibration_outcome_name(CalibrationOutcome outcome) {
    switch (outcome) {
    case CalibrationOutcome::Completed:
        return "completed";
    case CalibrationOutcome::Aborted:
        return "aborted";
    case CalibrationOutcome::Failed:
        return "failed";
    }
    return "failed";
}

const char* filament_op_name(FilamentOp op) {
    switch (op) {
    case FilamentOp::Load:
        return "load";
    case FilamentOp::Unload:
        return "unload";
    case FilamentOp::ToolChange:
        return "tool_change";
    }
    return "load";
}

const char* filament_op_outcome_name(FilamentOpOutcome outcome) {
    switch (outcome) {
    case FilamentOpOutcome::Succeeded:
        return "success";
    case FilamentOpOutcome::Failed:
        return "failed";
    case FilamentOpOutcome::Aborted:
        return "aborted";
    }
    return "failed";
}

const char* filament_op_error_name(FilamentOpError error) {
    switch (error) {
    case FilamentOpError::NoError:
        return "none";
    case FilamentOpError::SensorTimeout:
        return "sensor_timeout";
    case FilamentOpError::Jam:
        return "jam";
    case FilamentOpError::Hub:
        return "hub";
    case FilamentOpError::ToolheadSensor:
        return "toolhead_sensor";
    case FilamentOpError::BackendError:
        return "backend_error";
    case FilamentOpError::Other:
        return "other";
    }
    return "other";
}

FilamentOpError classify_filament_op_error(std::string_view text) {
    if (text.empty()) {
        return FilamentOpError::Other;
    }
    for (std::string_view w : {"jam", "clog", "stuck"}) {
        if (contains_word(text, w)) {
            return FilamentOpError::Jam;
        }
    }
    if (contains_word(text, "hub")) {
        return FilamentOpError::Hub;
    }
    for (std::string_view w :
         {"toolhead", "tool_start", "tool_end", "extruder sensor", "tool sensor"}) {
        if (contains_word(text, w)) {
            return FilamentOpError::ToolheadSensor;
        }
    }
    for (std::string_view w :
         {"timeout", "timed out", "did not trigger", "not triggered", "not detected"}) {
        if (contains_word(text, w)) {
            return FilamentOpError::SensorTimeout;
        }
    }
    return FilamentOpError::BackendError;
}

std::optional<FilamentOpTracker::Finished>
FilamentOpTracker::on_action(AmsAction action, Clock::time_point now, bool printing) {
    switch (action) {
    case AmsAction::LOADING:
    case AmsAction::PURGING:
    case AmsAction::UNLOADING:
    case AmsAction::CUTTING:
    case AmsAction::FORMING_TIP:
    case AmsAction::SELECTING:
    case AmsAction::HEATING:
        if (!active_) {
            active_ = true;
            loaded_ = unloaded_ = cancel_requested_ = false;
            during_print_ = printing;
            start_ = now;
        }
        if (action == AmsAction::LOADING || action == AmsAction::PURGING) {
            loaded_ = true;
        } else if (action == AmsAction::UNLOADING || action == AmsAction::CUTTING ||
                   action == AmsAction::FORMING_TIP) {
            unloaded_ = true;
        }
        return std::nullopt;
    case AmsAction::CHECKING:
    case AmsAction::RESETTING:
        return std::nullopt;
    case AmsAction::IDLE:
    case AmsAction::ERROR:
    case AmsAction::PAUSED:
        break;
    }
    if (!active_) {
        return std::nullopt;
    }
    active_ = false;
    if (!loaded_ && !unloaded_) {
        return std::nullopt;
    }
    Finished f;
    f.op = loaded_ && unloaded_ ? FilamentOp::ToolChange
           : loaded_            ? FilamentOp::Load
                                : FilamentOp::Unload;
    f.outcome = cancel_requested_           ? FilamentOpOutcome::Aborted
                : action == AmsAction::IDLE ? FilamentOpOutcome::Succeeded
                                            : FilamentOpOutcome::Failed;
    f.duration_sec =
        static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - start_).count());
    f.during_print = during_print_;
    return f;
}

void FilamentOpTracker::note_cancel_requested() {
    if (active_) {
        cancel_requested_ = true;
    }
}

} // namespace helix::telemetry
