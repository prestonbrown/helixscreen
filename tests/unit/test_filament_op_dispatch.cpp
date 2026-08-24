// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_op_dispatch.cpp
 * @brief The three-tier filament dispatch decision, shared by all three surfaces.
 *
 * FilamentPanel::execute_load() has always routed AMS backend -> configured
 * macro -> raw gcode. AmsOperationSidebar and the runout handler never learned
 * the tiering, and each grew its own partial answer:
 *
 *   - The sidebar silently returns when there is no backend, and owns the
 *     load-vs-swap rule (needs_unload_before_load + mapped_tool -> change_tool)
 *     that the panel has never had.
 *   - The sidebar also owns the already-mounted toolchanger guard, which is why
 *     the same no-op that hung the panel's Load button for 120 s (bundle
 *     9KRXZ62P) is harmless from the AMS panel.
 *   - The runout handler navigates away for a load and reaches a
 *     params-suppressed macro for unload/purge.
 *
 * These cases pin the merged rule. Two asymmetries are deliberate and are
 * asserted rather than smoothed over:
 *
 *   1. Load falls through to tier 2 when requires_slot_selection_for_load() is
 *      false — that is how a bypass spool reaches the user's LOAD_FILAMENT
 *      macro at all. Unload gates on the backend merely existing, because AFC
 *      runs the user's unload macro itself under bypass and routing it to
 *      tier 2 would run it twice.
 *   2. Load is not always load_filament(). A seated machine swaps.
 */

#include "ams_types.h"
#include "filament_op_dispatch.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::AmsCall;
using helix::ui::BackendCaps;
using helix::ui::EXTERNAL_SPOOL_SLOT;
using helix::ui::FilamentRefusal;
using helix::ui::FilamentTier;
using helix::ui::plan_load;
using helix::ui::plan_unload;
using helix::ui::unload_needs_manual_pull;
using helix::ui::unload_target_is_loaded;

namespace {

/// One unit with `slot_count` slots; slot i maps to tool i unless remapped.
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

/// A backend that wants a slot and has nothing seated — the fresh-load shape.
BackendCaps fresh_ams() {
    return {/*present=*/true, /*requires_slot_selection_for_load=*/true,
            /*needs_unload_before_load=*/false, /*is_tool_changer=*/false};
}

} // namespace

// =============================================================================
// Tier selection
// =============================================================================

// ---------------------------------------------------------------------------
// Macro override precedence (Settings > Macro Buttons)
//
// A macro the USER assigned outranks a filament system that would otherwise own
// the operation; a macro we merely auto-DETECTED does not. The two planners used
// to disagree about this — plan_load() let bypass fall through to the macro tier
// while plan_unload() gated tier 1 on the backend merely existing — so a custom
// Unload macro was silently discarded on every AMS printer.
// ---------------------------------------------------------------------------

TEST_CASE("dispatch: a user-configured macro outranks the backend on both ops",
          "[filament][dispatch][macro_override]") {
    AmsSystemInfo sys;
    BackendCaps ams = fresh_ams();

    // Load: a backend that would otherwise demand slot selection.
    auto load = plan_load(sys, ams, /*target_slot=*/2, /*macro_available=*/true,
                          /*macro_user_configured=*/true);
    CHECK(load.tier == FilamentTier::Macro);

    // Unload: the surface that used to ignore the setting entirely.
    auto unload = plan_unload(ams, /*target_slot=*/2, /*target_is_loaded=*/true,
                              /*macro_available=*/true, /*macro_user_configured=*/true);
    CHECK(unload.tier == FilamentTier::Macro);
}

