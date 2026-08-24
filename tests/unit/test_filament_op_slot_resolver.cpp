// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_op_slot_resolver.cpp
 * @brief Tests for resolve_op_button_slot() — which slot's load-state gates the
 *        FilamentPanel Load/Unload/Purge buttons for the selected tool.
 *
 * Run with: ./build/bin/helix-tests "[filament][op_slot]"
 *
 * Regression guard for prestonbrown/helixscreen#1065: on a single-extruder
 * multi-lane AMS (AD5X native ZMOD IFS) the backend never populates
 * tool_to_slot_map, so the old fallback collapsed the slot to the tool index
 * (0) and read slot 0's load-state — wrongly greying Unload while a *different*
 * lane was loaded to the toolhead. The loaded lane must come from current_slot.
 */

#include "../test_helpers/print_state_test_drivers.h"
#include "filament_op_slot_resolver.h"

#include <functional>
#include <optional>

#include "../catch_amalgamated.hpp"

using helix::ui::resolve_op_button_slot;

namespace {

// Minimal AmsSystemInfo tailored for a resolution case.
AmsSystemInfo make_sys(std::vector<int> tool_map, int current_slot) {
    AmsSystemInfo sys;
    sys.tool_to_slot_map = std::move(tool_map);
    sys.current_slot = current_slot;
    return sys;
}

// Mirror of FilamentPanel::execute_load's target decision (single source of
// truth == resolve_op_button_slot on the dropdown-selected tool). A result
// >= 0 is loaded directly; < 0 means the panel redirects to the AMS slot
// picker instead. FilamentPanel is LVGL/Moonraker-coupled and not
// unit-instantiated (cf. test_filament_op_button_state_char.cpp), so these
// wrappers pin the per-op branching the panel applies on top of the resolver.
int panel_load_target(const AmsSystemInfo& sys, int selected_tool, int tool_count) {
    return resolve_op_button_slot(sys, selected_tool, tool_count);
}

// Mirror of execute_unload's gate: only unload when the dropdown-selected slot
// is actually loaded (slot_is_actively_loaded || slot_has_filament_at_toolhead).
bool panel_unload_allowed(const AmsSystemInfo& sys, int selected_tool, int tool_count,
                          const std::function<bool(int)>& slot_loaded) {
    int slot = resolve_op_button_slot(sys, selected_tool, tool_count);
    return slot >= 0 && slot_loaded(slot);
}

} // namespace

TEST_CASE("AD5X IFS: single tool, no map, loaded lane 1 → slot 1 (Unload enabled)",
          "[filament][op_slot]") {
    // Native ZMOD: tool_to_slot_map is empty, tool 0 fed by lane 1 (current_slot=1).
    AmsSystemInfo sys = make_sys(/*tool_map=*/{}, /*current_slot=*/1);
    // The bug returned 0 here (tool index) → slot 0 not loaded → Unload greyed.
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/0, /*tool_count=*/1) == 1);
}

TEST_CASE("AD5X IFS: single tool, all-unmapped map (-1 fill) still uses current_slot",
          "[filament][op_slot]") {
    // Some backends size the vector but leave entries at -1 (unmapped).
    AmsSystemInfo sys = make_sys(/*tool_map=*/{-1, -1, -1, -1}, /*current_slot=*/2);
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/0, /*tool_count=*/1) == 2);
}

TEST_CASE("Single tool, nothing loaded → -1 (Unload stays disabled)", "[filament][op_slot]") {
    AmsSystemInfo sys = make_sys(/*tool_map=*/{}, /*current_slot=*/-1);
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/0, /*tool_count=*/1) == -1);
}

TEST_CASE("Toolchanger: multi-tool, no map → tool index == slot index", "[filament][op_slot]") {
    // A real toolchanger must keep per-tool selection: selecting tool 2 gates on
    // slot 2, NOT on current_slot (which is the *active* tool's lane).
    AmsSystemInfo sys = make_sys(/*tool_map=*/{}, /*current_slot=*/0);
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/2, /*tool_count=*/4) == 2);
}

