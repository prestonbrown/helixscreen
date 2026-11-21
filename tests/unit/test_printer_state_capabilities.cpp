// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_state_capabilities.cpp
 * @brief Tests for PrinterState printer type and pre-print option storage
 *
 * These tests verify the PrinterState methods:
 * - set_printer_type_sync(const std::string& type) - synchronous version for tests
 * - get_printer_type() const
 * - get_pre_print_option_set() const
 *
 * Note: Tests use set_printer_type_sync() which directly calls the internal
 * method. The async set_printer_type() defers to the main thread via
 * helix::async::call_method_ref() for thread safety from WebSocket callbacks.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
// ============================================================================
// PrinterState Printer Type Storage Tests
// ============================================================================

TEST_CASE("PrinterState: set_printer_type stores the type name", "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set a known printer type
    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

    // Verify the type is stored and retrievable
    REQUIRE(state.get_printer_type() == "FlashForge Adventurer 5M Pro");
}

TEST_CASE("PrinterState: set_printer_type with different printer names",
          "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("FlashForge Adventurer 5M") {
        state.set_printer_type_sync("FlashForge Adventurer 5M");
        REQUIRE(state.get_printer_type() == "FlashForge Adventurer 5M");
    }

    SECTION("Voron 2.4") {
        state.set_printer_type_sync("Voron 2.4");
        REQUIRE(state.get_printer_type() == "Voron 2.4");
    }

    SECTION("Custom/Other") {
        state.set_printer_type_sync("Custom/Other");
        REQUIRE(state.get_printer_type() == "Custom/Other");
    }

    SECTION("Empty string") {
        state.set_printer_type_sync("");
        REQUIRE(state.get_printer_type() == "");
    }
}

// ============================================================================
// PrinterState Capability Fetching Tests
// ============================================================================

TEST_CASE("PrinterState: set_printer_type fetches capabilities from database",
          "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set printer type that has capabilities in the database
    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

    // Get the capabilities
    const PrePrintOptionSet& caps = state.get_pre_print_option_set();

    // Verify macro name is populated from database
    REQUIRE(caps.macro_name == "START_PRINT");

    // Verify bed_mesh option exists with correct values
    const PrePrintOption* bed_mesh = caps.find("bed_mesh");
    REQUIRE(bed_mesh != nullptr);
    REQUIRE(bed_mesh->strategy_kind == PrePrintStrategyKind::MacroParam);
    const auto* macro = std::get_if<PrePrintStrategyMacroParam>(&bed_mesh->strategy);
    REQUIRE(macro != nullptr);
    REQUIRE(macro->param_name == "SKIP_LEVELING");
    REQUIRE(macro->skip_value == "1");
    REQUIRE(macro->enable_value == "0");
}

TEST_CASE("PrinterState: AD5M Pro does not include purge_line parameter",
          "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

    const PrePrintOptionSet& caps = state.get_pre_print_option_set();

    // AD5M Pro START_PRINT macro does not have purge_line or skew_correct options
    REQUIRE(caps.find("purge_line") == nullptr);
}

TEST_CASE("PrinterState: AD5M Pro does not include skew_correct parameter",
          "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

    const PrePrintOptionSet& caps = state.get_pre_print_option_set();

    // AD5M Pro START_PRINT macro does not have skew_correct option
    REQUIRE(caps.find("skew_correct") == nullptr);
}

// ============================================================================
// Unknown Printer Type Tests
// ============================================================================

TEST_CASE("PrinterState: unknown printer type returns empty capabilities",
          "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set an unknown printer type (not in database)
    state.set_printer_type_sync("Some Unknown Printer Model XYZ");

    // Capabilities should be empty
    const PrePrintOptionSet& caps = state.get_pre_print_option_set();
    REQUIRE(caps.empty());
    REQUIRE(caps.macro_name.empty());
    REQUIRE(caps.options.empty());
}

