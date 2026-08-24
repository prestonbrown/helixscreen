// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// #1160: b73781ca8 made it possible to finish the setup wizard while Klipper is
// in `error` — necessary, because that state used to be an inescapable dead end.
// But discovery never runs in that state, so every hardware picker is empty and
// the user selects nothing. ui_wizard_complete() then committed that emptiness
// as the printer's expected-hardware snapshot, and the first boot where Klipper
// DID come up compared live discovery against an empty list and reported every
// fan, filament sensor and LED as newly appeared.
//
// The snapshot is deferred instead: a per-printer marker records the debt, the
// first successful discovery accepts what is there as expected, and the user is
// offered the hardware steps their broken printer made them skip.
//
// The LVGL-side glue (ui_wizard_complete, the modal, the targeted session) is
// covered by the wizard UI tests; what is exercised here is the decision itself,
// the per-printer storage, and the discovery-side snapshot write.

#include "../helix_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "hardware_validator.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "wizard_step.h"
#include "wizard_step_logic.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using helix::wizard::StepId;
using namespace helix;

namespace {

// Two configured printers, "printer-a" active — the shape that catches a marker
// stored at the config root instead of under df() (cf. #1162 for `preset`).
class DeferredHardwareFixture : public HelixTestFixture {
  protected:
    Config config;
    MoonrakerClientMock client;

    DeferredHardwareFixture() {
        ConfigTestAccess::data(config) = {{"config_version", CURRENT_CONFIG_VERSION},
                                          {"active_printer_id", "printer-a"},
                                          {"printers",
                                           {{"printer-a", {{"moonraker_host", "192.168.1.10"}}},
                                            {"printer-b", {{"moonraker_host", "192.168.1.11"}}}}}};
        ConfigTestAccess::active_printer_id(config) = "printer-a";
        // In-memory only: an empty path makes save() a no-op, so nothing here
        // touches the real settings.json.
        ConfigTestAccess::path(config) = "";
    }

    std::vector<std::string> expected_hardware() {
        return config.get_string_array(config.df() + "hardware/expected");
    }

    bool has_expected_key() {
        return ConfigTestAccess::data(config).contains(
            json::json_pointer(config.df() + "hardware/expected"));
    }

    bool marker_key_present() {
        return ConfigTestAccess::data(config).contains(
            json::json_pointer(config.df() + helix::WIZARD_HARDWARE_SETUP_DEFERRED));
    }

    static std::vector<helix::StepSkip> all_visible() {
        std::vector<helix::StepSkip> v;
        for (int i = 0; i < helix::wizard::STEP_COUNT; ++i) {
            v.push_back({static_cast<StepId>(i), false});
        }
        return v;
    }

    static void skip(std::vector<helix::StepSkip>& v, StepId id) {
        for (auto& e : v) {
            if (e.id == id) {
                e.skipped = true;
            }
        }
    }

    static bool contains(const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    }

    static bool contains(const std::vector<StepId>& v, StepId s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    }
};

// ============================================================================
// (a) A wizard that finished with Klipper down must not commit an empty snapshot
// ============================================================================

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: Klipper-down completion records a debt instead of an empty "
                 "snapshot",
                 "[wizard][hardware][deferred]") {
    // Discovery never ran and the pickers produced nothing.
    const bool deferred = helix::wizard_apply_hardware_snapshot_decision(
        &config, /*discovery_succeeded=*/false, /*snapshot_has_entries=*/false);

    REQUIRE(deferred);
    CHECK(helix::wizard_hardware_setup_deferred(&config));
    // Nothing was committed — the snapshot is owed, not written empty.
    CHECK_FALSE(has_expected_key());
}

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: a preset-seeded Klipper-down run is NOT deferred",
                 "[wizard][hardware][deferred][preset]") {
    // A preset supplies real hardware names even with Klipper unreachable, so
    // the snapshot it produces is meaningful and final. Deferring here would
    // wrongly re-offer the hardware steps a preset already answered.
    const bool deferred = helix::wizard_apply_hardware_snapshot_decision(
        &config, /*discovery_succeeded=*/false, /*snapshot_has_entries=*/true);

    CHECK_FALSE(deferred);
    CHECK_FALSE(helix::wizard_hardware_setup_deferred(&config));
}

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: the raw predicate covers all four "
                 "input combinations",
                 "[wizard][hardware][deferred]") {
    // Deferral is the ONE case where discovery failed and nothing was recorded.
    CHECK(helix::wizard_hardware_snapshot_is_deferred(false, false));
    CHECK_FALSE(helix::wizard_hardware_snapshot_is_deferred(false, true));
    CHECK_FALSE(helix::wizard_hardware_snapshot_is_deferred(true, false));
    CHECK_FALSE(helix::wizard_hardware_snapshot_is_deferred(true, true));
}

