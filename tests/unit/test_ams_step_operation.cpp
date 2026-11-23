// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_step_operation.cpp
 * @brief Unit tests for AMS step progress operation type detection
 *
 * Tests the pure detection logic in detect_step_operation() which determines
 * whether to show LOAD_FRESH, LOAD_SWAP, or UNLOAD step progress based on
 * action transitions and backend state.
 *
 * Key scenarios:
 * - External swap starting with HEATING (nozzle cold)
 * - External swap starting with CUTTING (nozzle already hot)
 * - External swap starting with UNLOADING (no cutter, nozzle hot)
 * - Fresh load (no filament loaded)
 * - Explicit unload
 * - Mid-operation upgrade from UNLOAD to LOAD_SWAP
 * - UI-initiated operations (not external) should not trigger detection
 */

#include "ams_step_operation.h"

#include "../catch_amalgamated.hpp"

using helix::ams_action_is_filament_operation;
using helix::AmsAction;
using helix::detect_step_operation;
using helix::StepOperationType;

// External Swap Detection (filament loaded, various start actions)

TEST_CASE("Step operation: external swap starting with HEATING", "[ams-step][step-detect]") {
    // Classic case: nozzle is cold, backend starts with HEATING
    auto result = detect_step_operation(
        AmsAction::HEATING, AmsAction::IDLE, StepOperationType::LOAD_FRESH, true, true,
        ams_action_is_filament_operation(AmsAction::HEATING), false);
    REQUIRE(result.should_recreate);
    REQUIRE(result.op_type == StepOperationType::LOAD_SWAP);
    REQUIRE(result.jump_to_step == -1);
}

TEST_CASE("Step operation: external swap starting with CUTTING", "[ams-step][step-detect]") {
    // Nozzle already hot, backend skips heating and goes straight to cutting
    auto result = detect_step_operation(
        AmsAction::CUTTING, AmsAction::IDLE, StepOperationType::LOAD_FRESH, true, true,
        ams_action_is_filament_operation(AmsAction::CUTTING), false);
    REQUIRE(result.should_recreate);
    REQUIRE(result.op_type == StepOperationType::LOAD_SWAP);
}

TEST_CASE("Step operation: external swap starting with FORMING_TIP", "[ams-step][step-detect]") {
    // Nozzle hot, no cutter — tip-forming is the first action
    auto result = detect_step_operation(
        AmsAction::FORMING_TIP, AmsAction::IDLE, StepOperationType::LOAD_FRESH, true, true,
        ams_action_is_filament_operation(AmsAction::FORMING_TIP), false);
    REQUIRE(result.should_recreate);
    REQUIRE(result.op_type == StepOperationType::LOAD_SWAP);
}

TEST_CASE("Step operation: external swap starting with UNLOADING", "[ams-step][step-detect]") {
    // Nozzle hot, no cutter, no tip-forming — goes straight to unloading
    auto result = detect_step_operation(
        AmsAction::UNLOADING, AmsAction::IDLE, StepOperationType::LOAD_FRESH, true, true,
        ams_action_is_filament_operation(AmsAction::UNLOADING), false);
    REQUIRE(result.should_recreate);
    REQUIRE(result.op_type == StepOperationType::LOAD_SWAP);
}

// External Fresh Load (no filament loaded)

TEST_CASE("Step operation: external fresh load starting with HEATING", "[ams-step][step-detect]") {
    auto result = detect_step_operation(
        AmsAction::HEATING, AmsAction::IDLE, StepOperationType::LOAD_FRESH, true, false,
        ams_action_is_filament_operation(AmsAction::HEATING), false);
    REQUIRE(result.should_recreate);
    REQUIRE(result.op_type == StepOperationType::LOAD_FRESH);
}

TEST_CASE("Step operation: external fresh load starting with LOADING", "[ams-step][step-detect]") {
    // Nozzle already hot, goes straight to loading
    auto result = detect_step_operation(
        AmsAction::LOADING, AmsAction::IDLE, StepOperationType::LOAD_FRESH, true, false,
        ams_action_is_filament_operation(AmsAction::LOADING), false);
    REQUIRE(result.should_recreate);
    REQUIRE(result.op_type == StepOperationType::LOAD_FRESH);
}

TEST_CASE("Step operation: LOADING action always means LOAD_FRESH even if filament loaded",
          "[ams-step][step-detect]") {
    // If the first action is LOADING, it's always a fresh load — the backend has
    // already handled any unloading before reporting LOADING.
    auto result = detect_step_operation(
        AmsAction::LOADING, AmsAction::IDLE, StepOperationType::LOAD_FRESH, true, true,
        ams_action_is_filament_operation(AmsAction::LOADING), false);
    REQUIRE(result.should_recreate);
    REQUIRE(result.op_type == StepOperationType::LOAD_FRESH);
}

// External Unload