TEST_CASE("PrinterState: Custom/Other printer type returns empty capabilities",
          "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Custom/Other is a valid selection but has no database entry
    state.set_printer_type_sync("Custom/Other");

    const PrePrintOptionSet& caps = state.get_pre_print_option_set();
    REQUIRE(caps.empty());
}

TEST_CASE("PrinterState: empty printer type returns empty capabilities",
          "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    state.set_printer_type_sync("");

    const PrePrintOptionSet& caps = state.get_pre_print_option_set();
    REQUIRE(caps.empty());
}

// ============================================================================
// Changing Printer Type Tests
// ============================================================================

TEST_CASE("PrinterState: changing printer type updates capabilities",
          "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // First set to AD5M Pro (has capabilities)
    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

    // Verify it has the option set
    const PrePrintOptionSet& caps1 = state.get_pre_print_option_set();
    REQUIRE_FALSE(caps1.empty());
    REQUIRE(caps1.macro_name == "START_PRINT");
    REQUIRE(caps1.find("bed_mesh") != nullptr);

    // Change to unknown printer
    state.set_printer_type_sync("Some Unknown Printer");

    // Capabilities should now be empty
    const PrePrintOptionSet& caps2 = state.get_pre_print_option_set();
    REQUIRE(caps2.empty());
}

TEST_CASE("PrinterState: changing from unknown to known updates capabilities",
          "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start with unknown
    state.set_printer_type_sync("Unknown Printer");
    REQUIRE(state.get_pre_print_option_set().empty());

    // Change to known printer with capabilities
    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

    const PrePrintOptionSet& caps = state.get_pre_print_option_set();
    REQUIRE_FALSE(caps.empty());
    REQUIRE(caps.macro_name == "START_PRINT");
}

TEST_CASE("PrinterState: changing between printers with different capabilities",
          "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set to AD5M Pro
    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");
    const PrePrintOptionSet& caps1 = state.get_pre_print_option_set();
    REQUIRE(caps1.macro_name == "START_PRINT");

    // Change to regular AD5M (also has START_PRINT but same capabilities in DB)
    state.set_printer_type_sync("FlashForge Adventurer 5M");
    const PrePrintOptionSet& caps2 = state.get_pre_print_option_set();
    // AD5M should also have capabilities from database
    REQUIRE(caps2.macro_name == "START_PRINT");
}

// ============================================================================
// Default/Initial State Tests
// ============================================================================

TEST_CASE("PrinterState: initial printer type is empty", "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Before setting any type, should be empty
    REQUIRE(state.get_printer_type().empty());
}

TEST_CASE("PrinterState: initial capabilities are empty", "[printer_state][capabilities]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Before setting any type, capabilities should be empty
    const PrePrintOptionSet& caps = state.get_pre_print_option_set();
    REQUIRE(caps.empty());
}

// ============================================================================
// Edge Case Tests
// ============================================================================

TEST_CASE("PrinterState: printer type lookup is case-insensitive",
          "[printer_state][capabilities][edge]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Correct case should work
    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");
    REQUIRE_FALSE(state.get_pre_print_option_set().empty());

    // Different case should also work (database lookup is case-insensitive)
    state.set_printer_type_sync("flashforge adventurer 5m pro");
    REQUIRE_FALSE(state.get_pre_print_option_set().empty());
}

TEST_CASE("PrinterState: setting same type twice is idempotent",
          "[printer_state][capabilities][edge]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");
    const PrePrintOptionSet& caps1 = state.get_pre_print_option_set();

    // Set same type again
    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");
    const PrePrintOptionSet& caps2 = state.get_pre_print_option_set();

    // Should still have same option set
    REQUIRE(caps2.macro_name == caps1.macro_name);
    REQUIRE(caps2.options.size() == caps1.options.size());
}

