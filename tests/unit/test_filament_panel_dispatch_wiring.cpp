// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_panel_dispatch_wiring.cpp
 * @brief What FilamentPanel does with a FilamentOpPlan — the panel side of the
 *        three-tier router.
 *
 * Run with: ./build/bin/helix-tests "[filament][dispatch][wiring]"
 *
 * test_filament_op_dispatch.cpp pins the decision; this file pins the wiring on
 * top of it: which backend entry point each AmsCall reaches, which toast each
 * refusal raises, and — the reason the refusal enum exists — that the
 * already-mounted refusal arms neither the operation guard nor the on-button
 * spinner. Dispatching that no-op (SELECT_TOOL on the carriage tool) is what
 * left the Load button spinning for the full guard timeout in bundle 9KRXZ62P.
 *
 * The branching itself is shipped code: helix::ui::panel_load_outcome() and
 * panel_unload_outcome() (ui_panel_filament.h) ARE execute_load()/execute_unload()
 * from the plan onward, and the panel reads the same struct for its toast copy,
 * its slot-picker redirect, its backend argument and its manual-pull arm. This
 * file used to carry a hand-written copy of that branching instead, and it had
 * drifted: the BypassLoaded refusal shipped in plan_load() without ever reaching
 * the copy, which fell through to SelectSlot and asserted the wrong toast plus an
 * AMS-panel redirect the panel never performs.
 *
 * What the outcome deliberately does NOT carry is the dispatch: the backend call,
 * the param modal, the raw-gcode fallback and the guard/spinner bookkeeping stay
 * in the panel. tests/unit/test_filament_panel_op_timeout.cpp drives a real
 * FilamentPanel built from filament_panel.xml over a recording backend, via
 * tests/test_helpers/filament_panel_test_access.h, and is where that half belongs.
 */

#include "ui_panel_filament.h"

#include "ams_types.h"
#include "filament_op_dispatch.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::AmsCall;
using helix::ui::BackendCaps;
using helix::ui::EXTERNAL_SPOOL_SLOT;
using helix::ui::FilamentPanelOutcome;
using helix::ui::FilamentTier;
using helix::ui::panel_load_outcome;
using helix::ui::panel_unload_outcome;
using helix::ui::plan_load;
using helix::ui::plan_unload;

namespace {

/// plan_load() + the panel's answer to it — the two production calls
/// execute_load() itself makes back to back, before it dispatches.
FilamentPanelOutcome load_outcome(const AmsSystemInfo& sys, const BackendCaps& caps,
                                  int target_slot, bool macro_available,
                                  bool macro_user_configured = false) {
    return panel_load_outcome(
        plan_load(sys, caps, target_slot, macro_available, macro_user_configured));
}

/// plan_unload() + the panel's answer to it. `caps.present` is the same
/// backend-exists answer execute_unload() hands the manual-pull question.
FilamentPanelOutcome unload_outcome(const BackendCaps& caps, int target_slot, bool target_is_loaded,
                                    bool macro_available, bool macro_user_configured = false) {
    return panel_unload_outcome(
        plan_unload(caps, target_slot, target_is_loaded, macro_available, macro_user_configured),
        caps.present, target_slot);
}

AmsSystemInfo make_sys(int slot_count, int current_slot, std::vector<int> mapped_tools = {}) {
    AmsSystemInfo sys;
    AmsUnit unit;
    unit.slot_count = slot_count;
    for (int i = 0; i < slot_count; ++i) {
        SlotInfo slot;
        slot.mapped_tool =
            (i < static_cast<int>(mapped_tools.size())) ? mapped_tools[static_cast<size_t>(i)] : i;
        unit.slots.push_back(slot);
    }
    sys.units.push_back(std::move(unit));
    sys.total_slots = slot_count;
    sys.current_slot = current_slot;
    return sys;
}

BackendCaps fresh_ams() {
    return {/*present=*/true, /*requires_slot_selection_for_load=*/true,
            /*needs_unload_before_load=*/false, /*is_tool_changer=*/false};
}

/// A backend with bypass engaged. is_bypass_active() and
/// !requires_slot_selection_for_load() always travel together — the latter
/// defaults to the negation of the former — so a test that flips only the
/// second models a state no backend ever reports, and misses plan_load()'s
/// entire bypass branch.
BackendCaps bypassed_ams() {
    BackendCaps caps = fresh_ams();
    caps.requires_slot_selection_for_load = false;
    caps.bypass_active = true;
    return caps;
}

BackendCaps seated_toolchanger() {
    BackendCaps caps = fresh_ams();
    caps.is_tool_changer = true;
    caps.needs_unload_before_load = true;
    return caps;
}

} // namespace