TEST_CASE("dispatch: an auto-detected macro does NOT outrank the backend",
          "[filament][dispatch][macro_override]") {
    AmsSystemInfo sys;
    BackendCaps ams = fresh_ams();

    // This is the case that would quietly break CFS bypass unload: the detector
    // matches the vendor's QUIT_MATERIAL, which is incomplete for an external
    // spool (its retract is tn_retrude = -10 against a tn_extrude = 140 path,
    // because the box's feeder normally reels the rest). Only an explicit human
    // choice may take the operation away from the backend.
    auto unload = plan_unload(ams, /*target_slot=*/2, /*target_is_loaded=*/true,
                              /*macro_available=*/true, /*macro_user_configured=*/false);
    CHECK(unload.tier == FilamentTier::AmsBackend);

    auto load = plan_load(sys, ams, /*target_slot=*/2, /*macro_available=*/true,
                          /*macro_user_configured=*/false);
    CHECK(load.tier == FilamentTier::AmsBackend);
}

TEST_CASE("dispatch: the override applies with no backend too",
          "[filament][dispatch][macro_override]") {
    AmsSystemInfo sys;
    BackendCaps none;
    none.present = false;

    // No backend to outrank, but the tier must still be Macro rather than
    // RawGcode — the user named a macro and it has to run.
    auto unload = plan_unload(none, /*target_slot=*/-1, /*target_is_loaded=*/true,
                              /*macro_available=*/true, /*macro_user_configured=*/true);
    CHECK(unload.tier == FilamentTier::Macro);
}

TEST_CASE("dispatch: an override still beats a refusal the backend would have raised",
          "[filament][dispatch][macro_override]") {
    BackendCaps ams = fresh_ams();

    // Nothing loaded: plan_unload() would refuse with NothingLoaded. A user who
    // assigned their own macro gets to run it anyway — their macro may be what
    // recovers the very state we think is empty.
    auto refused = plan_unload(ams, /*target_slot=*/2, /*target_is_loaded=*/false,
                               /*macro_available=*/true, /*macro_user_configured=*/false);
    REQUIRE(refused.tier == FilamentTier::Refused);

    auto overridden = plan_unload(ams, /*target_slot=*/2, /*target_is_loaded=*/false,
                                  /*macro_available=*/true, /*macro_user_configured=*/true);
    CHECK(overridden.tier == FilamentTier::Macro);
}

TEST_CASE("plan_load: no backend falls through to the configured macro",
          "[filament][dispatch][tier]") {
    AmsSystemInfo sys = make_sys(0, -1);
    BackendCaps none{};

    auto plan = plan_load(sys, none, /*target_slot=*/-1, /*macro_available=*/true,
                          /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::Macro);
    CHECK(plan.refusal == FilamentRefusal::None);
}

TEST_CASE("plan_load: no backend and no macro falls through to raw gcode",
          "[filament][dispatch][tier]") {
    AmsSystemInfo sys = make_sys(0, -1);
    BackendCaps none{};

    auto plan = plan_load(sys, none, /*target_slot=*/-1, /*macro_available=*/false,
                          /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::RawGcode);
}

TEST_CASE("plan_load: bypass reaches the macro even with a backend present",
          "[filament][dispatch][tier][bypass]") {
    // requires_slot_selection_for_load() is `!is_bypass_active()`. Under bypass
    // the backend is present but must NOT own the load — the user's
    // LOAD_FILAMENT macro is the whole point of a bypass spool.
    AmsSystemInfo sys = make_sys(4, 1);
    BackendCaps bypassed = fresh_ams();
    bypassed.requires_slot_selection_for_load = false;

    auto plan = plan_load(sys, bypassed, /*target_slot=*/-2, /*macro_available=*/true,
                          /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::Macro);
    CHECK(plan.ams_call == AmsCall::None);
}

// =============================================================================
// Tier 1: what the backend is actually asked to do
// =============================================================================

TEST_CASE("plan_load: nothing seated dispatches a plain load", "[filament][dispatch]") {
    AmsSystemInfo sys = make_sys(4, -1);

    auto plan = plan_load(sys, fresh_ams(), /*target_slot=*/2, /*macro_available=*/true,
                          /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::AmsBackend);
    CHECK(plan.ams_call == AmsCall::Load);
    CHECK(plan.ams_arg == 2);
}