TEST_CASE("PrinterState: set_printer_type deduplicates redundant calls",
          "[printer_state][capabilities][startup]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // First call sets type and capabilities
    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");
    REQUIRE(state.get_printer_type() == "FlashForge Adventurer 5M Pro");
    REQUIRE_FALSE(state.get_pre_print_option_set().empty());

    // Capture the capabilities object address — if dedup works, the internal
    // object won't be reassigned, so the address stays the same.
    const auto* caps_ptr = &state.get_pre_print_option_set();

    // Second call with same type should be a no-op (dedup early return)
    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

    // The capabilities reference should point to the exact same object
    // (not a freshly-assigned copy) because the early-return skipped assignment
    REQUIRE(&state.get_pre_print_option_set() == caps_ptr);
    REQUIRE(state.get_printer_type() == "FlashForge Adventurer 5M Pro");

    // But changing to a different type should NOT be deduped
    state.set_printer_type_sync("FlashForge Adventurer 5M");
    REQUIRE(state.get_printer_type() == "FlashForge Adventurer 5M");
}

TEST_CASE("PrinterState: set_printer_type dedup detects strategy changes",
          "[printer_state][capabilities][startup]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set to an unknown printer (no DB entry → strategy from probe state)
    state.set_printer_type_sync("Unknown Printer");
    REQUIRE(state.get_printer_type() == "Unknown Printer");

    // Setting the same unknown type again should still dedup (same strategy)
    const auto* caps_ptr = &state.get_pre_print_option_set();
    state.set_printer_type_sync("Unknown Printer");
    REQUIRE(&state.get_pre_print_option_set() == caps_ptr);

    // But switching to a known type (different strategy) must NOT dedup
    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");
    REQUIRE_FALSE(state.get_pre_print_option_set().empty());
}

TEST_CASE("PrinterState: get_printer_type returns const reference",
          "[printer_state][capabilities][edge]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

    // Get reference and verify it's stable
    const std::string& type1 = state.get_printer_type();
    const std::string& type2 = state.get_printer_type();

    // Should return the same reference (not a copy)
    REQUIRE(&type1 == &type2);
    REQUIRE(type1 == "FlashForge Adventurer 5M Pro");
}