// =============================================================================
// Load — refusals
// =============================================================================

TEST_CASE("Load on the already-mounted tool arms no guard and no spinner",
          "[filament][dispatch][wiring][1183]") {
    // The whole point of the refusal: the panel used to dispatch here, and the
    // firmware no-op left operation_guard_ running for its full timeout with the
    // Load button spinning and every other op locked out by is_busy().
    AmsSystemInfo sys = make_sys(5, /*current_slot=*/4);

    FilamentPanelOutcome out = load_outcome(sys, seated_toolchanger(), /*target_slot=*/4,
                                            /*macro_available=*/true);
    CHECK(out.tier == FilamentTier::Refused);
    CHECK_FALSE(out.guard_armed);
    CHECK(out.call == AmsCall::None);
}

TEST_CASE("Load on the already-mounted tool still tells the user something",
          "[filament][dispatch][wiring][1183]") {
    // AmsOperationSidebar refuses silently, which is tolerable on a slot grid
    // where the active slot is already highlighted. On the Filament panel the
    // user pressed a button and gets no other feedback, so a toast is required.
    AmsSystemInfo sys = make_sys(5, /*current_slot=*/4);

    FilamentPanelOutcome out = load_outcome(sys, seated_toolchanger(), /*target_slot=*/4,
                                            /*macro_available=*/true);
    CHECK(out.toast == "That tool is already loaded");
    // Not the slot-picker redirect: there is nothing to pick, the tool is on.
    CHECK_FALSE(out.navigate_to_ams);
}

TEST_CASE("Load with no resolved slot redirects to the AMS slot picker",
          "[filament][dispatch][wiring]") {
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/-1);

    FilamentPanelOutcome out = load_outcome(sys, fresh_ams(), /*target_slot=*/-1,
                                            /*macro_available=*/true);
    CHECK(out.tier == FilamentTier::Refused);
    CHECK(out.toast == "Select a filament slot to load");
    CHECK(out.navigate_to_ams);
    CHECK_FALSE(out.guard_armed);
}

TEST_CASE("The two load refusals do not share a toast or a navigation",
          "[filament][dispatch][wiring][1183]") {
    // Guard rail: collapsing AlreadyMounted into the SelectSlot arm would send a
    // toolchanger user to the AMS panel to re-pick the tool they already have.
    AmsSystemInfo mounted = make_sys(5, /*current_slot=*/2);
    AmsSystemInfo empty = make_sys(5, /*current_slot=*/-1);

    FilamentPanelOutcome a = load_outcome(mounted, seated_toolchanger(), 2, true);
    FilamentPanelOutcome b = load_outcome(empty, fresh_ams(), -1, true);
    CHECK(a.toast != b.toast);
    CHECK(a.navigate_to_ams != b.navigate_to_ams);
}

// =============================================================================
// Load — tier 1 entry points
// =============================================================================

TEST_CASE("Load with nothing seated calls load_filament on the selected slot",
          "[filament][dispatch][wiring]") {
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/-1);

    FilamentPanelOutcome out = load_outcome(sys, fresh_ams(), /*target_slot=*/2,
                                            /*macro_available=*/true);
    CHECK(out.tier == FilamentTier::AmsBackend);
    CHECK(out.call == AmsCall::Load);
    CHECK(out.arg == 2);
    CHECK(out.guard_armed);
}

TEST_CASE("Load with filament seated calls change_tool on the mapped tool",
          "[filament][dispatch][wiring][swap]") {
    // The load-vs-swap rule the panel never had. The argument is a TOOL number,
    // so passing plan.ams_arg to load_filament() would feed the wrong lane.
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/0, /*mapped_tools=*/{0, 3, 2, 1});
    BackendCaps seated = fresh_ams();
    seated.needs_unload_before_load = true;

    FilamentPanelOutcome out = load_outcome(sys, seated, /*target_slot=*/1,
                                            /*macro_available=*/true);
    CHECK(out.tier == FilamentTier::AmsBackend);
    CHECK(out.call == AmsCall::ChangeTool);
    CHECK(out.arg == 3);
    CHECK(out.guard_armed);
}