TEST_CASE("plan_load: a seated machine swaps via change_tool on the MAPPED tool",
          "[filament][dispatch][swap]") {
    // The rule AmsOperationSidebar has always applied and FilamentPanel never
    // has: with filament seated, feeding another lane is a tool change, not a
    // load. The argument is a TOOL number, so a remap must be honoured.
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/0, /*mapped_tools=*/{0, 3, 2, 1});
    BackendCaps seated = fresh_ams();
    seated.needs_unload_before_load = true;

    auto plan = plan_load(sys, seated, /*target_slot=*/1, /*macro_available=*/true,
                          /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::AmsBackend);
    CHECK(plan.ams_call == AmsCall::ChangeTool);
    CHECK(plan.ams_arg == 3); // slot 1's mapped_tool, NOT slot 1
}

TEST_CASE("plan_load: a seated machine with no tool mapping loads the slot anyway",
          "[filament][dispatch][swap]") {
    // There is no tool number to change to, so the swap arm cannot fire. This
    // used to emit unload_active_filament() and stop — filament came out, the
    // stepper showed a swap, and nothing ever loaded. One command now goes to
    // the backend and the firmware decides: ACE's change_tool() IS
    // load_filament(), QIDI's load_filament() retracts the seated slot itself,
    // and AFC's is `CHANGE_TOOL LANE={n}`. Happy Hare's `MMU_LOAD GATE={n}` will
    // refuse, which is what allows_implicit_chaining()==false asks for (#1229).
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/0, /*mapped_tools=*/{0, -1, -1, -1});
    BackendCaps seated = fresh_ams();
    seated.needs_unload_before_load = true;

    auto plan = plan_load(sys, seated, /*target_slot=*/1, /*macro_available=*/true,
                          /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::AmsBackend);
    CHECK(plan.ams_call == AmsCall::Load);
    CHECK(plan.ams_arg == 1); // the SLOT the user tapped, not a tool number
}

TEST_CASE("plan_load: an unresolvable target slot still dispatches a plain load",
          "[filament][dispatch][swap]") {
    // get_slot_global() returns nullptr for an index no unit covers. The old
    // code treated that as "unload whatever is active"; the backend's own
    // validate_slot_index() is the right place to refuse it.
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/0);
    BackendCaps seated = fresh_ams();
    seated.needs_unload_before_load = true;

    auto plan = plan_load(sys, seated, /*target_slot=*/9, /*macro_available=*/true,
                          /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::AmsBackend);
    CHECK(plan.ams_call == AmsCall::Load);
    CHECK(plan.ams_arg == 9);
}

TEST_CASE("plan_load: reloading the seated slot itself is a plain load, not a swap",
          "[filament][dispatch][swap]") {
    // current_slot == target: the swap arm must not fire, or a top-up on the
    // loaded lane would dispatch a pointless tool change.
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/2);
    BackendCaps seated = fresh_ams();
    seated.needs_unload_before_load = true;

    auto plan = plan_load(sys, seated, /*target_slot=*/2, /*macro_available=*/true,
                          /*macro_user_configured=*/false);
    CHECK(plan.ams_call == AmsCall::Load);
    CHECK(plan.ams_arg == 2);
}

// =============================================================================
// Refusals
// =============================================================================

TEST_CASE("plan_load: toolchanger refuses a load on the tool already mounted",
          "[filament][dispatch][refusal][1183]") {
    // Bundle 9KRXZ62P. SELECT_TOOL on the carriage tool is a firmware no-op;
    // dispatching it left the Load button spinning for 120 s and locked out
    // every later operation via is_busy(). The sidebar has always refused here.
    AmsSystemInfo sys = make_sys(5, /*current_slot=*/4);
    BackendCaps tc = fresh_ams();
    tc.is_tool_changer = true;
    tc.needs_unload_before_load = true;

    auto plan = plan_load(sys, tc, /*target_slot=*/4, /*macro_available=*/true,
                          /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::Refused);
    CHECK(plan.refusal == FilamentRefusal::AlreadyMounted);
    CHECK(plan.ams_call == AmsCall::None);
}