TEST_CASE("Explicit tool→slot map wins over both fallbacks", "[filament][op_slot]") {
    AmsSystemInfo sys = make_sys(/*tool_map=*/{3, 1, 0}, /*current_slot=*/1);
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/0, /*tool_count=*/1) == 3);
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/2, /*tool_count=*/4) == 0);
}

TEST_CASE("Mapped entry of -1 falls through to topology fallback", "[filament][op_slot]") {
    // tool 1 explicitly unmapped (-1) on a single-tool system → current_slot.
    AmsSystemInfo sys = make_sys(/*tool_map=*/{-1}, /*current_slot=*/3);
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/0, /*tool_count=*/1) == 3);
}

TEST_CASE("BoxTurtle AFC: selected tool's lane wins over the loaded current_slot",
          "[filament][op_slot]") {
    // Live-captured .112 BoxTurtle: 4 lanes, identity tool->slot map, lane4 (slot 3)
    // loaded to the toolhead, dropdown defaulted to T0. The op slot must follow the
    // SELECTED tool (T0 -> slot 0), never the loaded current_slot (3). That divergence
    // made Load act on the already-loaded lane instead of the selected one.
    AmsSystemInfo sys = make_sys(/*tool_map=*/{0, 1, 2, 3}, /*current_slot=*/3);
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/0, /*tool_count=*/4) == 0);
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/3, /*tool_count=*/4) == 3);

    // AFC "map" remap (non-identity): T0 -> lane3. Still follows the selection.
    AmsSystemInfo remapped = make_sys(/*tool_map=*/{3, 2, 1, 0}, /*current_slot=*/1);
    CHECK(resolve_op_button_slot(remapped, /*selected_tool=*/0, /*tool_count=*/4) == 3);
}

TEST_CASE("Bypass: the external spool outranks any tool->slot map", "[filament][op_slot][bypass]") {
    // K2 / stock CFS with bypass engaged. CFS populates tool_to_slot_map from
    // the box's own `map` (and falls back to identity on the flat dialect), so
    // the map resolves T0 -> lane 0 and the buttons gated on an EMPTY BAY while
    // the external spool was plainly threaded through the nozzle. Bypass is not
    // a lane; no map entry can describe it, so it has to short-circuit.
    AmsSystemInfo bypassed = make_sys(/*tool_map=*/{0, 1, 2, 3}, /*current_slot=*/-2);
    CHECK(resolve_op_button_slot(bypassed, /*selected_tool=*/0, /*tool_count=*/1) ==
          helix::ui::EXTERNAL_SPOOL_SLOT);

    // AFC/Happy Hare reach the same sentinel by a different road (bypass_state /
    // the selector position), and a non-identity remap must not rescue a lane.
    AmsSystemInfo remapped = make_sys(/*tool_map=*/{3, 2, 1, 0}, /*current_slot=*/-2);
    CHECK(resolve_op_button_slot(remapped, /*selected_tool=*/0, /*tool_count=*/4) ==
          helix::ui::EXTERNAL_SPOOL_SLOT);

    // The guard rail: with bypass OFF the map still wins, exactly as before.
    // This is the BoxTurtle contract above and must not become collateral.
    AmsSystemInfo normal = make_sys(/*tool_map=*/{0, 1, 2, 3}, /*current_slot=*/3);
    CHECK(resolve_op_button_slot(normal, /*selected_tool=*/0, /*tool_count=*/4) == 0);

    // -1 is "nothing resolved", not a target. It must NOT be swept up by the
    // sentinel arm and start acting on the external spool.
    AmsSystemInfo nothing = make_sys(/*tool_map=*/{}, /*current_slot=*/-1);
    CHECK(resolve_op_button_slot(nothing, /*selected_tool=*/0, /*tool_count=*/1) == -1);
}