TEST_CASE("PrinterState: get_pre_print_option_set returns const reference",
          "[printer_state][capabilities][edge]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

    // Get reference and verify it's stable
    const PrePrintOptionSet& caps1 = state.get_pre_print_option_set();
    const PrePrintOptionSet& caps2 = state.get_pre_print_option_set();

    // Should return the same reference (not a copy)
    REQUIRE(&caps1 == &caps2);
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
    state.set_spoolman_available(true);
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

    state.set_spoolman_available(false);
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

    state.set_spoolman_available(true);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    state.init_subjects(false);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(state.is_spoolman_available());

    state.set_spoolman_available(false);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    CHECK_FALSE(state.is_spoolman_available());
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
    REQUIRE(state.get_z_offset_calibration_strategy() !=
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    REQUIRE(lv_subject_get_int(state.get_z_offset_can_save_subject()) == 1);

    // Discovery matched a provider (Helper-Script's save-zoffset wrapper, ZMOD,
    // Forge-X): the module owns persistence, so Save Z Offset must stand down
    // or its probe fold double-applies on every restart.
    state.set_z_offset_external_persistence_internal("Helper-Script");
    REQUIRE(state.get_z_offset_calibration_strategy() ==
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    REQUIRE(lv_subject_get_int(state.get_z_offset_can_save_subject()) == 0);

    // Sticky across a type re-resolution that would recompute a
    // probe/endstop strategy.
    state.set_printer_type_sync("Another Unknown Printer");
    REQUIRE(state.get_z_offset_calibration_strategy() ==
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);

    // And rediscovery without the module restores the type-derived strategy.
    state.clear_z_offset_external_persistence_internal();
    REQUIRE(state.get_z_offset_calibration_strategy() !=
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    REQUIRE(lv_subject_get_int(state.get_z_offset_can_save_subject()) == 1);
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
    REQUIRE(state.get_z_offset_calibration_strategy() !=
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);

    // Discovery latches on the wrapper object - conservative by design.
    state.set_z_offset_external_persistence_internal("Helper-Script");
    REQUIRE(state.get_z_offset_calibration_strategy() ==
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    REQUIRE(lv_subject_get_int(state.get_z_offset_can_save_subject()) == 0);

    // The save_variables store arrives complete, carrying someone else's
    // variables and no `zoffset` key: save-zoffset.cfg is not installed, the
    // wrapper stores nothing, and Save Z Offset must come back.
    state.update_from_status(save_variables_frame(nlohmann::json{{"lan_clients", 7}}));

    CHECK(state.get_z_offset_calibration_strategy() !=
          ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    CHECK(lv_subject_get_int(state.get_z_offset_can_save_subject()) == 1);

    // One-shot: further frames of the same shape find nothing latched and must
    // not thrash the strategy back and forth.
    state.update_from_status(save_variables_frame(nlohmann::json::object()));
    CHECK(state.get_z_offset_calibration_strategy() !=
          ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    CHECK(lv_subject_get_int(state.get_z_offset_can_save_subject()) == 1);
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
    REQUIRE(state.get_z_offset_calibration_strategy() ==
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

    CHECK(state.get_z_offset_calibration_strategy() ==
          ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    CHECK(lv_subject_get_int(state.get_z_offset_can_save_subject()) == 0);
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
    REQUIRE(state.get_z_offset_calibration_strategy() ==
            ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);

    state.update_from_status(save_variables_frame(nlohmann::json{{"lan_clients", 7}}));

    CHECK(state.get_z_offset_calibration_strategy() ==
          ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    CHECK(lv_subject_get_int(state.get_z_offset_can_save_subject()) == 0);
}

// ============================================================================
// Z-offset tune hint: Print Tune claims auto-save only where something persists it
// ============================================================================

TEST_CASE("Z-offset tune hint claims auto-save only where something persists it",
          "[printer_state][capabilities][zoffset]") {
    using S = ZOffsetCalibrationStrategy;
    using H = ZOffsetTuneHint;

    // A strategy with its own Save step: the hint points at it, whatever else is known.
    CHECK(resolve_z_offset_tune_hint(S::PROBE_CALIBRATE, false, true, false) ==
          H::SAVE_ON_CONTROLS);
    CHECK(resolve_z_offset_tune_hint(S::ENDSTOP, false, false, true) == H::SAVE_ON_CONTROLS);

    // Firmware-managed and the firmware persists the offset.
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, false, true, false) == H::AUTO_SAVED);
    // An installed persistence provider is proof on its own.
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, true, false, false) == H::AUTO_SAVED);
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, true, false, true) == H::AUTO_SAVED);

    // No Save step and nothing persists: the adjustment is temporary.
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, false, false, false) == H::TEMPORARY);
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, false, false, true) ==
          H::TEMPORARY_TOOL_CHANGE);
}

