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
 * FilamentPanel is LVGL/Moonraker-coupled and not unit-instantiable, so these
 * are mirror wrappers of execute_load()/execute_unload()'s post-plan branching —
 * the same seam test_filament_op_slot_resolver.cpp uses for the panel's slot
 * decision. They must be edited in lockstep with the panel.
 */

#include "ams_types.h"
#include "filament_op_dispatch.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::AmsCall;
using helix::ui::BackendCaps;
using helix::ui::EXTERNAL_SPOOL_SLOT;
using helix::ui::FilamentOpPlan;
using helix::ui::FilamentRefusal;
using helix::ui::FilamentTier;
using helix::ui::plan_load;
using helix::ui::plan_unload;
using helix::ui::unload_needs_manual_pull;

namespace {

/// Which arm of the panel's switch ran.
enum class Arm {
    Backend,  ///< a backend entry point was called
    Macro,    ///< handed to run_filament_macro via the StandardMacros slot
    RawGcode, ///< the built-in extrude/retract fallback
    Refused,  ///< nothing dispatched
};

struct PanelOutcome {
    Arm arm = Arm::Refused;
    AmsCall call = AmsCall::None;
    int arg = -1;
    /// begin_operation_guard() + backend_op_active_ + op_started() ran. False on
    /// every refusal — a refused op must leave the buttons live.
    bool guard_armed = false;
    bool navigated_to_ams = false;
    /// arm_manual_pull_prompt() ran — the "pull it out by hand" watch. Only for
    /// an unload with no lane to retract into, and never on a refusal.
    bool armed_manual_pull = false;
    std::string toast; ///< the msgid handed to NOTIFY_*, empty when none
};

/// Mirror of FilamentPanel::execute_load() from the plan onward.
PanelOutcome panel_execute_load(const AmsSystemInfo& sys, const BackendCaps& caps, int target_slot,
                                bool macro_available, bool macro_user_configured = false) {
    const FilamentOpPlan plan =
        plan_load(sys, caps, target_slot, macro_available, macro_user_configured);
    PanelOutcome out;

    switch (plan.tier) {
    case FilamentTier::AmsBackend:
        out.arm = Arm::Backend;
        out.guard_armed = true;
        out.call = plan.ams_call;
        out.arg = plan.ams_arg;
        break;

    case FilamentTier::Refused:
        out.arm = Arm::Refused;
        switch (plan.refusal) {
        case FilamentRefusal::AlreadyMounted:
            out.toast = "That tool is already loaded";
            break;
        case FilamentRefusal::SelectSlot:
        default:
            out.toast = "Select a filament slot to load";
            out.navigated_to_ams = true;
            break;
        }
        break;

    case FilamentTier::Macro:
        out.arm = Arm::Macro;
        break;

    case FilamentTier::RawGcode:
        out.arm = Arm::RawGcode;
        break;
    }
    return out;
}

/// Mirror of FilamentPanel::execute_unload() from the plan onward.
PanelOutcome panel_execute_unload(const BackendCaps& caps, int target_slot, bool target_is_loaded,
                                  bool macro_available, bool macro_user_configured = false) {
    const FilamentOpPlan plan =
        plan_unload(caps, target_slot, target_is_loaded, macro_available, macro_user_configured);
    PanelOutcome out;

    // Mirrors the arm site, which sits between the plan and the switch so it
    // covers every dispatching tier at once and no refusal.
    out.armed_manual_pull = plan.tier != FilamentTier::Refused &&
                            helix::ui::unload_needs_manual_pull(caps.present, target_slot);

    switch (plan.tier) {
    case FilamentTier::AmsBackend:
        out.arm = Arm::Backend;
        out.guard_armed = true;
        out.call = plan.ams_call;
        out.arg = plan.ams_arg;
        break;

    case FilamentTier::Refused:
        out.arm = Arm::Refused;
        out.toast = "No filament loaded to unload";
        break;

    case FilamentTier::Macro:
        out.arm = Arm::Macro;
        break;

    case FilamentTier::RawGcode:
        out.arm = Arm::RawGcode;
        break;
    }
    return out;
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

    PanelOutcome out = panel_execute_load(sys, seated_toolchanger(), /*target_slot=*/4,
                                          /*macro_available=*/true);
    CHECK(out.arm == Arm::Refused);
    CHECK_FALSE(out.guard_armed);
    CHECK(out.call == AmsCall::None);
}

TEST_CASE("Load on the already-mounted tool still tells the user something",
          "[filament][dispatch][wiring][1183]") {
    // AmsOperationSidebar refuses silently, which is tolerable on a slot grid
    // where the active slot is already highlighted. On the Filament panel the
    // user pressed a button and gets no other feedback, so a toast is required.
    AmsSystemInfo sys = make_sys(5, /*current_slot=*/4);

    PanelOutcome out = panel_execute_load(sys, seated_toolchanger(), /*target_slot=*/4,
                                          /*macro_available=*/true);
    CHECK(out.toast == "That tool is already loaded");
    // Not the slot-picker redirect: there is nothing to pick, the tool is on.
    CHECK_FALSE(out.navigated_to_ams);
}

TEST_CASE("Load with no resolved slot redirects to the AMS slot picker",
          "[filament][dispatch][wiring]") {
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/-1);

