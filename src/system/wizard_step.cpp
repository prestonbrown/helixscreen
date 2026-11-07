// SPDX-License-Identifier: GPL-3.0-or-later
#include "wizard_step.h"

namespace helix::wizard {

const char* to_string(StepId id) {
    switch (id) {
    case StepId::TouchCalibration:
        return "TouchCalibration";
    case StepId::Language:
        return "Language";
    case StepId::Appearance:
        return "Appearance";
    case StepId::Wifi:
        return "Wifi";
    case StepId::Connection:
        return "Connection";
    case StepId::PrinterIdentify:
        return "PrinterIdentify";
    case StepId::HeaterSelect:
        return "HeaterSelect";
    case StepId::FanSelect:
        return "FanSelect";
    case StepId::AmsIdentify:
        return "AmsIdentify";
    case StepId::LedSelect:
        return "LedSelect";
    case StepId::FilamentSensor:
        return "FilamentSensor";
    case StepId::InputShaper:
        return "InputShaper";
    case StepId::Preferences:
        return "Preferences";
    case StepId::Summary:
        return "Summary";
    case StepId::Telemetry:
        return "Telemetry";
    }
    return "Unknown";
}

} // namespace helix::wizard