// Snapmaker U1 regression guard (commit 504905a2 "Unload visits T0 first").
// U1 = 4 virtual tools T0..T3 with an identity tool->slot map
// (ams_backend_snapmaker.cpp), current_slot == current_tool == the picked-up
// toolhead. The original bug came from a stuck current_slot == -1 forcing a
// bare-default (T0) unload. Resolving the op slot from the dropdown-selected
// tool through the identity map must (a) match current_slot in steady state
// and (b) STILL yield the selected tool even when current_slot is stuck at -1,
// so a wrong-tool unload can't recur.
TEST_CASE("Snapmaker U1: identity map resolves the selected tool, immune to stuck current_slot",
          "[filament][op_slot]") {
    // Bart's scenario: T3 picked up, dropdown synced to the active tool T3.
    AmsSystemInfo u1 = make_sys(/*tool_map=*/{0, 1, 2, 3}, /*current_slot=*/3);
    CHECK(resolve_op_button_slot(u1, /*selected_tool=*/3, /*tool_count=*/4) == 3); // NOT 0

    // Divergence (user picks a different tool during the async change): follow dropdown.
    CHECK(resolve_op_button_slot(u1, /*selected_tool=*/1, /*tool_count=*/4) == 1);

    // Original root-cause state: current_slot stuck at -1. Identity map still
    // yields the selected tool — no bare-default T0 unload.
    AmsSystemInfo u1_stuck = make_sys(/*tool_map=*/{0, 1, 2, 3}, /*current_slot=*/-1);
    CHECK(resolve_op_button_slot(u1_stuck, /*selected_tool=*/3, /*tool_count=*/4) == 3);
}

// Characterization of the FilamentPanel op-target contract: gating, Load, and
// Unload all resolve their slot the SAME way (selected_op_slot ==
// resolve_op_button_slot on the dropdown tool). Load acts on the resolved slot
// or redirects when < 0; Unload only fires when that slot is loaded.
TEST_CASE("Panel op-target contract: Load follows the dropdown, Unload gated on that slot",
          "[filament][op_slot][char]") {
    auto only_slot3_loaded = [](int s) { return s == 3; };

    // BoxTurtle: T0 selected while lane4 (slot 3) is loaded. Load targets slot 0
    // (the selection), NOT the loaded slot 3 — the exact bug this fix closes.
    AmsSystemInfo bt = make_sys(/*tool_map=*/{0, 1, 2, 3}, /*current_slot=*/3);
    CHECK(panel_load_target(bt, /*T0=*/0, /*tool_count=*/4) == 0);
    // Unload of T0 is refused (slot 0 not loaded → button greyed); Unload of T3
    // is allowed and targets the loaded slot 3.
    CHECK_FALSE(panel_unload_allowed(bt, /*T0=*/0, 4, only_slot3_loaded));
    CHECK(panel_unload_allowed(bt, /*T3=*/3, 4, only_slot3_loaded));

    // U1: T3 selected == loaded. Unload targets 3, never the bare-default 0.
    AmsSystemInfo u1 = make_sys(/*tool_map=*/{0, 1, 2, 3}, /*current_slot=*/3);
    CHECK(panel_load_target(u1, /*T3=*/3, 4) == 3);
    CHECK(panel_unload_allowed(u1, /*T3=*/3, 4, only_slot3_loaded));

    // AD5X IFS single-tool multi-lane, nothing loaded (no map, current_slot -1):
    // Load target is -1 → the panel redirects to the AMS slot picker.
    CHECK(panel_load_target(make_sys(/*tool_map=*/{}, /*current_slot=*/-1), /*T0=*/0, 1) == -1);
}