// ============================================================================
// (b) The marker is per-printer
// ============================================================================

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: the marker is per-printer, not per-config",
                 "[wizard][hardware][deferred][multi-printer]") {
    REQUIRE(helix::wizard_apply_hardware_snapshot_decision(&config, false, false));
    REQUIRE(helix::wizard_hardware_setup_deferred(&config));

    // A second printer, set up normally, inherits nothing.
    REQUIRE(config.set_active_printer("printer-b"));
    CHECK_FALSE(helix::wizard_hardware_setup_deferred(&config));

    // ...and settling the second printer's (absent) debt must not settle the
    // first one's. A root-level flag would clear both.
    CHECK_FALSE(helix::wizard_clear_hardware_setup_deferred(&config));
    REQUIRE(config.set_active_printer("printer-a"));
    CHECK(helix::wizard_hardware_setup_deferred(&config));

    // Storage lives under the printer node, with nothing at the root.
    CHECK(config.get<bool>(
        std::string("/printers/printer-a/") + helix::WIZARD_HARDWARE_SETUP_DEFERRED, false));
    CHECK_FALSE(ConfigTestAccess::data(config).contains(
        std::string(helix::WIZARD_HARDWARE_SETUP_DEFERRED)));
}

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: settling one printer leaves the other's debt standing",
                 "[wizard][hardware][deferred][multi-printer]") {
    REQUIRE(helix::wizard_apply_hardware_snapshot_decision(&config, false, false));
    REQUIRE(config.set_active_printer("printer-b"));
    REQUIRE(helix::wizard_apply_hardware_snapshot_decision(&config, false, false));

    CHECK(helix::wizard_clear_hardware_setup_deferred(&config));
    CHECK_FALSE(helix::wizard_hardware_setup_deferred(&config));

    REQUIRE(config.set_active_printer("printer-a"));
    CHECK(helix::wizard_hardware_setup_deferred(&config));
}

// ============================================================================
// (c) A normally-completed wizard sets no marker and is never prompted
// ============================================================================

TEST_CASE_METHOD(DeferredHardwareFixture, "Deferred hardware: a normal completion sets no marker",
                 "[wizard][hardware][deferred]") {
    const bool deferred = helix::wizard_apply_hardware_snapshot_decision(
        &config, /*discovery_succeeded=*/true, /*snapshot_has_entries=*/true);

    CHECK_FALSE(deferred);
    CHECK_FALSE(helix::wizard_hardware_setup_deferred(&config));
    // Nothing is written at all, so the discovery-side offer never sees a debt.
    CHECK_FALSE(marker_key_present());
}

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: reaching Klipper and picking nothing is still a final answer",
                 "[wizard][hardware][deferred]") {
    // Discovery ran, so the pickers had real lists — declining every one of
    // them is a deliberate choice, not a debt.
    CHECK_FALSE(helix::wizard_apply_hardware_snapshot_decision(&config, true, false));
    CHECK_FALSE(helix::wizard_hardware_setup_deferred(&config));
    CHECK_FALSE(marker_key_present());
}

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: a --wizard re-run that reaches Klipper settles the debt",
                 "[wizard][hardware][deferred]") {
    REQUIRE(helix::wizard_apply_hardware_snapshot_decision(&config, false, false));
    REQUIRE(helix::wizard_hardware_setup_deferred(&config));

    // Second run, Klipper up, user picks hardware.
    CHECK_FALSE(helix::wizard_apply_hardware_snapshot_decision(&config, true, true));
    CHECK_FALSE(helix::wizard_hardware_setup_deferred(&config));
}

