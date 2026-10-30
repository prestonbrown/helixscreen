// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Per-backend implicit-chaining policy for the bypass toggle
// (prestonbrown/helixscreen#1229 defect 6).
//
// Enabling bypass from the AMS sidebar used to synthesise an unload the user
// never asked for: if a slot was loaded, the sidebar fired
// unload_active_filament() and deferred enable_bypass() to the action
// observer. On AFC and Happy Hare that is wrong — those users have a console,
// expect the screen to pass their command straight through, and lose the
// filament in the toolhead to a command they never issued. OEM backends (CFS,
// ACE, AD5X IFS, Snapmaker, QIDI) have no console fallback, so they keep the
// chaining.
//
// The decision is a pure function so it is testable without LVGL; the sidebar
// calls it rather than duplicating the condition.

#include "ams_types.h"

#include "../catch_amalgamated.hpp"

namespace {

helix::AmsSystemInfo make_info(int current_slot, bool filament_loaded) {
    helix::AmsSystemInfo info;
    info.current_slot = current_slot;
    info.filament_loaded = filament_loaded;
    return info;
}

} // namespace

TEST_CASE("should_unload_before_bypass truth table", "[ams][afc][1229][bypass]") {
    // Expectations are literal, not recomputed from the implementation's
    // expression. Each row states what the UI must do for that state.
    struct Row {
        const char* name;
        int current_slot;
        bool filament_loaded;
        bool allows_chaining;
        bool expect_unload;
    };

    const Row rows[] = {
        // Chaining allowed (OEM backends): unload only when a slot really is
        // loaded into the toolhead.
        {"chaining + slot 0 loaded", 0, true, true, true},
        {"chaining + slot 3 loaded", 3, true, true, true},
        {"chaining + slot loaded flag false", 2, false, true, false},
        {"chaining + no slot but loaded flag", -1, true, true, false},
        {"chaining + nothing loaded", -1, false, true, false},
        {"chaining + bypass slot (-2) loaded", -2, true, true, false},

        // Chaining disallowed (AFC / Happy Hare): NEVER synthesise an unload,
        // regardless of load state. This is the defect.
        {"no chaining + slot 0 loaded", 0, true, false, false},
        {"no chaining + slot 3 loaded", 3, true, false, false},
        {"no chaining + slot loaded flag false", 2, false, false, false},
        {"no chaining + no slot but loaded flag", -1, true, false, false},
        {"no chaining + nothing loaded", -1, false, false, false},
    };

    for (const auto& row : rows) {
        CAPTURE(row.name);
        helix::AmsSystemInfo info = make_info(row.current_slot, row.filament_loaded);
        CHECK(helix::should_unload_before_bypass(info, row.allows_chaining) == row.expect_unload);
    }
}