// ---------------------------------------------------------------------------
// Print-state gating (bundle JX2FVRB9, bundle XWPBR2DX)
// ---------------------------------------------------------------------------
//
// Load/Unload both run through AmsSubscriptionBackend::check_preconditions(true)
// -> refuse_if_printing(). The panel gated on load state alone, so during a
// runout pause the Load button stayed lit and every tap produced a "Cannot run
// filament operation while printing" toast.
//
// print_blocks_filament_op() is the UI mirror of that guard, and it is NOT
// print_occupies_toolhead(): PRINTING always refuses, but a PAUSED print is now
// allowed on every backend whose filament macro does not home itself. Only AD5X
// IFS self-homes (`_IFS_REMOVE_CURRENT_PRUTOK` runs a buried `_G28` that probes a
// loadcell-Z nozzle into the part).
//
// Mutation check: return `printing || paused` and the "paused, no self-home"
// cases fail; return only `printing` and the AD5X cases fail.
TEST_CASE("print_blocks_filament_op mirrors refuse_if_printing",
          "[filament][op_slot][print_guard]") {
    using helix::ui::print_blocks_filament_op;

    SECTION("idle never blocks") {
        CHECK_FALSE(print_blocks_filament_op(PrintState::Idle, false));
        CHECK_FALSE(print_blocks_filament_op(PrintState::Idle, true));
    }

    SECTION("PRINTING always blocks, self-homing or not") {
        CHECK(print_blocks_filament_op(PrintState::Printing, /*self_homes=*/false));
        CHECK(print_blocks_filament_op(PrintState::Printing, /*self_homes=*/true));
    }

    SECTION("PREPARING blocks like PRINTING, on every backend") {
        // The hole this signature change closes. On the bool pair a host-side
        // pre-start block read (printing=false, paused=false) - indistinguishable
        // from idle - so a toolhead-motion op was offered while the pre-start
        // G-code homed and probed. The backend's self-homing capability is
        // irrelevant here: the app's own block is already moving the toolhead.
        CHECK(print_blocks_filament_op(PrintState::Preparing, /*self_homes=*/false));
        CHECK(print_blocks_filament_op(PrintState::Preparing, /*self_homes=*/true));
    }

    SECTION("PAUSED blocks only on a self-homing backend") {
        // AFC / Happy Hare / CFS / ACE / QIDI / toolchanger / Snapmaker:
        // pause-then-swap is the runout recovery workflow, and it works.
        CHECK_FALSE(print_blocks_filament_op(PrintState::Paused, /*self_homes=*/false));
        // AD5X IFS.
        CHECK(print_blocks_filament_op(PrintState::Paused, /*self_homes=*/true));
    }

    SECTION("terminal states never block") {
        // A finished job does not own the toolhead. Worth pinning: on the
        // lifecycle these are distinct values rather than "neither bool set",
        // so a sloppy job_holds_machine() would be caught here.
        for (PrintState terminal :
             {PrintState::Complete, PrintState::Cancelled, PrintState::Error}) {
            CHECK_FALSE(print_blocks_filament_op(terminal, false));
            CHECK_FALSE(print_blocks_filament_op(terminal, true));
        }
    }
}

// Mutation check: drop `|| s.print_blocks_op` from compute_op_button_gating's
// load_disabled term and "a print owning the toolhead disables both" fails.
TEST_CASE("compute_op_button_gating: print state gates Load and Unload",
          "[filament][op_slot][print_guard]") {
    using helix::ui::compute_op_button_gating;
    using helix::ui::OpButtonState;
    using helix::ui::print_blocks_filament_op;

    // The filament panel's shape: Unload acts on whatever is at the toolhead for
    // the selected slot, and it is always the heated unload. Print state is fed
    // through the shared predicate, exactly as update_filament_op_buttons() does.
    auto panel = [](bool is_loaded, bool printing, bool paused, bool self_homes) {
        OpButtonState s;
        s.slot_is_loaded = is_loaded;
        s.unload_available = is_loaded;
        s.print_blocks_op = print_blocks_filament_op(
            helix::test::lifecycle_from_bools(printing, paused), self_homes);
        return compute_op_button_gating(s);
    };

    SECTION("no print: load state alone decides, as before") {
        auto empty = panel(/*is_loaded=*/false, false, false, false);
        CHECK_FALSE(empty.load_disabled); // can load
        CHECK(empty.unload_disabled);     // nothing to unload

        auto loaded = panel(/*is_loaded=*/true, false, false, false);
        CHECK(loaded.load_disabled);         // already loaded
        CHECK_FALSE(loaded.unload_disabled); // can unload
    }

    SECTION("PRINTING disables both") {
        auto empty = panel(/*is_loaded=*/false, /*printing=*/true, false, false);
        CHECK(empty.load_disabled);
        CHECK(empty.unload_disabled);

        auto loaded = panel(/*is_loaded=*/true, /*printing=*/true, false, false);
        CHECK(loaded.load_disabled);
        CHECK(loaded.unload_disabled);
    }

    SECTION("a PAUSED print leaves both usable on a backend that does not self-home") {
        // The runout-pause recovery. Load state still decides which one is live.
        auto empty = panel(/*is_loaded=*/false, false, /*paused=*/true, /*self_homes=*/false);
        CHECK_FALSE(empty.load_disabled);
        CHECK(empty.unload_disabled); // nothing to unload, not a print refusal

        auto loaded = panel(/*is_loaded=*/true, false, /*paused=*/true, /*self_homes=*/false);
        CHECK(loaded.load_disabled); // already loaded, not a print refusal
        CHECK_FALSE(loaded.unload_disabled);
    }

    SECTION("a PAUSED print still disables both on AD5X IFS") {
        auto loaded = panel(/*is_loaded=*/true, false, /*paused=*/true, /*self_homes=*/true);
        CHECK(loaded.load_disabled);
        CHECK(loaded.unload_disabled);
    }
}

