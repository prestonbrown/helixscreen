// SPDX-License-Identifier: GPL-3.0-or-later
#include "system/telemetry_outcomes.h"

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

/// The class a hardware fault Klipper words falls in, else None.
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
        return FailureClass::None;
    }
}

/// The class of the softer faults Klipper words, else None.
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
        return FailureClass::None;
    }
}

} // namespace

const char* failure_class_name(FailureClass c) {
    switch (c) {
    case FailureClass::None:
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
        if (const FailureClass c = hardware_class(*worded); c != FailureClass::None) {
            return c;
        }
    }
    if (e.filament_system_fault) {
        return FailureClass::FilamentSystem;
    }
    if (worded) {
        if (const FailureClass c = soft_class(*worded); c != FailureClass::None) {
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
    return e.failed ? FailureClass::Unknown : FailureClass::None;
}

} // namespace helix::telemetry