    PanelOutcome out = panel_execute_load(sys, fresh_ams(), /*target_slot=*/-1,
                                          /*macro_available=*/true);
    CHECK(out.arm == Arm::Refused);
    CHECK(out.toast == "Select a filament slot to load");
    CHECK(out.navigated_to_ams);
    CHECK_FALSE(out.guard_armed);
}

TEST_CASE("The two load refusals do not share a toast or a navigation",
          "[filament][dispatch][wiring][1183]") {
    // Guard rail: collapsing AlreadyMounted into the SelectSlot arm would send a
    // toolchanger user to the AMS panel to re-pick the tool they already have.
    AmsSystemInfo mounted = make_sys(5, /*current_slot=*/2);
    AmsSystemInfo empty = make_sys(5, /*current_slot=*/-1);

    PanelOutcome a = panel_execute_load(mounted, seated_toolchanger(), 2, true);
    PanelOutcome b = panel_execute_load(empty, fresh_ams(), -1, true);
    CHECK(a.toast != b.toast);
    CHECK(a.navigated_to_ams != b.navigated_to_ams);
}

// =============================================================================
// Load — tier 1 entry points
// =============================================================================

TEST_CASE("Load with nothing seated calls load_filament on the selected slot",
          "[filament][dispatch][wiring]") {
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/-1);

    PanelOutcome out = panel_execute_load(sys, fresh_ams(), /*target_slot=*/2,
                                          /*macro_available=*/true);
    CHECK(out.arm == Arm::Backend);
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

    PanelOutcome out = panel_execute_load(sys, seated, /*target_slot=*/1,
                                          /*macro_available=*/true);
    CHECK(out.arm == Arm::Backend);
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

    PanelOutcome out = panel_execute_load(sys, seated, /*target_slot=*/1,
                                          /*macro_available=*/true);
    CHECK(out.arm == Arm::Backend);
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
    BackendCaps bypassed = fresh_ams();
    bypassed.requires_slot_selection_for_load = false;

    PanelOutcome out = panel_execute_load(sys, bypassed, /*target_slot=*/-2,
                                          /*macro_available=*/true);
    CHECK(out.arm == Arm::Macro);
    // The backend guard belongs to the backend arm; the macro arm's guard is
    // armed inside run_filament_macro, after any param modal is answered.
    CHECK_FALSE(out.guard_armed);
}

TEST_CASE("Load with no backend and no macro falls back to raw gcode",
          "[filament][dispatch][wiring]") {
    AmsSystemInfo sys = make_sys(0, -1);
    BackendCaps none{};

    PanelOutcome out = panel_execute_load(sys, none, /*target_slot=*/-1,
                                          /*macro_available=*/false);
    CHECK(out.arm == Arm::RawGcode);
}

// =============================================================================
// Unload — deliberately asymmetric with load
// =============================================================================

TEST_CASE("Unload with nothing loaded warns and dispatches nothing",
          "[filament][dispatch][wiring]") {
    PanelOutcome out = panel_execute_unload(fresh_ams(), /*target_slot=*/2,
                                            /*target_is_loaded=*/false, /*macro_available=*/true);
    CHECK(out.arm == Arm::Refused);
    CHECK(out.toast == "No filament loaded to unload");
    CHECK_FALSE(out.guard_armed);
    // The unload refusal never redirects — the panel already knows the slot.
    CHECK_FALSE(out.navigated_to_ams);
}

