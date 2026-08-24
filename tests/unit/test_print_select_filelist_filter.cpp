// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_filelist_filter.cpp
 * @brief notify_filelist_changed must only refresh for the root the panel lists.
 *
 * PrintSelectFileProvider hardcodes the "gcodes" root, so a change anywhere else
 * cannot alter what the panel shows. Debug bundle L53W5PKG: an AFC printer
 * rewrites config:AFC/AFC.var.unit on every SET_* command and a SAVE_VARIABLE
 * delayed_gcode rewrites config:saved_variables.cfg, which drove 113 full
 * server.files.get_directory round trips in one session while the user sat on
 * the print-status panel.
 */

#include "ui_panel_print_select.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("filelist_change_affects_gcodes: gcodes root refreshes", "[print_select][filelist]") {
    REQUIRE(filelist_change_affects_gcodes("gcodes") == true);
}

TEST_CASE("filelist_change_affects_gcodes: config root does not refresh",
          "[print_select][filelist][regression]") {
    // The two writers that flooded L53W5PKG, both under the config root.
    REQUIRE(filelist_change_affects_gcodes("config") == false);
}

TEST_CASE("filelist_change_affects_gcodes: other Moonraker roots do not refresh",
          "[print_select][filelist]") {
    REQUIRE(filelist_change_affects_gcodes("logs") == false);
    REQUIRE(filelist_change_affects_gcodes("timelapse") == false);
    REQUIRE(filelist_change_affects_gcodes("config_examples") == false);
}

TEST_CASE("filelist_change_affects_gcodes: unknown payload shape still refreshes",
          "[print_select][filelist]") {
    // A notification we could not parse a root out of must not silently stop
    // refreshing the list — going stale is worse than an extra round trip.
    REQUIRE(filelist_change_affects_gcodes("") == true);
}

TEST_CASE("filelist_change_affects_gcodes: match is exact, not a prefix",
          "[print_select][filelist]") {
    // "gcodes_backup" is a distinct root; a substring match would let it
    // through and reintroduce the storm for anyone with such a directory
    // registered.
    REQUIRE(filelist_change_affects_gcodes("gcodes_backup") == false);
    REQUIRE(filelist_change_affects_gcodes("my_gcodes") == false);
}