TEST_CASE("plan_load: toolchanger still swaps to a DIFFERENT tool",
          "[filament][dispatch][refusal][1183]") {
    // Guard rail on the refusal above: it must key on the mounted tool, not on
    // being a toolchanger.
    AmsSystemInfo sys = make_sys(5, /*current_slot=*/4);
    BackendCaps tc = fresh_ams();
    tc.is_tool_changer = true;
    tc.needs_unload_before_load = true;

    auto plan = plan_load(sys, tc, /*target_slot=*/1, /*macro_available=*/true,
                          /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::AmsBackend);
    CHECK(plan.ams_call == AmsCall::ChangeTool);
    CHECK(plan.ams_arg == 1);
}

TEST_CASE("plan_load: unresolved slot refuses with SelectSlot", "[filament][dispatch][refusal]") {
    AmsSystemInfo sys = make_sys(4, -1);

    auto plan = plan_load(sys, fresh_ams(), /*target_slot=*/-1, /*macro_available=*/true,
                          /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::Refused);
    CHECK(plan.refusal == FilamentRefusal::SelectSlot);
}

// =============================================================================
// Unload — deliberately asymmetric with load
// =============================================================================

TEST_CASE("plan_unload: a present backend owns the unload even under bypass",
          "[filament][dispatch][bypass]") {
    // AFC calls the user's unload macro itself when bypass is enabled, so
    // routing bypass unload to tier 2 would run that macro twice. Unload gates
    // on the backend existing, NOT on requires_slot_selection_for_load().
    BackendCaps bypassed = fresh_ams();
    bypassed.requires_slot_selection_for_load = false;

    auto plan = plan_unload(bypassed, /*target_slot=*/0, /*target_is_loaded=*/true,
                            /*macro_available=*/true, /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::AmsBackend);
    CHECK(plan.ams_call == AmsCall::Unload);
}

TEST_CASE("plan_unload: nothing loaded refuses instead of dispatching",
          "[filament][dispatch][refusal]") {
    auto plan = plan_unload(fresh_ams(), /*target_slot=*/2, /*target_is_loaded=*/false,
                            /*macro_available=*/true, /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::Refused);
    CHECK(plan.refusal == FilamentRefusal::NothingLoaded);
}

TEST_CASE("plan_unload: no backend falls through to macro then raw gcode",
          "[filament][dispatch][tier]") {
    BackendCaps none{};

    auto with_macro = plan_unload(none, /*target_slot=*/-1, /*target_is_loaded=*/false,
                                  /*macro_available=*/true, /*macro_user_configured=*/false);
    CHECK(with_macro.tier == FilamentTier::Macro);

    auto without = plan_unload(none, /*target_slot=*/-1, /*target_is_loaded=*/false,
                               /*macro_available=*/false, /*macro_user_configured=*/false);
    CHECK(without.tier == FilamentTier::RawGcode);
}

// =============================================================================
// Bypass / external spool — the -2 sentinel is a TARGET, not "no slot"
// =============================================================================

TEST_CASE("unload_target_is_loaded: bypass reads the aggregate, not per-slot sensors",
          "[filament][dispatch][bypass]") {
    // The AFC case, and the whole reason this arm exists. AFC is the one backend
    // with has_per_slot_loaded_authority(), so slot_is_actively_loaded(-2) looks
    // up get_slot_info(-2) — a bounds-miss that yields an empty SlotInfo — and
    // answers false while filament is demonstrably at the nozzle. CFS and Happy
    // Hare accidentally answer true via `slot == current_slot && loaded`, which
    // is exactly the kind of per-backend divergence this header exists to end.
    CHECK(unload_target_is_loaded(EXTERNAL_SPOOL_SLOT, /*slot_actively_loaded=*/false,
                                  /*slot_filament_at_toolhead=*/false, /*is_current_slot=*/true,
                                  /*any_filament_loaded=*/true));

    // Bypass engaged with nothing fed yet — AFC's bypass_state is independent of
    // filament presence, so "bypass is on" must not be read as "something to pull".
    CHECK_FALSE(unload_target_is_loaded(EXTERNAL_SPOOL_SLOT, false, false, /*is_current_slot=*/true,
                                        /*any_filament_loaded=*/false));
}