TEST_CASE("Unload passes the panel's selected slot to unload_filament",
          "[filament][dispatch][wiring]") {
    // Re-resolving current_slot inside the backend was the U1 wrong-tool bug;
    // the plan must carry the slot the panel asked for.
    PanelOutcome out = panel_execute_unload(fresh_ams(), /*target_slot=*/3,
                                            /*target_is_loaded=*/true, /*macro_available=*/true);
    CHECK(out.arm == Arm::Backend);
    CHECK(out.call == AmsCall::Unload);
    CHECK(out.arg == 3);
    CHECK(out.guard_armed);
}

TEST_CASE("Unload under bypass stays on the backend while load falls through",
          "[filament][dispatch][wiring][bypass]") {
    // The asymmetry, asserted from the panel's side: harmonizing the two gates
    // would run the user's unload macro twice under AFC bypass.
    BackendCaps bypassed = fresh_ams();
    bypassed.requires_slot_selection_for_load = false;
    AmsSystemInfo sys = make_sys(4, /*current_slot=*/0);

    PanelOutcome unload = panel_execute_unload(bypassed, /*target_slot=*/0,
                                               /*target_is_loaded=*/true, /*macro_available=*/true);
    PanelOutcome load = panel_execute_load(sys, bypassed, /*target_slot=*/0,
                                           /*macro_available=*/true);
    CHECK(unload.arm == Arm::Backend);
    CHECK(load.arm == Arm::Macro);
}

TEST_CASE("Unload with no backend falls through macro then raw gcode",
          "[filament][dispatch][wiring]") {
    BackendCaps none{};

    PanelOutcome with_macro = panel_execute_unload(none, /*target_slot=*/-1,
                                                   /*target_is_loaded=*/false,
                                                   /*macro_available=*/true);
    CHECK(with_macro.arm == Arm::Macro);

    PanelOutcome without = panel_execute_unload(none, /*target_slot=*/-1,
                                                /*target_is_loaded=*/false,
                                                /*macro_available=*/false);
    CHECK(without.arm == Arm::RawGcode);
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
    auto out = panel_execute_unload(fresh_ams(), EXTERNAL_SPOOL_SLOT, /*target_is_loaded=*/true,
                                    /*macro_available=*/true);
    CHECK(out.arm == Arm::Backend);
    CHECK(out.arg == EXTERNAL_SPOOL_SLOT);
    CHECK(out.armed_manual_pull);
}

TEST_CASE("Unloading an AMS lane arms no manual-pull prompt",
          "[filament][dispatch][wiring][bypass]") {
    // The lane reels its own filament back. Prompting here would fire on every
    // ordinary unload, which is how a helpful toast turns into noise.
    auto out = panel_execute_unload(fresh_ams(), /*target_slot=*/2, /*target_is_loaded=*/true,
                                    /*macro_available=*/true);
    REQUIRE(out.arm == Arm::Backend);
    CHECK_FALSE(out.armed_manual_pull);
}

TEST_CASE("A backend-less unload arms the prompt on every tier",
          "[filament][dispatch][wiring][bypass]") {
    // No AMS at all: the spool sits on a holder and feeds the toolhead directly,
    // so both the macro and the raw-gcode fallback leave the same manual job.
    BackendCaps none{};

    auto via_macro = panel_execute_unload(none, /*target_slot=*/-1, /*target_is_loaded=*/false,
                                          /*macro_available=*/true);
    REQUIRE(via_macro.arm == Arm::Macro);
    CHECK(via_macro.armed_manual_pull);

    auto via_gcode = panel_execute_unload(none, /*target_slot=*/-1, /*target_is_loaded=*/false,
                                          /*macro_available=*/false);
    REQUIRE(via_gcode.arm == Arm::RawGcode);
    CHECK(via_gcode.armed_manual_pull);
}

TEST_CASE("A refused unload arms nothing", "[filament][dispatch][wiring][bypass]") {
    // Nothing was dispatched, so nothing will ever complete to disarm it. An arm
    // here would sit waiting for the next unrelated toolhead-sensor edge.
    auto out = panel_execute_unload(fresh_ams(), EXTERNAL_SPOOL_SLOT, /*target_is_loaded=*/false,
                                    /*macro_available=*/true);
    REQUIRE(out.arm == Arm::Refused);
    CHECK_FALSE(out.armed_manual_pull);
}
