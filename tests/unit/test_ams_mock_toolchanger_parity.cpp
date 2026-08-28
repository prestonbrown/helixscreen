// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_mock.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

/**
 * @file test_ams_mock_toolchanger_parity.cpp
 * @brief Pin AmsBackendMock's tool-changer mode to AmsBackendToolChanger's rules.
 *
 * The mock is what every `--test` run and every UI-level check actually drives, so
 * where it disagrees with the real backend a regression in the real one is
 * invisible. This file pins the disagreements that mattered.
 *
 * can_unload_from_toolhead() is the one that bit: the mock never overrode it, so it
 * inherited AmsBackend's PARALLEL rule (slot.is_present(), true for every toolhead
 * forever). That feeds the context menu's `toolhead_unload` factor, which inverts
 * into decide_can_load() — so Load came up disabled on every docked tool and the
 * only way to mount one from the AMS panel was gone. That is exactly the shape of
 * prestonbrown/helixscreen#1199, which AmsBackendToolChanger fixed by keying on the
 * carriage slot instead.
 */

TEST_CASE("Mock tool changer: only the carriage tool can unload from the toolhead",
          "[ams][toolchanger][mock]") {
    AmsBackendMock backend(6);
    backend.set_tool_changer_mode(true);

    const int carriage = backend.get_current_slot();
    REQUIRE(carriage >= 0); // the mock seats a tool at startup

    // The mounted tool is the only unmount target — "unload" here is UNSELECT_TOOL,
    // and unmounting a tool sitting in its dock is meaningless.
    CHECK(backend.can_unload_from_toolhead(carriage));

    for (int slot = 0; slot < 6; ++slot) {
        if (slot == carriage) {
            continue;
        }
        INFO("docked slot " << slot << " (carriage is " << carriage << ")");
        CHECK_FALSE(backend.can_unload_from_toolhead(slot));
    }
}

TEST_CASE("Mock tool changer: the unmount target follows the carriage",
          "[ams][toolchanger][mock]") {
    AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);

    // Not change_tool(): that needs a started backend and completes asynchronously.
    // This is the synchronous "the printer swapped tools" notification.
    backend.on_simulated_gcode_tool_changed(2);
    REQUIRE(backend.get_current_slot() == 2);

    CHECK(backend.can_unload_from_toolhead(2));
    CHECK_FALSE(backend.can_unload_from_toolhead(0));
    CHECK_FALSE(backend.can_unload_from_toolhead(1));
    CHECK_FALSE(backend.can_unload_from_toolhead(3));
}

TEST_CASE("Mock tool changer: out-of-range slots are not unmount targets",
          "[ams][toolchanger][mock]") {
    AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);

    CHECK_FALSE(backend.can_unload_from_toolhead(-1));
    CHECK_FALSE(backend.can_unload_from_toolhead(4));
    CHECK_FALSE(backend.can_unload_from_toolhead(99));
}

TEST_CASE("Mock outside tool-changer mode keeps the inherited rule", "[ams][toolchanger][mock]") {
    // The override must be scoped to tool-changer mode. A lane-based mock is not a
    // changer: its slots are LINEAR/HUB and the base LOADED-status rule is correct
    // there, so narrowing it to the carriage slot would break every other backend
    // the mock stands in for.
    AmsBackendMock backend(4);
    REQUIRE_FALSE(backend.is_tool_changer_mode());

    bool any_unloadable = false;
    for (int slot = 0; slot < 4; ++slot) {
        any_unloadable = any_unloadable || backend.can_unload_from_toolhead(slot);
    }
    CHECK(any_unloadable);
}
