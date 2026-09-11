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

#include "json_utils.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("filelist_change_affects_gcodes: gcodes root refreshes", "[print_select][filelist]") {
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("gcodes") == true);
}

TEST_CASE("filelist_change_affects_gcodes: config root does not refresh",
          "[print_select][filelist][regression]") {
    // The two writers that flooded L53W5PKG, both under the config root.
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("config") == false);
}

TEST_CASE("filelist_change_affects_gcodes: other Moonraker roots do not refresh",
          "[print_select][filelist]") {
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("logs") == false);
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("timelapse") == false);
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("config_examples") == false);
}

TEST_CASE("filelist_change_affects_gcodes: unknown payload shape still refreshes",
          "[print_select][filelist]") {
    // A notification we could not parse a root out of must not silently stop
    // refreshing the list — going stale is worse than an extra round trip.
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("") == true);
}

TEST_CASE("filelist_change_affects_gcodes: match is exact, not a prefix",
          "[print_select][filelist]") {
    // "gcodes_backup" is a distinct root; a substring match would let it
    // through and reintroduce the storm for anyone with such a directory
    // registered.
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("gcodes_backup") == false);
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("my_gcodes") == false);
}

TEST_CASE("filelist_change_affects_gcodes: a move out of gcodes refreshes",
          "[print_select][filelist][1575]") {
    // Moonraker builds `item` from the move DESTINATION and attaches the
    // origin as `source_item`, so a gcodes -> config move reports
    // item.root == "config" with source_item.root == "gcodes". The listing
    // still names the file, so it must refresh.
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("config", "gcodes") == true);
}

TEST_CASE("filelist_change_affects_gcodes: a change confined to other roots does not refresh",
          "[print_select][filelist][1575]") {
    // Positive control for the case above: with neither end of the operation
    // in gcodes the filter still rejects, so the config-root storm stays
    // filtered out.
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("config", "config") == false);
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("logs", "timelapse") == false);
}

TEST_CASE("filelist_change_affects_gcodes: empty source root is not relevant",
          "[print_select][filelist][1575]") {
    // source_item rides along only on a move or copy; an empty one is the
    // norm for uploads, creates and deletes. Treating it as relevant would
    // admit every root again and make the filter inert.
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("config", "") == false);
}

TEST_CASE("filelist_change_affects_gcodes: source match is exact, not a prefix",
          "[print_select][filelist][1575]") {
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("config", "gcodes_backup") == false);
}

TEST_CASE("filelist_change_affects_gcodes: unparseable item shape still refreshes with source",
          "[print_select][filelist][1575]") {
    // The item side keeps its fail-safe rule even when a source root was
    // parsed: an unrecognised payload shape must not silently stop refreshing.
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("", "config") == true);
    REQUIRE(helix::json_util::filelist_change_affects_gcodes("", "gcodes") == true);
}