// ============================================================================
// (d) The snapshot IS written on the first successful discovery
// ============================================================================

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: first successful discovery writes the owed snapshot",
                 "[wizard][hardware][deferred]") {
    client.set_heaters({"extruder", "heater_bed"});
    client.set_fans({"fan", "heater_fan hotend_fan", "controller_fan board_fan"});
    client.set_leds({"neopixel case_lights"});
    client.set_filament_sensors({"filament_switch_sensor runout"});

    const size_t accepted =
        HardwareValidator::acknowledge_discovered_hardware(&config, client.hardware());

    CHECK(accepted > 0);
    auto expected = expected_hardware();
    CHECK(contains(expected, "fan"));
    CHECK(contains(expected, "heater_fan hotend_fan"));
    CHECK(contains(expected, "controller_fan board_fan"));
    CHECK(contains(expected, "neopixel case_lights"));
    CHECK(contains(expected, "filament_switch_sensor runout"));
}

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: the written snapshot silences the new-hardware flood",
                 "[wizard][hardware][deferred]") {
    client.set_heaters({"extruder", "heater_bed"});
    client.set_fans({"fan", "heater_fan hotend_fan", "controller_fan board_fan"});
    client.set_leds({"neopixel case_lights"});
    client.set_filament_sensors({"filament_switch_sensor runout"});

    // Config as a Klipper-down wizard leaves it: no role keys, no LED, no
    // sensors, no expected list. This is the state that produced the flood.
    HardwareValidator before;
    auto noisy = before.validate(&config, client.hardware());
    REQUIRE_FALSE(noisy.newly_discovered.empty());

    HardwareValidator::acknowledge_discovered_hardware(&config, client.hardware());

    HardwareValidator after;
    auto quiet = after.validate(&config, client.hardware());
    CHECK(quiet.newly_discovered.empty());
}

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: accepting discovery is idempotent and adds no duplicates",
                 "[wizard][hardware][deferred]") {
    client.set_heaters({"extruder"});
    client.set_fans({"fan"});
    client.set_leds({"neopixel case_lights"});
    // Pin all three accepted categories. The mock derives filament sensors from
    // its default printer objects, so leaving this unset lets a sensor the test
    // never asked for into the count.
    client.set_filament_sensors({});

    const size_t first =
        HardwareValidator::acknowledge_discovered_hardware(&config, client.hardware());
    REQUIRE(first == 2);

    // A reconnect re-runs discovery; nothing new must be appended.
    const size_t second =
        HardwareValidator::acknowledge_discovered_hardware(&config, client.hardware());
    CHECK(second == 0);

    auto expected = expected_hardware();
    CHECK(expected.size() == 2);
}

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: accepting discovery preserves names already recorded",
                 "[wizard][hardware][deferred]") {
    HardwareValidator::add_expected_hardware(&config, "AFC");
    client.set_heaters({"extruder"});
    client.set_fans({"fan"});

    HardwareValidator::acknowledge_discovered_hardware(&config, client.hardware());

    auto expected = expected_hardware();
    CHECK(contains(expected, "AFC"));
    CHECK(contains(expected, "fan"));
}

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: accepting discovery never records AMS capability names",
                 "[wizard][hardware][deferred][ams]") {
    // validate_expected_hardware() treats "mmu"/"AFC"/"ace"/"toolchanger" in the
    // expected list as must-be-present and warns every boot if the system goes
    // away. Only the wizard's AMS step — where the user confirms the hardware —
    // may add those; blanket acceptance must not. The backend's own conventional
    // sensors are excluded too, matching validate_new_hardware().
    client.set_heaters({"extruder"});
    client.set_fans({"fan"});
    client.set_additional_objects({"mmu"});
    client.set_filament_sensors({"filament_switch_sensor extruder",
                                 "filament_switch_sensor toolhead",
                                 "filament_switch_sensor runout"});

    HardwareValidator::acknowledge_discovered_hardware(&config, client.hardware());

    auto expected = expected_hardware();
    CHECK_FALSE(contains(expected, "mmu"));
    CHECK_FALSE(contains(expected, "AFC"));
    CHECK_FALSE(contains(expected, "toolchanger"));
    CHECK_FALSE(contains(expected, "filament_switch_sensor extruder"));
    CHECK_FALSE(contains(expected, "filament_switch_sensor toolhead"));
    // The standalone runout switch is not AMS-managed and must be recorded.
    CHECK(contains(expected, "filament_switch_sensor runout"));
}

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: accepting discovery tolerates a null config",
                 "[wizard][hardware][deferred]") {
    client.set_fans({"fan"});
    CHECK(HardwareValidator::acknowledge_discovered_hardware(nullptr, client.hardware()) == 0);
}