TEST_CASE("Step operation: explicit unload (UNLOADING after non-cutting prev)",
          "[ams-step][step-detect]") {
    // UNLOADING arrives, prev was HEATING (not CUTTING/FORMING_TIP), not in LOAD_SWAP
    auto result = detect_step_operation(
        AmsAction::UNLOADING, AmsAction::HEATING, StepOperationType::LOAD_FRESH, true, false,
        ams_action_is_filament_operation(AmsAction::UNLOADING), false);
    REQUIRE(result.should_recreate);
    REQUIRE(result.op_type == StepOperationType::UNLOAD);
}

TEST_CASE("Step operation: UNLOADING after CUTTING does not recreate as UNLOAD",
          "[ams-step][step-detect]") {
    // UNLOADING follows CUTTING — this is part of a swap, don't override
    auto result = detect_step_operation(
        AmsAction::UNLOADING, AmsAction::CUTTING, StepOperationType::LOAD_SWAP, true, false,
        ams_action_is_filament_operation(AmsAction::UNLOADING), false);
    REQUIRE_FALSE(result.should_recreate);
}

TEST_CASE("Step operation: UNLOADING after FORMING_TIP does not recreate as UNLOAD",
          "[ams-step][step-detect]") {
    // UNLOADING follows FORMING_TIP — this is part of a swap, don't override
    auto result = detect_step_operation(
        AmsAction::UNLOADING, AmsAction::FORMING_TIP, StepOperationType::LOAD_SWAP, true, false,
        ams_action_is_filament_operation(AmsAction::UNLOADING), false);
    REQUIRE_FALSE(result.should_recreate);
}

TEST_CASE("Step operation: UNLOADING does not override LOAD_SWAP", "[ams-step][step-detect]") {
    // Already in LOAD_SWAP mode, UNLOADING comes from a non-cutting prev
    // (e.g., after HEATING) — should not downgrade to UNLOAD
    auto result = detect_step_operation(
        AmsAction::UNLOADING, AmsAction::HEATING, StepOperationType::LOAD_SWAP, true, false,
        ams_action_is_filament_operation(AmsAction::UNLOADING), false);
    REQUIRE_FALSE(result.should_recreate);
}

// Mid-Operation Upgrade: UNLOAD → LOAD_SWAP

TEST_CASE("Step operation: upgrade UNLOAD to LOAD_SWAP when LOADING arrives",
          "[ams-step][step-detect]") {
    // Was showing UNLOAD, but loading started — this is actually a swap
    auto result = detect_step_operation(
        AmsAction::LOADING, AmsAction::UNLOADING, StepOperationType::UNLOAD, true, false,
        ams_action_is_filament_operation(AmsAction::LOADING), false);
    REQUIRE(result.should_recreate);
    REQUIRE(result.op_type == StepOperationType::LOAD_SWAP);
    REQUIRE(result.jump_to_step == 2); // Skip heat + cut/tip steps
}

TEST_CASE("Step operation: LOADING during LOAD_SWAP does not recreate", "[ams-step][step-detect]") {
    // Already in LOAD_SWAP, LOADING is expected — no recreate needed
    auto result = detect_step_operation(
        AmsAction::LOADING, AmsAction::UNLOADING, StepOperationType::LOAD_SWAP, true, false,
        ams_action_is_filament_operation(AmsAction::LOADING), false);
    REQUIRE_FALSE(result.should_recreate);
}

// UI-Initiated Operations (not external)

TEST_CASE("Step operation: UI-initiated operations are never detected", "[ams-step][step-detect]") {
    // is_external = false — operation was started by our UI via start_operation()
    // Detection should NOT trigger; the UI already set the correct operation type.
    auto result = detect_step_operation(
        AmsAction::HEATING, AmsAction::IDLE, StepOperationType::LOAD_FRESH, false, true,
        ams_action_is_filament_operation(AmsAction::HEATING), false);
    REQUIRE_FALSE(result.should_recreate);
}

TEST_CASE("Step operation: UI-initiated UNLOAD not overridden", "[ams-step][step-detect]") {
    auto result = detect_step_operation(
        AmsAction::UNLOADING, AmsAction::HEATING, StepOperationType::UNLOAD, false, false,
        ams_action_is_filament_operation(AmsAction::UNLOADING), false);
    REQUIRE_FALSE(result.should_recreate);
}

TEST_CASE("Step operation: UI-initiated LOAD_SWAP not upgraded", "[ams-step][step-detect]") {
    auto result = detect_step_operation(
        AmsAction::LOADING, AmsAction::UNLOADING, StepOperationType::UNLOAD, false, false,
        ams_action_is_filament_operation(AmsAction::LOADING), false);
    REQUIRE_FALSE(result.should_recreate);
}

// Edge Cases