// The panel's rule had NO system_busy term, so an AMS op started from the AMS
// panel (or by the printer itself) left the filament panel's Load button lit the
// whole time it ran — and every tap produced a backend "busy" refusal.
// AmsSystemInfo::is_busy() is the predicate check_preconditions() refuses on.
//
// Mutation check: drop `s.system_busy ||` from either term and this fails.
TEST_CASE("compute_op_button_gating: a busy AMS disables both, print or not",
          "[filament][op_slot][op_gating]") {
    using helix::ui::compute_op_button_gating;
    using helix::ui::OpButtonState;

    OpButtonState s;
    s.slot_has_filament = true;
    s.unload_available = true;
    s.system_busy = true;

    auto gating = compute_op_button_gating(s);
    CHECK(gating.load_disabled);
    CHECK(gating.unload_disabled);

    // Busy even outranks a cold lane op, which print state does not.
    s.unload_is_cold_lane_op = true;
    CHECK(compute_op_button_gating(s).unload_disabled);
}

// The panel had no slot_has_filament term either: Load stayed enabled on an
// empty lane. The tri-state is what makes adopting it safe — SlotStatus::UNKNOWN
// means "this backend publishes no presence signal", and reading that as "empty"
// would grey Load on printers that simply never report it.
//
// Mutation check: change nothing_to_feed from
// `s.slot_has_filament.has_value() && !*s.slot_has_filament` to
// `!s.slot_has_filament.value_or(false)` and the unanswerable section fails;
// drop the term entirely and the known-empty section fails.
TEST_CASE("compute_op_button_gating: an empty lane blocks Load, an unknown one does not",
          "[filament][op_slot][op_gating]") {
    using helix::ui::compute_op_button_gating;
    using helix::ui::OpButtonState;

    OpButtonState s;

    SECTION("lane known to hold filament: Load offered") {
        s.slot_has_filament = true;
        CHECK_FALSE(compute_op_button_gating(s).load_disabled);
    }

    SECTION("lane known to be empty: Load refused") {
        s.slot_has_filament = false;
        CHECK(compute_op_button_gating(s).load_disabled);
    }

    SECTION("unanswerable: Load stays reachable so the surface can refuse and explain") {
        s.slot_has_filament = std::nullopt;
        CHECK_FALSE(compute_op_button_gating(s).load_disabled);
    }

    SECTION("presence never affects Unload — that is what unload_available is for") {
        s.unload_available = true;
        s.slot_has_filament = false;
        CHECK_FALSE(compute_op_button_gating(s).unload_disabled);
    }
}

// slot_presence() is the shared SlotStatus -> tri-state mapping, so the panel and
// the context menu cannot answer "is this lane empty" differently.
TEST_CASE("slot_presence: UNKNOWN is unanswerable, not empty", "[filament][op_slot][op_gating]") {
    using helix::ui::slot_presence;

    SlotInfo slot;
    slot.status = SlotStatus::UNKNOWN;
    CHECK_FALSE(slot_presence(slot).has_value());

    slot.status = SlotStatus::EMPTY;
    REQUIRE(slot_presence(slot).has_value());
    CHECK_FALSE(*slot_presence(slot));

    for (auto s : {SlotStatus::AVAILABLE, SlotStatus::LOADED, SlotStatus::FROM_BUFFER,
                   SlotStatus::BLOCKED}) {
        slot.status = s;
        REQUIRE(slot_presence(slot).has_value());
        CHECK(*slot_presence(slot));
    }
}

