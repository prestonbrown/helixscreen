// SPDX-License-Identifier: GPL-3.0-or-later

// The fixed classes the outcome events send in place of the printer's words.

#include "system/telemetry_outcomes.h"

#include <chrono>
#include <initializer_list>
#include <optional>
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

// ============================================================================
// filament_op [telemetry][filament_op]
// ============================================================================

namespace {

using helix::AmsAction;
using helix::telemetry::classify_filament_op_error;
using helix::telemetry::filament_op_error_name;
using helix::telemetry::FilamentOp;
using helix::telemetry::FilamentOpOutcome;
using helix::telemetry::FilamentOpTracker;

/// Feed @p actions one second apart, starting at t0; the last result.
std::optional<FilamentOpTracker::Finished>
run(FilamentOpTracker& t, std::initializer_list<AmsAction> actions, bool printing = false) {
    auto now = FilamentOpTracker::Clock::time_point{} + std::chrono::hours(1);
    std::optional<FilamentOpTracker::Finished> last;
    for (AmsAction a : actions) {
        last = t.on_action(a, now, printing);
        now += std::chrono::seconds(1);
    }
    return last;
}

} // namespace

TEST_CASE("Filament op tracker: actions make one operation, named by what moved",
          "[telemetry][filament_op]") {
    FilamentOpTracker t;
    auto load =
        run(t, {AmsAction::HEATING, AmsAction::LOADING, AmsAction::PURGING, AmsAction::IDLE});
    REQUIRE(load);
    CHECK(load->op == FilamentOp::Load);
    CHECK(load->outcome == FilamentOpOutcome::Success);
    CHECK(load->duration_sec == 3);

    auto unload = run(t, {AmsAction::CUTTING, AmsAction::UNLOADING, AmsAction::IDLE});
    REQUIRE(unload);
    CHECK(unload->op == FilamentOp::Unload);

    auto swap = run(
        t, {AmsAction::UNLOADING, AmsAction::SELECTING, AmsAction::LOADING, AmsAction::IDLE}, true);
    REQUIRE(swap);
    CHECK(swap->op == FilamentOp::ToolChange);
    CHECK(swap->during_print);

    // Selecting or heating alone, and the internal sensor check, move no filament.
    CHECK_FALSE(run(t, {AmsAction::SELECTING, AmsAction::IDLE}));
    CHECK_FALSE(run(t, {AmsAction::CHECKING, AmsAction::IDLE}));
    CHECK_FALSE(t.active());
}

TEST_CASE("Filament op tracker: an error or pause fails it, a cancel aborts it",
          "[telemetry][filament_op]") {
    FilamentOpTracker t;
    auto failed = run(t, {AmsAction::LOADING, AmsAction::ERROR});
    REQUIRE(failed);
    CHECK(failed->outcome == FilamentOpOutcome::Failed);
    // ERROR to IDLE afterwards is the fault clearing, not a second operation.
    CHECK_FALSE(run(t, {AmsAction::IDLE}));

    auto paused = run(t, {AmsAction::UNLOADING, AmsAction::PAUSED});
    REQUIRE(paused);
    CHECK(paused->outcome == FilamentOpOutcome::Failed);

    run(t, {AmsAction::IDLE, AmsAction::LOADING});
    t.note_cancel_requested();
    auto aborted = run(t, {AmsAction::IDLE});
    REQUIRE(aborted);
    CHECK(aborted->outcome == FilamentOpOutcome::Aborted);

    // A cancel asked for while nothing ran does not abort the next operation.
    t.note_cancel_requested();
    auto next = run(t, {AmsAction::LOADING, AmsAction::IDLE});
    REQUIRE(next);
    CHECK(next->outcome == FilamentOpOutcome::Success);
}

TEST_CASE("Filament op error class: the fault's text picks a fixed class",
          "[telemetry][filament_op]") {
    auto cls = [](const char* text) {
        return std::string(filament_op_error_name(classify_filament_op_error(text)));
    };
    CHECK(cls("Lane 2 jammed while loading") == "jam");
    CHECK(cls("Filament stuck at the hub") == "jam");
    CHECK(cls("Hub sensor did not trigger") == "hub");
    CHECK(cls("tool_start sensor not triggered after load") == "toolhead_sensor");
    CHECK(cls("Load timed out waiting for the gate sensor") == "sensor_timeout");
    CHECK(cls("Unit 1 is not responding") == "backend_error");
    CHECK(cls("") == "other");
}