TEST_CASE("Step operation: IDLE action never triggers detection", "[ams-step][step-detect]") {
    auto result =
        detect_step_operation(AmsAction::IDLE, AmsAction::IDLE, StepOperationType::LOAD_FRESH, true,
                              true, ams_action_is_filament_operation(AmsAction::IDLE), false);
    REQUIRE_FALSE(result.should_recreate);
}

TEST_CASE("Step operation: ERROR action never triggers detection", "[ams-step][step-detect]") {
    auto result = detect_step_operation(AmsAction::ERROR, AmsAction::LOADING,
                                        StepOperationType::LOAD_FRESH, true, true,
                                        ams_action_is_filament_operation(AmsAction::ERROR), false);
    REQUIRE_FALSE(result.should_recreate);
}

TEST_CASE("Step operation: non-IDLE to active does not trigger initial detection",
          "[ams-step][step-detect]") {
    // prev != IDLE — this is a mid-operation transition, not a new operation start
    // (unless it matches the unload or upgrade paths)
    auto result = detect_step_operation(
        AmsAction::HEATING, AmsAction::LOADING, StepOperationType::LOAD_FRESH, true, true,
        ams_action_is_filament_operation(AmsAction::HEATING), false);
    REQUIRE_FALSE(result.should_recreate);
}

// Backends whose unloads stand alone (one toolhead per lane, no swap)

TEST_CASE("Step operation: stand-alone unload starting with UNLOADING stays an unload",
          "[ams-step][step-detect]") {
    // Filament loaded, firmware starts its unload: there is no load to follow.
    auto result = detect_step_operation(
        AmsAction::UNLOADING, AmsAction::IDLE, StepOperationType::LOAD_FRESH, true, true,
        ams_action_is_filament_operation(AmsAction::UNLOADING), true);
    REQUIRE(result.should_recreate);
    CHECK(result.op_type == StepOperationType::UNLOAD);
}

TEST_CASE("Step operation: stand-alone unload with nothing reported loaded is an unload",
          "[ams-step][step-detect]") {
    auto result = detect_step_operation(
        AmsAction::UNLOADING, AmsAction::IDLE, StepOperationType::LOAD_FRESH, true, false,
        ams_action_is_filament_operation(AmsAction::UNLOADING), true);
    REQUIRE(result.should_recreate);
    CHECK(result.op_type == StepOperationType::UNLOAD);
}

TEST_CASE("Step operation: stand-alone backend starting with HEATING is not a swap",
          "[ams-step][step-detect]") {
    // The first frame seen is the unload's heat step; UNLOADING arrives next and
    // the explicit-unload rule turns it into UNLOAD.
    auto first = detect_step_operation(AmsAction::HEATING, AmsAction::IDLE,
                                       StepOperationType::LOAD_FRESH, true, true,
                                       ams_action_is_filament_operation(AmsAction::HEATING), true);
    REQUIRE(first.should_recreate);
    CHECK(first.op_type == StepOperationType::LOAD_FRESH);

    auto next =
        detect_step_operation(AmsAction::UNLOADING, AmsAction::HEATING, first.op_type, true, true,
                              ams_action_is_filament_operation(AmsAction::UNLOADING), true);
    REQUIRE(next.should_recreate);
    CHECK(next.op_type == StepOperationType::UNLOAD);
}

TEST_CASE("Step operation: stand-alone LOADING after an unload is a fresh load",
          "[ams-step][step-detect]") {
    auto result = detect_step_operation(AmsAction::LOADING, AmsAction::UNLOADING,
                                        StepOperationType::UNLOAD, true, false,
                                        ams_action_is_filament_operation(AmsAction::LOADING), true);
    REQUIRE(result.should_recreate);
    CHECK(result.op_type == StepOperationType::LOAD_FRESH);
    CHECK(result.jump_to_step == -1);
}

TEST_CASE("Step operation: stand-alone LOADING from idle is a fresh load",
          "[ams-step][step-detect]") {
    auto result =
        detect_step_operation(AmsAction::LOADING, AmsAction::IDLE, StepOperationType::UNLOAD, true,
                              true, ams_action_is_filament_operation(AmsAction::LOADING), true);
    REQUIRE(result.should_recreate);
    CHECK(result.op_type == StepOperationType::LOAD_FRESH);
}

TEST_CASE("Step operation: UNLOADING after HEATING inside an unload keeps its bar",
          "[ams-step][step-detect]") {
    // An unload that heats mid-way (Home, Select, Heat, Retract) returns to
    // UNLOADING; it is the same operation, not a new one.
    for (bool stand_alone : {false, true}) {
        INFO("unloads_stand_alone=" << stand_alone);
        auto result = detect_step_operation(
            AmsAction::UNLOADING, AmsAction::HEATING, StepOperationType::UNLOAD, true, true,
            ams_action_is_filament_operation(AmsAction::UNLOADING), stand_alone);
        CHECK_FALSE(result.should_recreate);
    }
}