// ============================================================================
// The re-run offer: which steps it presents
// ============================================================================

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: the offer runs the hardware steps in wizard order",
                 "[wizard][hardware][deferred][steps]") {
    auto steps = helix::wizard_deferred_hardware_steps(all_visible());

    REQUIRE(steps.size() == 7);
    CHECK(steps[0] == StepId::PrinterIdentify);
    CHECK(steps[1] == StepId::HeaterSelect);
    CHECK(steps[2] == StepId::FanSelect);
    CHECK(steps[3] == StepId::AmsIdentify);
    CHECK(steps[4] == StepId::LedSelect);
    CHECK(steps[5] == StepId::FilamentSensor);
    CHECK(steps[6] == StepId::InputShaper);
}

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: the offer never presents a step the printer has nothing for",
                 "[wizard][hardware][deferred][steps]") {
    // A targeted session runs its list verbatim — ui_wizard_create_targeted()
    // does no skip filtering — so an unfiltered list would show an empty AMS,
    // LED and filament-sensor page on a printer that has none of them.
    auto v = all_visible();
    skip(v, StepId::AmsIdentify);
    skip(v, StepId::LedSelect);
    skip(v, StepId::FilamentSensor);

    auto steps = helix::wizard_deferred_hardware_steps(v);

    CHECK(steps.size() == 4);
    CHECK_FALSE(contains(steps, StepId::AmsIdentify));
    CHECK_FALSE(contains(steps, StepId::LedSelect));
    CHECK_FALSE(contains(steps, StepId::FilamentSensor));
    CHECK(contains(steps, StepId::PrinterIdentify));
    CHECK(contains(steps, StepId::InputShaper));
}

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: the offer excludes non-hardware steps entirely",
                 "[wizard][hardware][deferred][steps]") {
    auto steps = helix::wizard_deferred_hardware_steps(all_visible());

    // Re-running these would re-ask for a language, a WiFi network, a Moonraker
    // address, or the one-time telemetry opt-in — none of which were skipped.
    CHECK_FALSE(contains(steps, StepId::TouchCalibration));
    CHECK_FALSE(contains(steps, StepId::Language));
    CHECK_FALSE(contains(steps, StepId::Wifi));
    CHECK_FALSE(contains(steps, StepId::Connection));
    CHECK_FALSE(contains(steps, StepId::Summary));
    CHECK_FALSE(contains(steps, StepId::Telemetry));
}

TEST_CASE_METHOD(DeferredHardwareFixture,
                 "Deferred hardware: a preset that collapses every hardware step offers nothing",
                 "[wizard][hardware][deferred][steps][preset]") {
    // ctx.preset.skip_hardware marks all of these skipped. The caller settles
    // the debt silently rather than showing a dialog that leads nowhere.
    auto v = all_visible();
    for (StepId id :
         {StepId::PrinterIdentify, StepId::HeaterSelect, StepId::FanSelect, StepId::AmsIdentify,
          StepId::LedSelect, StepId::FilamentSensor, StepId::InputShaper}) {
        skip(v, id);
    }

    CHECK(helix::wizard_deferred_hardware_steps(v).empty());
}

} // namespace