TEST_CASE("PrinterState publishes the z-offset tune hint from the database",
          "[printer_state][capabilities][zoffset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    auto hint = [&] {
        return static_cast<ZOffsetTuneHint>(
            lv_subject_get_int(state.get_z_offset_tune_hint_subject()));
    };

    SECTION("Snapmaker U1: no save path, and a tool change re-applies the head's offset") {
        state.set_printer_type_sync("Snapmaker U1");
        CHECK(state.get_z_offset_calibration_strategy() ==
              ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
        CHECK(lv_subject_get_int(state.get_z_offset_can_save_subject()) == 0);
        CHECK(hint() == ZOffsetTuneHint::TEMPORARY_TOOL_CHANGE);
        CHECK(state.get_z_offset_tune_hint() == ZOffsetTuneHint::TEMPORARY_TOOL_CHANGE);

        // A provider latching later is a change even though the strategy is not.
        state.set_z_offset_external_persistence_internal("Helper-Script");
        CHECK(hint() == ZOffsetTuneHint::AUTO_SAVED);
        state.clear_z_offset_external_persistence_internal();
        CHECK(hint() == ZOffsetTuneHint::TEMPORARY_TOOL_CHANGE);
    }

    SECTION("the other firmware-managed printers keep the auto-saved claim") {
        for (const char* type :
             {"FlashForge Adventurer 5M (ForgeX)", "FlashForge Adventurer 5M Pro (ForgeX)",
              "FlashForge Adventurer 5X", "Artillery M1 Pro"}) {
            INFO(type);
            state.set_printer_type_sync(type);
            CHECK(state.get_z_offset_calibration_strategy() ==
                  ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
            CHECK(hint() == ZOffsetTuneHint::AUTO_SAVED);
        }
    }

    SECTION("a printer with a Save step points at it") {
        state.set_printer_type_sync("Unknown Printer");
        CHECK(lv_subject_get_int(state.get_z_offset_can_save_subject()) == 1);
        CHECK(hint() == ZOffsetTuneHint::SAVE_ON_CONTROLS);
    }

    PrinterStateTestAccess::reset(state);
}

namespace {

/// The z_offset_tune_hint value each z_offset_help_text_* label in @p path is
/// shown for: its bind_flag_if_not_eq ref_value (hidden for every other value).
std::map<std::string, int> tune_hint_label_values(const std::string& path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string xml = buf.str();

    std::map<std::string, int> values;
    const std::string name_attr = "name=\"z_offset_help_text_";
    for (size_t pos = xml.find(name_attr); pos != std::string::npos;
         pos = xml.find(name_attr, pos + 1)) {
        const size_t name_start = pos + std::string("name=\"").size();
        const std::string name = xml.substr(name_start, xml.find('"', name_start) - name_start);
        const size_t end = xml.find("</text_small>", pos);
        REQUIRE(end != std::string::npos);
        const std::string element = xml.substr(pos, end - pos);
        INFO(path << " " << name);
        CHECK(element.find("hidden=\"true\"") != std::string::npos);
        const std::string bind = "<bind_flag_if_not_eq subject=\"z_offset_tune_hint\" "
                                 "flag=\"hidden\" ref_value=\"";
        const size_t b = element.find(bind);
        REQUIRE(b != std::string::npos);
        CHECK(element.find("bind_flag_if_not_eq", b + bind.size()) == std::string::npos);
        values[name] = std::stoi(element.substr(b + bind.size()));
    }
    return values;
}

} // namespace

TEST_CASE("Print Tune shows exactly the label for the resolved z-offset tune hint",
          "[printer_state][capabilities][zoffset][xml]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.set_printer_type_sync("Snapmaker U1");
    const int u1_hint = lv_subject_get_int(state.get_z_offset_tune_hint_subject());
    PrinterStateTestAccess::reset(state);

    const std::map<std::string, int> expected = {
        {"z_offset_help_text_manual", static_cast<int>(ZOffsetTuneHint::SAVE_ON_CONTROLS)},
        {"z_offset_help_text_auto", static_cast<int>(ZOffsetTuneHint::AUTO_SAVED)},
        {"z_offset_help_text_temporary", static_cast<int>(ZOffsetTuneHint::TEMPORARY)},
        {"z_offset_help_text_tool_change",
         static_cast<int>(ZOffsetTuneHint::TEMPORARY_TOOL_CHANGE)},
    };

    for (const char* path :
         {"ui_xml/print_tune_panel.xml", "ui_xml/portrait/print_tune_panel.xml"}) {
        INFO(path);
        const auto values = tune_hint_label_values(path);
        CHECK(values == expected);

        // On the U1 the tool-change label is the only one left visible.
        std::vector<std::string> visible;
        for (const auto& [name, value] : values) {
            if (value == u1_hint) {
                visible.push_back(name);
            }
        }
        CHECK(visible == std::vector<std::string>{"z_offset_help_text_tool_change"});
    }
}