TEST_CASE("unload_target_is_loaded: each lane arm still stands alone",
          "[filament][dispatch][regression]") {
    // The recovery cases from #995 / #1199. Any one of the three is sufficient,
    // and the aggregate flag must NOT be able to veto them.
    CHECK(unload_target_is_loaded(2, /*actively_loaded=*/true, false, false,
                                  /*any_filament_loaded=*/false));
    CHECK(unload_target_is_loaded(2, false, /*at_toolhead=*/true, false, false));
    CHECK(unload_target_is_loaded(2, false, false, /*is_current_slot=*/true, false));
    CHECK_FALSE(unload_target_is_loaded(2, false, false, false, /*any_filament_loaded=*/true));
}

TEST_CASE("unload_target_is_loaded: -1 is still nothing, whatever the sensors say",
          "[filament][dispatch][regression]") {
    // Pins the sentinel as the ONLY negative slot that unloads. Without this,
    // relaxing plan_unload's `target_slot < 0` guard quietly makes "no slot
    // resolved" dispatch an unload against whatever the firmware last touched.
    CHECK_FALSE(unload_target_is_loaded(-1, true, true, true, true));
    CHECK_FALSE(unload_target_is_loaded(-3, true, true, true, true));
}

TEST_CASE("plan_unload: the bypass spool dispatches to the backend",
          "[filament][dispatch][bypass]") {
    // Every backend already handles -2 correctly: CFS ignores the slot and sends
    // its unload script, AFC resolves the lane name to "" and sends a bare
    // TOOL_UNLOAD, Happy Hare sends MMU_UNLOAD. Only this decision layer refused.
    auto plan = plan_unload(fresh_ams(), EXTERNAL_SPOOL_SLOT, /*target_is_loaded=*/true,
                            /*macro_available=*/true, /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::AmsBackend);
    CHECK(plan.ams_call == AmsCall::Unload);
    CHECK(plan.ams_arg == EXTERNAL_SPOOL_SLOT);
}

TEST_CASE("plan_unload: bypass with an empty toolhead still refuses",
          "[filament][dispatch][bypass][refusal]") {
    auto plan = plan_unload(fresh_ams(), EXTERNAL_SPOOL_SLOT, /*target_is_loaded=*/false,
                            /*macro_available=*/true, /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::Refused);
    CHECK(plan.refusal == FilamentRefusal::NothingLoaded);
}

TEST_CASE("plan_unload: an unresolved slot is still refused, sentinel or not",
          "[filament][dispatch][regression]") {
    // The guard rail on the change above. -1 means "nothing resolved" and must
    // never dispatch, even when the caller insists something is loaded.
    auto plan = plan_unload(fresh_ams(), -1, /*target_is_loaded=*/true, /*macro_available=*/true,
                            /*macro_user_configured=*/false);
    CHECK(plan.tier == FilamentTier::Refused);
    CHECK(plan.refusal == FilamentRefusal::NothingLoaded);
}

// =============================================================================
// Manual pull — which unloads leave filament for the user to remove by hand
// =============================================================================

TEST_CASE("unload_needs_manual_pull: only unloads with no lane to retract into",
          "[filament][dispatch][bypass]") {
    // An AMS lane unload reels the filament back into its own lane; prompting
    // the user to pull it would be noise. The bypass spool and a backend-less
    // printer both leave it dangling out of the toolhead.
    CHECK(unload_needs_manual_pull(/*backend_present=*/true, EXTERNAL_SPOOL_SLOT));
    CHECK(unload_needs_manual_pull(/*backend_present=*/false, /*target_slot=*/0));
    CHECK(unload_needs_manual_pull(/*backend_present=*/false, EXTERNAL_SPOOL_SLOT));
    CHECK_FALSE(unload_needs_manual_pull(/*backend_present=*/true, /*target_slot=*/0));
    CHECK_FALSE(unload_needs_manual_pull(/*backend_present=*/true, /*target_slot=*/3));
}
