// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_z_movement_style.cpp
 * @brief Unit tests for Z movement style override setting
 *
 * Tests the ZMovementStyle enum, SettingsManager get/set, and
 * PrinterState::apply_effective_bed_moves() override logic.
 * Pure logic tests - no LVGL or Moonraker required.
 */

#include "../../include/settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// ZMovementStyle Enum Tests
// ============================================================================

TEST_CASE("ZMovementStyle enum values", "[z_movement_style]") {
    REQUIRE(static_cast<int>(ZMovementStyle::AUTO) == 0);
    REQUIRE(static_cast<int>(ZMovementStyle::BED_MOVES) == 1);
    REQUIRE(static_cast<int>(ZMovementStyle::NOZZLE_MOVES) == 2);
}