TEST_CASE("Load with filament seated and no tool mapping still calls load_filament",
          "[filament][dispatch][wiring][swap]") {
    // The panel used to dispatch unload_active_filament() here and arm the
    // operation guard for a load that never came: the unload finished, the
    // observer resolved the op, and the slot the user tapped was never fed.
    // The backend now gets one command with the tapped slot.
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/0, /*mapped_tools=*/{0, -1, -1, -1});
    BackendCaps seated = fresh_ams();
    seated.needs_unload_before_load = true;

    FilamentPanelOutcome out = load_outcome(sys, seated, /*target_slot=*/1,
                                            /*macro_available=*/true);
    CHECK(out.tier == FilamentTier::AmsBackend);
    CHECK(out.call == AmsCall::Load);
    CHECK(out.arg == 1);
    CHECK(out.guard_armed);
}

// =============================================================================
// Load — tiers 2 and 3
// =============================================================================

TEST_CASE("Load under bypass reaches the configured macro, not the backend",
          "[filament][dispatch][wiring][bypass]") {
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/1);

    // The bypass TARGET, not a lane: plan_load()'s bypass branch only claims
    // target_slot >= 0, so EXTERNAL_SPOOL_SLOT still falls to the macro tier.
    FilamentPanelOutcome out = load_outcome(sys, bypassed_ams(), EXTERNAL_SPOOL_SLOT,
                                            /*macro_available=*/true);
    CHECK(out.tier == FilamentTier::Macro);
    // The backend guard belongs to the backend arm; the macro arm's guard is
    // armed inside run_filament_macro, after any param modal is answered.
    CHECK_FALSE(out.guard_armed);
}

TEST_CASE("Under bypass a named lane goes to the backend, not the bypass macro",
          "[filament][dispatch][wiring][bypass]") {
    // Falling through sent the EXTERNAL holder's macro with no argument: on a K2
    // Plus that is LOAD_MATERIAL, whose heat step is gated on the toolhead
    // sensor, so with nothing threaded it moved the head and reported success
    // having loaded nothing. A lane tap is an explicit target — honour it.
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/-1);
    sys.filament_loaded = false;

    FilamentPanelOutcome out = load_outcome(sys, bypassed_ams(), /*target_slot=*/2,
                                            /*macro_available=*/true);
    CHECK(out.tier == FilamentTier::AmsBackend);
    CHECK(out.call == AmsCall::Load);
    CHECK(out.arg == 2);
    CHECK(out.guard_armed);
}

TEST_CASE("A lane load is refused while the bypass spool still crosses the toolhead",
          "[filament][dispatch][wiring][bypass]") {
    // The refusal this file's mirror was missing: it had no BypassLoaded arm, so
    // it fell to the SelectSlot default and claimed the panel raises "Select a
    // filament slot to load" and redirects to the AMS panel. It does neither —
    // the bypass switch sits above the cutter and reads the upstream piece, so
    // no gcode retracts it and there is nothing for the user to pick (36205eb27).
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/-1);
    sys.filament_loaded = true;

    FilamentPanelOutcome out = load_outcome(sys, bypassed_ams(), /*target_slot=*/2,
                                            /*macro_available=*/true);
    CHECK(out.tier == FilamentTier::Refused);
    CHECK(out.toast == "Remove the bypass spool from the toolhead first");
    CHECK_FALSE(out.navigate_to_ams);
    CHECK_FALSE(out.guard_armed);
    CHECK(out.call == AmsCall::None);
}

TEST_CASE("Load with no backend and no macro falls back to raw gcode",
          "[filament][dispatch][wiring]") {
    AmsSystemInfo sys = make_sys(0, -1);
    BackendCaps none{};

    FilamentPanelOutcome out = load_outcome(sys, none, /*target_slot=*/-1,
                                            /*macro_available=*/false);
    CHECK(out.tier == FilamentTier::RawGcode);
}

// =============================================================================
// Unload — deliberately asymmetric with load
// =============================================================================

TEST_CASE("Unload with nothing loaded warns and dispatches nothing",
          "[filament][dispatch][wiring]") {
    FilamentPanelOutcome out = unload_outcome(fresh_ams(), /*target_slot=*/2,
                                              /*target_is_loaded=*/false, /*macro_available=*/true);
    CHECK(out.tier == FilamentTier::Refused);
    CHECK(out.toast == "No filament loaded to unload");
    CHECK_FALSE(out.guard_armed);
    // The unload refusal never redirects — the panel already knows the slot.
    CHECK_FALSE(out.navigate_to_ams);
}

TEST_CASE("Unload passes the panel's selected slot to unload_filament",
          "[filament][dispatch][wiring]") {
    // Re-resolving current_slot inside the backend was the U1 wrong-tool bug;
    // the plan must carry the slot the panel asked for.
    FilamentPanelOutcome out = unload_outcome(fresh_ams(), /*target_slot=*/3,
                                              /*target_is_loaded=*/true, /*macro_available=*/true);
    CHECK(out.tier == FilamentTier::AmsBackend);
    CHECK(out.call == AmsCall::Unload);
    CHECK(out.arg == 3);
    CHECK(out.guard_armed);
}

