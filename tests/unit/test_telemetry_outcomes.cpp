// SPDX-License-Identifier: GPL-3.0-or-later

// The fixed classes the outcome events send in place of the printer's words.

#include "system/telemetry_outcomes.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::telemetry::classify_failure;
using helix::telemetry::failure_class_name;
using helix::telemetry::FailureClass;
using helix::telemetry::FailureEvidence;

namespace {

std::string class_of(const char* text, bool failed = true, bool ams_fault = false,
                     bool klippy_down = false) {
    FailureEvidence e;
    e.text = text;
    e.failed = failed;
    e.filament_system_fault = ams_fault;
    e.klippy_down = klippy_down;
    return failure_class_name(classify_failure(e));
}

} // namespace

TEST_CASE("Failure class: Klipper's worded faults map to their class", "[telemetry][outcomes]") {
    CHECK(class_of("Heater extruder not heating at expected rate") == "thermal_runaway");
    CHECK(class_of("ADC out of range") == "heater");
    CHECK(class_of("MCU 'mcu' shutdown: Timer too close") == "mcu_shutdown");
    CHECK(class_of("Lost communication with MCU 'ebb36'") == "mcu_shutdown");
    CHECK(class_of("Shutdown due to M112 command") == "emergency_stop");
    CHECK(class_of("TMC 'stepper_x' reports error: GSTAT:      00000001 reset=1") == "driver");
    CHECK(class_of("No trigger on z after full movement") == "homing_probe");
    CHECK(class_of("Move out of range: 251.000 100.000 0.300 [12.400]") == "move");
    CHECK(class_of("Unknown command:\"FOO\"") == "gcode_error");
    CHECK(class_of("Filament runout") == "runout");
}

TEST_CASE("Failure class: a filament-system fault outranks soft errors, not hardware ones",
          "[telemetry][outcomes]") {
    // The macro error a stuck lane raises is the filament system's.
    CHECK(class_of("Unknown command:\"AFC_FOO\"", true, true) == "filament_system");
    CHECK(class_of("", true, true) == "filament_system");
    // A heater fault during an AMS error is still the heater.
    CHECK(class_of("Heater extruder not heating at expected rate", true, true) ==
          "thermal_runaway");
}

TEST_CASE("Failure class: unworded text falls back to keywords, then Klipper's state",
          "[telemetry][outcomes]") {
    CHECK(class_of("Filament Sensor runout_sensor: Runout Detected") == "runout");
    CHECK(class_of("Probe timed out waiting for response") == "timeout");
    CHECK(class_of("Something odd", true, false, true) == "klippy_shutdown");
    CHECK(class_of("Something odd") == "other");
    CHECK(class_of("", true, false, true) == "klippy_shutdown");
}

TEST_CASE("Failure class: no evidence is unknown for a failure and none for a cancel",
          "[telemetry][outcomes]") {
    CHECK(class_of("", true) == "unknown");
    CHECK(class_of("", false) == "none");
}
