// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_filelist_filter.cpp
 * @brief notify_filelist_changed must only refresh for the root the panel lists.
 *
 * PrintSelectFileProvider hardcodes the "gcodes" root, so a change anywhere else
 * cannot alter what the panel shows. In one debug bundle, an AFC printer
 * rewrites config:AFC/AFC.var.unit on every SET_* command and a SAVE_VARIABLE
 * delayed_gcode rewrites config:saved_variables.cfg, which drove 113 full
 * server.files.get_directory round trips in one session while the user sat on
 * the print-status panel.
 */

#include "json_utils.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("filelist_change_affects_gcodes refreshes only when gcodes is either end",
          "[print_select][filelist][regression][1575]") {
    // A move reports its DESTINATION as the item root and its origin as the
    // source root; uploads, creates and deletes carry no source (""). An item
    // root that could not be parsed ("") refreshes: stale is worse than a round trip.
    const struct {
        const char* item_root;
        const char* source_root;
        bool refresh;
    } cases[] = {
        {"gcodes", "", true},
        {"", "", true},       // unparseable payload
        {"", "config", true}, // unparseable item keeps its fail-safe with a source
        {"", "gcodes", true},
        {"config", "gcodes", true}, // moved out of gcodes: the listing still names it
        {"config", "", false},      // the L53W5PKG storm: AFC vars, saved_variables.cfg
        {"logs", "", false},
        {"timelapse", "", false},
        {"config_examples", "", false},
        {"gcodes_backup", "", false}, // exact root names, never a prefix or substring
        {"my_gcodes", "", false},
        {"config", "gcodes_backup", false},
        {"config", "config", false},
        {"logs", "timelapse", false},
    };
    for (const auto& c : cases) {
        INFO(c.item_root << " <- " << c.source_root);
        CHECK(helix::json_util::filelist_change_affects_gcodes(c.item_root, c.source_root) ==
              c.refresh);
    }
}
