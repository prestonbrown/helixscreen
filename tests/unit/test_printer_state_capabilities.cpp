// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_state_capabilities.cpp
 * @brief PrinterState capability answers: the printer-type lookup, answers that
 *        arrive before the subjects exist, external z-offset persistence, and the
 *        enclosure top.
 *
 * Tests use set_printer_type_sync(), which calls the internal method directly;
 * the async set_printer_type() defers to the main thread.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/sensors_forget.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

TEST_CASE("PrinterState: printer type lookup is case-insensitive",
          "[printer_state][capabilities][edge]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Correct case should work
    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");
    REQUIRE_FALSE(state.profile_state().pre_print_option_set().empty());

    // Different case should also work (database lookup is case-insensitive)
    state.set_printer_type_sync("flashforge adventurer 5m pro");
    REQUIRE_FALSE(state.profile_state().pre_print_option_set().empty());
}

// ============================================================================
// Capability answers that arrive before the subjects exist
// ============================================================================

/**
 * Discovery runs on the WebSocket thread and its answers reach PrinterState
 * through AsyncLifetimeGuard::defer(), so on a fast printer they can land before
 * init_subjects() has run. INIT_SUBJECT_INT then resets the subject to its
 * hardcoded default and the answer is simply lost - and nothing re-runs
 * discovery in a stable session.
 *
 * That is how a connected Spoolman stayed dark for five days on a K2 Plus
 * (2026-08-24, verified on the device): the discovery log said
 * "Spoolman status: connected=true" at +7.8s, subject init ran at +9.5s, and
 * SpoolmanManager - whose observer drops the identity cache behind every slot's
 * vendor and material on the falling edge - never saw availability again.
 */
TEST_CASE("PrinterState: a capability answered before subject init survives it",
          "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);

    // Answer BEFORE the subjects exist, the way discovery can.
    state.capabilities_state().set_spoolman_available(true);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    state.init_subjects(false);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    CHECK(state.is_spoolman_available());
}

TEST_CASE("PrinterState: a pre-init 'absent' answer is honoured too",
          "[printer_state][capabilities]") {
    // The latch must carry a false as faithfully as a true - seeding only the
    // positive case would make an absent component look present.
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);

    state.capabilities_state().set_spoolman_available(false);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    state.init_subjects(false);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    CHECK_FALSE(state.is_spoolman_available());
}

TEST_CASE("PrinterState: a post-init answer still wins over the latched one",
          "[printer_state][capabilities]") {
    // The latch bridges the gap before the first init; it must not shadow a
    // later, fresher answer.
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);

    state.capabilities_state().set_spoolman_available(true);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    state.init_subjects(false);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(state.is_spoolman_available());

    state.capabilities_state().set_spoolman_available(false);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    CHECK_FALSE(state.is_spoolman_available());
}

TEST_CASE("PrinterCapabilitiesState: every setter's pre-init answer survives init",
          "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json::array({"ff_pa", "timelapse"}));
    CapabilityOverrides overrides;
    overrides.set_hardware(hardware);

    PrinterCapabilitiesState caps;
    caps.set_hardware(hardware, overrides);
    caps.set_has_individual_xyz_homing(false); // subject default is 1
    caps.set_supports_belt_compare(true);
    caps.set_has_chamber_sensor(true);
    caps.set_has_chamber_heater_diagnostics(true);
    caps.set_has_chamber_filter_fan(true);
    caps.set_has_chamber_element_temp(true);
    caps.set_has_chamber_dryer(true);

    caps.init_subjects(false);

    CHECK(lv_subject_get_int(caps.subject(Capability::HasTimelapse)) == 1);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasPaCal)) == 1);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasIndividualXyzHoming)) == 0);
    CHECK(lv_subject_get_int(caps.subject(Capability::SupportsBeltCompare)) == 1);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasChamber)) == 1);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasChamberHeaterDiagnostics)) == 1);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasChamberFilterFan)) == 1);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasChamberElementTemp)) == 1);
    CHECK(lv_subject_get_int(caps.subject(Capability::HasChamberDryer)) == 1);

    caps.deinit_subjects();
}

// ============================================================================
// External z-offset persistence override (prestonbrown/helixscreen#1401)
// ============================================================================