TEST_CASE("Unload under bypass stays on the backend while load falls through",
          "[filament][dispatch][wiring][bypass]") {
    // The asymmetry, asserted from the panel's side: harmonizing the two gates
    // would run the user's unload macro twice under AFC bypass.
    const BackendCaps bypassed = bypassed_ams();
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/0);

    // Both halves target the bypass spool itself. Aiming them at a LANE would
    // not show the asymmetry: plan_load()'s bypass branch claims a named lane
    // for the backend, so load and unload would agree.
    FilamentPanelOutcome unload =
        unload_outcome(bypassed, EXTERNAL_SPOOL_SLOT,
                       /*target_is_loaded=*/true, /*macro_available=*/true);
    FilamentPanelOutcome load = load_outcome(sys, bypassed, EXTERNAL_SPOOL_SLOT,
                                             /*macro_available=*/true);
    CHECK(unload.tier == FilamentTier::AmsBackend);
    CHECK(load.tier == FilamentTier::Macro);
}

TEST_CASE("Unload with no backend falls through macro then raw gcode",
          "[filament][dispatch][wiring]") {
    BackendCaps none{};

    FilamentPanelOutcome with_macro = unload_outcome(none, /*target_slot=*/-1,
                                                     /*target_is_loaded=*/false,
                                                     /*macro_available=*/true);
    CHECK(with_macro.tier == FilamentTier::Macro);

    FilamentPanelOutcome without = unload_outcome(none, /*target_slot=*/-1,
                                                  /*target_is_loaded=*/false,
                                                  /*macro_available=*/false);
    CHECK(without.tier == FilamentTier::RawGcode);
}

// =============================================================================
// Unload — the manual-pull prompt
// =============================================================================

TEST_CASE("Unloading the bypass spool arms the manual-pull prompt",
          "[filament][dispatch][wiring][bypass]") {
    // The bypass spool has no lane to be reeled back into, so the unload ends
    // with filament above the extruder and the user holding nothing. The panel
    // arms the prompt at dispatch; the toolhead sensor (or op completion) fires
    // it. Without this the operation just "succeeds" with filament still in the
    // machine and no instruction.
    auto out = unload_outcome(fresh_ams(), EXTERNAL_SPOOL_SLOT, /*target_is_loaded=*/true,
                              /*macro_available=*/true);
    CHECK(out.tier == FilamentTier::AmsBackend);
    CHECK(out.arg == EXTERNAL_SPOOL_SLOT);
    CHECK(out.arm_manual_pull);
}

TEST_CASE("Unloading an AMS lane arms no manual-pull prompt",
          "[filament][dispatch][wiring][bypass]") {
    // The lane reels its own filament back. Prompting here would fire on every
    // ordinary unload, which is how a helpful toast turns into noise.
    auto out = unload_outcome(fresh_ams(), /*target_slot=*/2, /*target_is_loaded=*/true,
                              /*macro_available=*/true);
    REQUIRE(out.tier == FilamentTier::AmsBackend);
    CHECK_FALSE(out.arm_manual_pull);
}

TEST_CASE("A backend-less unload arms the prompt on every tier",
          "[filament][dispatch][wiring][bypass]") {
    // No AMS at all: the spool sits on a holder and feeds the toolhead directly,
    // so both the macro and the raw-gcode fallback leave the same manual job.
    BackendCaps none{};

    auto via_macro = unload_outcome(none, /*target_slot=*/-1, /*target_is_loaded=*/false,
                                    /*macro_available=*/true);
    REQUIRE(via_macro.tier == FilamentTier::Macro);
    CHECK(via_macro.arm_manual_pull);

    auto via_gcode = unload_outcome(none, /*target_slot=*/-1, /*target_is_loaded=*/false,
                                    /*macro_available=*/false);
    REQUIRE(via_gcode.tier == FilamentTier::RawGcode);
    CHECK(via_gcode.arm_manual_pull);
}

TEST_CASE("A refused unload arms nothing", "[filament][dispatch][wiring][bypass]") {
    // Nothing was dispatched, so nothing will ever complete to disarm it. An arm
    // here would sit waiting for the next unrelated toolhead-sensor edge.
    auto out = unload_outcome(fresh_ams(), EXTERNAL_SPOOL_SLOT, /*target_is_loaded=*/false,
                              /*macro_available=*/true);
    REQUIRE(out.tier == FilamentTier::Refused);
    CHECK_FALSE(out.arm_manual_pull);
}
