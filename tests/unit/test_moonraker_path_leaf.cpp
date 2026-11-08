// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_path_leaf.cpp
 * @brief Lock in leaf-normalization of Moonraker dir/file names.
 *
 * Background: FlashForge's Moonraker fork (AD5X / Adventurer 5X) returns
 * server.files.get_directory `dirname`/`filename` values as paths *relative to
 * the gcodes root* (e.g. "Feinkost/Gridfinity") where stock Moonraker returns a
 * bare leaf ("Gridfinity"). PrintSelectPathNavigator::navigate_to() then
 * concatenated the full path onto the already-current path, producing a doubled
 * segment ("Feinkost/Feinkost/Gridfinity") that the server reports as missing,
 * wedging the Print Files panel.
 *
 * moonraker_path_leaf() normalizes any such value down to its final component so
 * navigation rebuilds the correct path. It must be a no-op for stock Moonraker
 * (leaf in -> leaf out) so every other printer is unaffected.
 */

#include "moonraker_file_api.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("[moonraker_path_leaf] stock Moonraker leaf is unchanged", "[files][path][regression]") {
    REQUIRE(moonraker_path_leaf("Gridfinity") == "Gridfinity");
    REQUIRE(moonraker_path_leaf("benchy.gcode") == "benchy.gcode");
}

TEST_CASE("[moonraker_path_leaf] FlashForge root-relative path is stripped to leaf",
          "[files][path][regression]") {
    // The exact value from the field report.
    REQUIRE(moonraker_path_leaf("Feinkost/Gridfinity") == "Gridfinity");
    REQUIRE(moonraker_path_leaf("a/b/c") == "c");
}

TEST_CASE("[moonraker_path_leaf] trailing slash is tolerated", "[files][path][regression]") {
    REQUIRE(moonraker_path_leaf("Feinkost/Gridfinity/") == "Gridfinity");
    REQUIRE(moonraker_path_leaf("Gridfinity/") == "Gridfinity");
}

TEST_CASE("[moonraker_path_leaf] empty stays empty", "[files][path][regression]") {
    REQUIRE(moonraker_path_leaf("").empty());
}