TEST_CASE("PrinterState: an external z-offset persistence provider forces firmware-managed save",
          "[printer_state][capabilities][1401]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // An unknown printer resolves to probe/endstop from live probe state -
    // never firmware-managed. Asserting that first is what makes the override
    // assertion mean something.
    state.set_printer_type_sync("Unknown Printer");
    REQUIRE(state.profile_state().z_offset_calibration_strategy() !=
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    REQUIRE(lv_subject_get_int(state.profile_state().get_z_offset_can_save_subject()) == 1);

    // Discovery matched a provider (Helper-Script's save-zoffset wrapper, ZMOD,
    // Forge-X): the module owns persistence, so Save Z Offset must stand down
    // or its probe fold double-applies on every restart.
    state.set_z_offset_external_persistence_internal("Helper-Script");
    REQUIRE(state.profile_state().z_offset_calibration_strategy() ==
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    REQUIRE(lv_subject_get_int(state.profile_state().get_z_offset_can_save_subject()) == 0);

    // Sticky across a type re-resolution that would recompute a
    // probe/endstop strategy.
    state.set_printer_type_sync("Another Unknown Printer");
    REQUIRE(state.profile_state().z_offset_calibration_strategy() ==
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);

    // And rediscovery without the module restores the type-derived strategy.
    state.clear_z_offset_external_persistence_internal();
    REQUIRE(state.profile_state().z_offset_calibration_strategy() !=
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    REQUIRE(lv_subject_get_int(state.profile_state().get_z_offset_can_save_subject()) == 1);
}

// ============================================================================
// Refuting an over-matched persistence provider (prestonbrown/helixscreen#1401)
// ============================================================================
//
// One provider row is keyed on the SET_GCODE_OFFSET wrapper object, which
// proves a wrapper is installed but not that it stores the offset - benign
// wrappers (logging, clamping, per-tool offsets) are a standard pattern.
// Discovery latches FIRMWARE_MANAGED on the match anyway, because losing the
// Save button is recoverable and the opposite mistake is not. update_from_status
// is where a frame that PROVES the store is absent releases the latch.

namespace {

/// A printer whose objects list carries the given gcode macros.
helix::PrinterDiscovery discovery_with_macros(std::initializer_list<const char*> macros) {
    nlohmann::json objects = nlohmann::json::array();
    objects.push_back("gcode_move");
    objects.push_back("toolhead");
    for (const char* m : macros) {
        objects.push_back(std::string("gcode_macro ") + m);
    }
    helix::PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

/// A save_variables status frame carrying exactly the given variables dict.
nlohmann::json save_variables_frame(const nlohmann::json& variables) {
    return nlohmann::json{{"save_variables", nlohmann::json{{"variables", variables}}}};
}

} // namespace

TEST_CASE("PrinterState: a status frame without the store refutes a wrapper-only match",
          "[printer_state][capabilities][1401]") {
    LVGLTestFixture fixture;

    helix::PrinterState state;
    state.init_subjects(false);
    state.set_hardware(discovery_with_macros({"SET_GCODE_OFFSET"}));

    // An unknown printer resolves to probe/endstop, so the stand-down below is
    // visible rather than the type's own answer.
    state.set_printer_type_sync("Unknown Printer");
    REQUIRE(state.profile_state().z_offset_calibration_strategy() !=
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);

    // Discovery latches on the wrapper object - conservative by design.
    state.set_z_offset_external_persistence_internal("Helper-Script");
    REQUIRE(state.profile_state().z_offset_calibration_strategy() ==
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    REQUIRE(lv_subject_get_int(state.profile_state().get_z_offset_can_save_subject()) == 0);

    // The save_variables store arrives complete, carrying someone else's
    // variables and no `zoffset` key: save-zoffset.cfg is not installed, the
    // wrapper stores nothing, and Save Z Offset must come back.
    state.update_from_status(save_variables_frame(nlohmann::json{{"lan_clients", 7}}));

    CHECK(state.profile_state().z_offset_calibration_strategy() !=
          ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    CHECK(lv_subject_get_int(state.profile_state().get_z_offset_can_save_subject()) == 1);

    // One-shot: further frames of the same shape find nothing latched and must
    // not thrash the strategy back and forth.
    state.update_from_status(save_variables_frame(nlohmann::json::object()));
    CHECK(state.profile_state().z_offset_calibration_strategy() !=
          ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    CHECK(lv_subject_get_int(state.profile_state().get_z_offset_can_save_subject()) == 1);
}

TEST_CASE("PrinterState: a real store, a seeded one, and no news all keep the stand-down",
          "[printer_state][capabilities][1401]") {
    // The damaging direction. Every frame here must leave Save Z Offset down:
    // re-enabling it on a printer that really persists folds the offset into the
    // probe and the firmware re-applies it at boot, growing the probe offset by
    // the full offset on every save cycle until the nozzle is on the bed.
    LVGLTestFixture fixture;

    helix::PrinterState state;
    state.init_subjects(false);
    state.set_hardware(discovery_with_macros({"SET_GCODE_OFFSET"}));
    state.set_printer_type_sync("Unknown Printer");
    state.set_z_offset_external_persistence_internal("Helper-Script");
    REQUIRE(state.profile_state().z_offset_calibration_strategy() ==
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);

    SECTION("the module's variable is present with a real value") {
        state.update_from_status(
            save_variables_frame(nlohmann::json{{"zoffset", nlohmann::json{{"z", -0.475}}}}));
    }
    SECTION("seeded as {'z': None} - installed, never used") {
        state.update_from_status(
            save_variables_frame(nlohmann::json{{"zoffset", nlohmann::json{{"z", nullptr}}}}));
    }
    SECTION("save_variables is not in this delta frame at all") {
        state.update_from_status(nlohmann::json{{"gcode_move", nlohmann::json{{"speed", 100.0}}}});
    }
    SECTION("save_variables arrived without its variables member") {
        state.update_from_status(nlohmann::json{{"save_variables", nlohmann::json::object()}});
    }

    CHECK(state.profile_state().z_offset_calibration_strategy() ==
          ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    CHECK(lv_subject_get_int(state.profile_state().get_z_offset_can_save_subject()) == 0);
}

TEST_CASE("PrinterState: an unambiguously detected provider is not refuted by a frame",
          "[printer_state][capabilities][1401]") {
    // ZMOD is matched on SAVE_ZMOD_DATA, which belongs to one firmware and means
    // one thing, so no status frame gets to argue with it - not even the very
    // frame shape that refutes the wrapper row. ZMOD's own store lives under a
    // different key in the same object.
    LVGLTestFixture fixture;

    helix::PrinterState state;
    state.init_subjects(false);
    state.set_hardware(discovery_with_macros({"SAVE_ZMOD_DATA"}));
    state.set_printer_type_sync("Unknown Printer");
    state.set_z_offset_external_persistence_internal("ZMOD");
    REQUIRE(state.profile_state().z_offset_calibration_strategy() ==
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);

    state.update_from_status(save_variables_frame(nlohmann::json{{"lan_clients", 7}}));

    CHECK(state.profile_state().z_offset_calibration_strategy() ==
          ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    CHECK(lv_subject_get_int(state.profile_state().get_z_offset_can_save_subject()) == 0);
}

namespace {

PrinterDiscovery discovery_with(const json& objects) {
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

const json kTopFitted = {{"purifier", {{"power_detected", true}, {"mode", 0}}}};

} // namespace

TEST_CASE("PrinterState: the enclosure top follows status frames",
          "[printer_state][enclosure_top]") {
    helix::test::SensorsForget forget;
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    auto& caps = state.capabilities_state();
    const json u1_objects = {"purifier", "fm175xx_reader", "tool"};

    SECTION("discovery, then the first frame") {
        state.set_hardware(discovery_with(u1_objects));
        REQUIRE_FALSE(caps.enclosure_top_fitted());

        state.update_from_status(kTopFitted);
        CHECK(caps.enclosure_top_fitted());

        // Delta frames that do not carry the flag keep the last report.
        state.update_from_status({{"purifier", {{"inner_fan_rpm", 1500.0}}}});
        CHECK(caps.enclosure_top_fitted());
        state.update_from_status({{"extruder", {{"temperature", 25.0}}}});
        CHECK(caps.enclosure_top_fitted());

        state.update_from_status({{"purifier", {{"power_detected", false}}}});
        CHECK_FALSE(caps.enclosure_top_fitted());

        // A second printer of the same kind starts from its own first frame.
        state.update_from_status(kTopFitted);
        REQUIRE(caps.enclosure_top_fitted());
        state.set_hardware(discovery_with(u1_objects));
        CHECK_FALSE(caps.enclosure_top_fitted());
    }

    SECTION("a frame before discovery, then the subscribe response's status") {
        // A live frame can reach PrinterState while it still holds the previous
        // printer's discovery; discovery then dispatches the subscribe
        // response's whole-object status after set_hardware.
        state.set_hardware(discovery_with({"extruder"}));
        state.update_from_status(kTopFitted);
        CHECK_FALSE(caps.enclosure_top_fitted());

        state.set_hardware(discovery_with(u1_objects));
        CHECK_FALSE(caps.enclosure_top_fitted());
        state.update_from_status(kTopFitted);
        CHECK(caps.enclosure_top_fitted());
    }

    SECTION("a purifier on another printer is not an enclosure top") {
        state.set_hardware(discovery_with({"purifier", "extruder"}));
        state.update_from_status(kTopFitted);
        CHECK_FALSE(caps.enclosure_top_fitted());
    }
}