// The deliberate asymmetry (#995 / #1199): the AMS context menu keeps the COLD
// lane ops — Eject / Recover — reachable mid-print, because they move no
// toolhead and the backend permits them via check_preconditions(false). Only the
// heated Unload proper is blocked. Flattening this into a blanket print gate
// would strand filament the user could have ejected.
//
// Mutation check: delete `&& !s.unload_is_cold_lane_op` and the first section fails.
TEST_CASE("compute_op_button_gating: cold lane ops survive a print, heated Unload does not",
          "[filament][op_slot][op_gating]") {
    using helix::ui::compute_op_button_gating;
    using helix::ui::OpButtonState;

    OpButtonState s;
    s.unload_available = true;
    s.print_blocks_op = true;

    SECTION("Eject / Recover stay offered mid-print") {
        s.unload_is_cold_lane_op = true;
        CHECK_FALSE(compute_op_button_gating(s).unload_disabled);
    }

    SECTION("the heated toolhead Unload is refused") {
        s.unload_is_cold_lane_op = false;
        CHECK(compute_op_button_gating(s).unload_disabled);
    }

    SECTION("a cold op with nothing to do is still refused") {
        s.unload_is_cold_lane_op = true;
        s.unload_available = false;
        CHECK(compute_op_button_gating(s).unload_disabled);
    }
}

// The AMS sidebar's Unload button had NO print term at all — it stayed tappable
// during a print or pause, dispatched, and ate "Cannot run filament operation
// while printing" (raised while merely PAUSED; live Discord report). Its shape:
// availability is the aggregate ams_filament_loaded flag, and it is always the
// heated unload.
//
// Note the two halves of that report. Before, the button was live and the op was
// refused. Now the button is live *and the op succeeds*, because a paused unload
// is permitted on every backend but AD5X — so the gate must not simply become
// "grey it whenever print_active", or the fix would be invisible.
//
// Mutation check: hardcode unload_is_cold_lane_op = true in the sidebar's
// read_unload_gating_state() and the PRINTING cases fail; feed the raw
// print_active subject instead of print_blocks_filament_op() and the PAUSED
// non-self-homing case fails.
TEST_CASE("compute_op_button_gating: the AMS sidebar Unload answers the same rule",
          "[filament][op_slot][op_gating]") {
    using helix::ui::compute_op_button_gating;
    using helix::ui::OpButtonState;

    // Mirror of AmsOperationSidebar::read_unload_gating_state().
    auto sidebar = [](bool filament_loaded, bool system_busy, bool printing, bool paused,
                      bool self_homes) {
        OpButtonState s;
        s.unload_available = filament_loaded;
        s.system_busy = system_busy;
        s.print_blocks_op = helix::ui::print_blocks_filament_op(
            helix::test::lifecycle_from_bools(printing, paused), self_homes);
        s.unload_is_cold_lane_op = false;
        return compute_op_button_gating(s);
    };

    // Idle: enabled iff something is loaded.
    CHECK_FALSE(sidebar(true, false, false, false, false).unload_disabled);
    CHECK(sidebar(false, false, false, false, false).unload_disabled);
    // Busy AMS.
    CHECK(sidebar(true, true, false, false, false).unload_disabled);
    // PRINTING: refused everywhere.
    CHECK(sidebar(true, false, /*printing=*/true, false, false).unload_disabled);
    CHECK(sidebar(true, false, /*printing=*/true, false, /*self_homes=*/true).unload_disabled);
    // PAUSED: the live Discord report. Offered on every backend but AD5X.
    CHECK_FALSE(sidebar(true, false, false, /*paused=*/true, /*self_homes=*/false).unload_disabled);
    CHECK(sidebar(true, false, false, /*paused=*/true, /*self_homes=*/true).unload_disabled);
}
