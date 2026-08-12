// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_belt_hardware_detect.cpp
 * @brief Tests for MoonrakerAdvancedAPI::detect_belt_hardware's two-RPC chain
 *
 * detect_belt_hardware() drives two RPCs: printer.objects.list, then
 * printer.objects.query for kinematics. printer.objects.list's response is no
 * longer parsed for a hardware section list - accelerometer presence comes
 * from the injected PrinterState's PrinterDiscovery instead - so the #1137
 * guard against a non-string object list went with the parse it was
 * guarding. What remains to verify is that the chain still completes
 * cleanly: one of the two callbacks fires, and nothing escapes the callback
 * as an exception.
 *
 * hw.has_adxl used to read helix::sensors::AccelSensorManager::has_sensors(),
 * which is always false in production - nothing calls
 * SensorRegistry::discover_all()/register_manager() anywhere in the
 * codebase, so `sensors_` never gets populated. detect_belt_hardware always
 * reported "no accelerometer" even on a printer with a real [adxl345]
 * section. It now reads api_.printer_state().get_discovery().has_accelerometer(),
 * the same PrinterDiscovery-backed source the printer_has_accelerometer
 * subject uses.
 */

#include "../../include/belt_tension_types.h"
#include "../../include/moonraker_advanced_api.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_discovery.h"
#include "../../include/printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

// Builds a PrinterDiscovery whose configfile.config contains the given
// section keys and feeds it into `state` via set_hardware(), the same path
// production discovery takes. Mirrors
// WizardInputShaperStepTestFixture::set_has_accelerometer() in
// test_wizard_input_shaper_step.cpp.
void set_config_sections(PrinterState& state, const nlohmann::json& config) {
    PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json::array({"heater_bed", "extruder", "fan"}));
    hardware.parse_config_keys(config);
    state.set_hardware(hardware);
}

} // namespace

// Step 2 (printer.objects.query for kinematics) already reads through "result"
// correctly, but it cannot be asserted here: the mock's configfile.settings.printer
// carries only max_velocity/max_accel, with no kinematics key
// (moonraker_client_mock_objects.cpp:211). Covering that path needs the mock to
// report a per-printer-type kinematics first — worth doing, but it is mock
// fidelity work rather than part of this fix.

// A malformed envelope must not throw out of the callback or skip on_complete.
// The kinematics parse sits behind a try/catch that reports through
// on_error, so the contract is "one of the two callbacks fires, and nothing
// escapes".
TEST_CASE("detect_belt_hardware's two-RPC chain completes without escaping an exception",
          "[belt_tension][detect]") {
    PrinterState state;
    state.init_subjects(false);
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::GENERIC_COREXY);
    MoonrakerAPI api(client, state);
    MoonrakerAdvancedAPI advanced(client, api);

    bool completed = false;
    bool errored = false;
    REQUIRE_NOTHROW(advanced.detect_belt_hardware(
        [&](const helix::calibration::BeltTensionHardware&) { completed = true; },
        [&](const MoonrakerError&) { errored = true; }));
    CHECK((completed || errored));
}

// ============================================================================
// hw.has_adxl sourcing (regression: was permanently false via AccelSensorManager)
// ============================================================================

TEST_CASE("detect_belt_hardware reports has_adxl from a bare [adxl345] section",
          "[belt_tension][detect]") {
    PrinterState state;
    state.init_subjects(false);
    nlohmann::json config;
    config["adxl345"] = nlohmann::json::object();
    set_config_sections(state, config);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::GENERIC_COREXY);
    MoonrakerAPI api(client, state);
    MoonrakerAdvancedAPI advanced(client, api);

    bool completed = false;
    helix::calibration::BeltTensionHardware detected;
    advanced.detect_belt_hardware(
        [&](const helix::calibration::BeltTensionHardware& hw) {
            completed = true;
            detected = hw;
        },
        [&](const MoonrakerError&) {});
    REQUIRE(completed);
    CHECK(detected.has_adxl);
}

TEST_CASE("detect_belt_hardware reports has_adxl from a named [adxl345 hotend] section",
          "[belt_tension][detect]") {
    PrinterState state;
    state.init_subjects(false);
    nlohmann::json config;
    config["adxl345 hotend"] = nlohmann::json::object();
    set_config_sections(state, config);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::GENERIC_COREXY);
    MoonrakerAPI api(client, state);
    MoonrakerAdvancedAPI advanced(client, api);

    bool completed = false;
    helix::calibration::BeltTensionHardware detected;
    advanced.detect_belt_hardware(
        [&](const helix::calibration::BeltTensionHardware& hw) {
            completed = true;
            detected = hw;
        },
        [&](const MoonrakerError&) {});
    REQUIRE(completed);
    CHECK(detected.has_adxl);
}

TEST_CASE("detect_belt_hardware reports has_adxl from a [lis2dw] section",
          "[belt_tension][detect]") {
    PrinterState state;
    state.init_subjects(false);
    nlohmann::json config;
    config["lis2dw"] = nlohmann::json::object();
    set_config_sections(state, config);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::GENERIC_COREXY);
    MoonrakerAPI api(client, state);
    MoonrakerAdvancedAPI advanced(client, api);

    bool completed = false;
    helix::calibration::BeltTensionHardware detected;
    advanced.detect_belt_hardware(
        [&](const helix::calibration::BeltTensionHardware& hw) {
            completed = true;
            detected = hw;
        },
        [&](const MoonrakerError&) {});
    REQUIRE(completed);
    CHECK(detected.has_adxl);
}

TEST_CASE("detect_belt_hardware reports has_adxl false with no accelerometer section",
          "[belt_tension][detect]") {
    PrinterState state;
    state.init_subjects(false);
    // No adxl345/lis2dw/etc key at all - just unrelated sections.
    nlohmann::json config;
    config["printer"] = nlohmann::json::object({{"kinematics", "corexy"}});
    set_config_sections(state, config);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::GENERIC_COREXY);
    MoonrakerAPI api(client, state);
    MoonrakerAdvancedAPI advanced(client, api);

    bool completed = false;
    helix::calibration::BeltTensionHardware detected;
    advanced.detect_belt_hardware(
        [&](const helix::calibration::BeltTensionHardware& hw) {
            completed = true;
            detected = hw;
        },
        [&](const MoonrakerError&) {});
    REQUIRE(completed);
    CHECK_FALSE(detected.has_adxl);
}
