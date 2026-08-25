// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_backend_bypass_slot_selection.cpp
 * @brief AmsBackend::requires_slot_selection_for_load() under bypass
 *
 * Run with: ./build/bin/helix-tests "[filament][bypass][ams]"
 *
 * AmsBackendMock does not override requires_slot_selection_for_load(), so these
 * cases land on the shipped base implementation in include/ams_backend.h. That
 * predicate is what routes a Load either to the lane picker or straight to the
 * external-spool macro tier, so the bypass toggle flipping it is the whole
 * contract.
 *
 * The routing decision that consumes it — plan_load()/plan_unload() across
 * backend, macro and raw-gcode tiers — is covered against the real planners in
 * test_filament_op_dispatch.cpp.
 */

#include "ams_backend_mock.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("AMS backend requires_slot_selection_for_load", "[filament][bypass][ams]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("default — requires slot selection") {
        REQUIRE(backend.requires_slot_selection_for_load());
    }

    SECTION("bypass active — does not require slot selection") {
        auto result = backend.enable_bypass();
        REQUIRE(result);

        REQUIRE_FALSE(backend.requires_slot_selection_for_load());
    }

    SECTION("bypass toggled off — requires slot selection again") {
        backend.enable_bypass();
        backend.disable_bypass();

        REQUIRE(backend.requires_slot_selection_for_load());
    }

    backend.stop();
}
