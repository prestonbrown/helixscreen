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

#include "../test_helpers/ams_backend_probes.h"
#include "ams_backend_ace.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_types.h"

#include "../catch_amalgamated.hpp"

namespace {

AmsSystemInfo make_info(int current_slot, bool filament_loaded) {
    AmsSystemInfo info;
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
        AmsSystemInfo info = make_info(row.current_slot, row.filament_loaded);
        CHECK(helix::should_unload_before_bypass(info, row.allows_chaining) == row.expect_unload);
    }
}

TEST_CASE("should_unload_before_bypass is false for every state when chaining is disallowed",
          "[ams][afc][1229][bypass]") {
    // Exhaustive over the interesting slot values crossed with the load flag:
    // with chaining off there is no state that may produce an implicit unload.
    for (int slot : {-2, -1, 0, 1, 7}) {
        for (bool loaded : {false, true}) {
            CAPTURE(slot, loaded);
            CHECK_FALSE(helix::should_unload_before_bypass(make_info(slot, loaded), false));
        }
    }
}

TEST_CASE("AFC and Happy Hare refuse implicit chaining", "[ams][afc][1229][bypass]") {
    AfcProbe afc;
    HappyHareProbe hh;

    // Console-equipped firmwares: one user action == one command sent.
    CHECK_FALSE(afc.allows_implicit_chaining());
    CHECK_FALSE(hh.allows_implicit_chaining());

    // And the pure function must honour that even with filament loaded.
    AmsSystemInfo loaded = make_info(0, true);
    CHECK_FALSE(helix::should_unload_before_bypass(loaded, afc.allows_implicit_chaining()));
    CHECK_FALSE(helix::should_unload_before_bypass(loaded, hh.allows_implicit_chaining()));
}

TEST_CASE("OEM backends keep implicit chaining", "[ams][afc][1229][bypass]") {
    CfsProbe cfs;
    AceProbe ace;

    // Neither overrides the predicate, so this also pins the AmsBackend base
    // default that the remaining OEM backends inherit. No console fallback on
    // these — the UI keeps doing the unload for the user.
    CHECK(cfs.allows_implicit_chaining());
    CHECK(ace.allows_implicit_chaining());

    // With a slot loaded, chaining backends do prompt the unload-first path.
    AmsSystemInfo loaded = make_info(1, true);
    CHECK(helix::should_unload_before_bypass(loaded, cfs.allows_implicit_chaining()));
    CHECK(helix::should_unload_before_bypass(loaded, ace.allows_implicit_chaining()));

    // But an unloaded system still goes straight to enable_bypass().
    AmsSystemInfo empty = make_info(-1, false);
    CHECK_FALSE(helix::should_unload_before_bypass(empty, cfs.allows_implicit_chaining()));
    CHECK_FALSE(helix::should_unload_before_bypass(empty, ace.allows_implicit_chaining()));
}
