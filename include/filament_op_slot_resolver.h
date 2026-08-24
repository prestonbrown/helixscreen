// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file filament_op_slot_resolver.h
 * @brief The per-surface decisions every filament op surface must answer alike.
 *
 * filament_op_dispatch.h answers *which tier* an op takes once the user has
 * committed to it. This header answers the three questions asked BEFORE that,
 * each of which had grown a private copy on every surface:
 *
 *   - which slot do this tool's buttons act on?  resolve_op_button_slot()
 *   - may Load / Unload be pressed right now?    compute_op_button_gating()
 *   - how hot must the nozzle be to load it?     resolve_load_preheat_material()
 *
 * All three are display-free and take plain values, so the whole decision is
 * testable in a binary with no printer and no screen.
 */

#include "active_material_provider.h"
#include "ams_types.h"
#include "filament_database.h"
#include "filament_op_dispatch.h" // EXTERNAL_SPOOL_SLOT — the bypass sentinel both headers key on
#include "print_lifecycle_state.h"

#include <optional>
#include <string>

namespace helix::ui {

/**
 * @brief Resolve which global AMS slot's load-state gates the Load/Unload/Purge
 *        buttons for the currently-selected tool.
 *
 * Resolution order:
 *   1. An explicit tool→slot map entry, when the backend publishes one
 *      (Happy Hare / AFC / properly-mapped toolchangers).
 *   2. No map entry: the meaning of "the tool index" depends on the topology.
 *      - Multi-tool toolchanger (one lane per tool): tool index == slot index.
 *      - Single-extruder multi-lane AMS (e.g. AD5X IFS): one tool is fed by any
 *        of N lanes, so the loaded lane is tracked by current_slot, NOT the tool
 *        index. Using the tool index there collapses to slot 0 and wrongly greys
 *        Unload while another lane is loaded (prestonbrown/helixscreen#1065).
 *
 * @param sys           Backend system info (tool_to_slot_map, current_slot).
 * @param selected_tool Dropdown/active tool index (>= 0).
 * @param tool_count    Number of tools the printer exposes (>1 == toolchanger).
 * @return Global slot index whose load-state gates the buttons,
 *         EXTERNAL_SPOOL_SLOT when bypass is engaged, or -1 if none.
 */
[[nodiscard]] inline int resolve_op_button_slot(const AmsSystemInfo& sys, int selected_tool,
                                                int tool_count) {
    // Bypass is not a lane, so no tool->slot map entry can describe it. While
    // the external spool feeds the toolhead it IS what these buttons act on:
    // CFS publishes a tool->slot map from the box's own `map` (identity fallback
    // on the flat dialect), so resolving through it here handed back lane 0 and
    // gated Unload on an empty bay with filament plainly in the nozzle. AFC and
    // Happy Hare land on the same sentinel from bypass_state / the selector.
    if (sys.current_slot == EXTERNAL_SPOOL_SLOT) {
        return EXTERNAL_SPOOL_SLOT;
    }

    int slot = -1;
    if (selected_tool >= 0 && selected_tool < static_cast<int>(sys.tool_to_slot_map.size())) {
        slot = sys.tool_to_slot_map[selected_tool];
    }
    if (slot < 0) {
        if (tool_count > 1) {
            slot = selected_tool; // toolchanger: tool index == slot index
        } else {
            slot = sys.current_slot; // single-tool multi-lane AMS: loaded lane
        }
    }
    return slot;
}

/// Enabled/disabled state of a surface's Load and Unload/Purge buttons.
struct OpButtonGating {
    bool load_disabled = false;
    bool unload_disabled = false;
};

/**
 * @brief Everything compute_op_button_gating() needs, from any surface.
 *
 * Filled from the live backend + print state. Every field is a question the
 * surface can answer locally; the *combination* is the part that must not fork.
 */
struct OpButtonState {
    /// Filament is at the toolhead for the acted-on slot, so there is nothing to
    /// load into it. On the AMS context menu this is slot_unloads_to_toolhead(),
    /// which is deliberately narrower than the broadened recovery signal.
    bool slot_is_loaded = false;

    /// There is filament in the lane worth feeding. std::nullopt means the
    /// surface cannot answer (no slot resolved, no per-slot presence signal) —
    /// Load then stays reachable so the surface's own refusal path can run and
    /// explain itself (e.g. FilamentRefusal::SelectSlot -> the AMS slot picker).
    std::optional<bool> slot_has_filament{};

    /// This surface's Unload button has something to do. The filament panel
    /// passes its slot's loaded state; the AMS context menu passes
    /// `mode != UnloadMode::Unavailable`, since Eject/Recover are also "unload".
    bool unload_available = false;

    /// AmsSystemInfo::is_busy() — an AMS operation is already running. Every
    /// backend op goes through check_preconditions(), which refuses on busy, so
    /// a button offered here is a guaranteed-failure dead end. This is the term
    /// the filament panel never had: an op started from the AMS panel (or by the
    /// printer itself) left the panel's Load button lit the whole time.
    bool system_busy = false;

    /// A print would make the backend refuse this op. NOT the raw print_active
    /// subject — PAUSED is now allowed on backends whose filament macros do not
    /// home themselves. Always fill this from print_blocks_filament_op() so the
    /// affordance and AmsSubscriptionBackend::refuse_if_printing() cannot drift.
    bool print_blocks_op = false;

    /// This surface's Unload button runs a COLD lane op (Eject / Recover) rather
    /// than a heated toolhead unload. Deliberate asymmetry, not an oversight:
    /// #995 / #1199 keep the cold lane ops reachable mid-print because they move
    /// no toolhead — the backend lets them through check_preconditions(false),
    /// which never consults the print state at all. Do not flatten this into a
    /// blanket print gate.
    bool unload_is_cold_lane_op = false;
};

/**
 * @brief Would a print refuse a toolhead-motion filament op right now?
 *
 * The UI mirror of AmsSubscriptionBackend::refuse_if_printing(). Read that
 * function's comment for the reasoning; the rule it enforces is:
 *
 *   PREPARING                           -> refuse. A host-side pre-start block
 *                                         is homing/probing; a firmware-side
 *                                         PRINT_START is doing the same inside
 *                                         a job that already reads PRINTING.
 *   PRINTING                            -> refuse. The nozzle is laying plastic.
 *   PAUSED, backend homes itself        -> refuse. AD5X IFS only: its
 *                                         `_IFS_REMOVE_CURRENT_PRUTOK` runs a
 *                                         buried `_G28` that probes a loadcell-Z
 *                                         nozzle into the part (bundle XWPBR2DX).
 *   PAUSED, backend does NOT self-home  -> ALLOW. Pause-then-swap is the runout
 *                                         and colour-change recovery workflow on
 *                                         AFC / Happy Hare / CFS / ACE / QIDI /
 *                                         toolchangers / Snapmaker.
 *
 * A UI that keeps greying the paused case makes the backend relaxation invisible
 * — which is the whole user-visible half of the fix. Equally, a UI that offers
 * what the backend refuses is the dead end of bundle JX2FVRB9. One predicate,
 * both directions.
 *
 * Takes the LIFECYCLE, not the raw job state. It used to take a
 * (printing, paused) bool pair read off print_stats.state, which cannot express
 * Preparing - so during a host-side pre-print block both bools were false and
 * this returned "nothing blocks", offering a toolhead-motion filament op while
 * the pre-start G-code was homing and probing. Preparing blocks exactly as
 * PRINTING does; the PAUSED relaxation below is unchanged.
 *
 * @param lifecycle           The derived PrintState (print_lifecycle subject).
 * @param backend_self_homes  AmsBackend::filament_ops_self_home(). Pass false
 *                            when there is no backend — a plain macro/gcode path
 *                            has no firmware macro that could hide a home, and
 *                            Layer 1 (reject_homing_during_active_print) still
 *                            refuses any G28 the app itself emits.
 */
[[nodiscard]] inline bool print_blocks_filament_op(PrintState lifecycle, bool backend_self_homes) {
    // Paused first: job_holds_machine() is true for it too, and the whole point
    // of this predicate is that PAUSED is the one state where the backend's own
    // capability decides.
    if (lifecycle == PrintState::Paused) {
        return backend_self_homes;
    }
    return job_holds_machine(lifecycle);
}

/**
 * @brief SlotInfo presence as an OpButtonState::slot_has_filament answer.
 *
 * SlotStatus::UNKNOWN is "this backend publishes no presence signal for the
 * lane", not "the lane is empty". is_present() collapses the two, so answering
 * with it directly greys Load on any printer that simply never reports slot
 * status. Unanswerable is the honest answer: the button stays live and the
 * backend gets to refuse if the lane really is empty.
 */
[[nodiscard]] inline std::optional<bool> slot_presence(const SlotInfo& slot) {
    if (slot.status == SlotStatus::UNKNOWN) {
        return std::nullopt;
    }
    return slot.is_present();
}

/**
 * @brief Gate Load / Unload from one rule, for every surface that shows them.
 *
 * Load state alone is not enough. Both operations run through
 * AmsSubscriptionBackend::check_preconditions(true), which refuses while the AMS
 * is busy and — per print_blocks_filament_op() — while a print owns the toolhead
 * in a way the backend will not accept. A button offered in either window is a
 * guaranteed-failure dead end, which is exactly what a runout-paused user hits
 * since Klipper's own message tells them to load filament (bundle JX2FVRB9).
 *
 * The filament panel and the AMS context menu each carried a partial version of
 * this: the panel had no `system_busy` and no `slot_has_filament` term, the AMS
 * sidebar had no print term at all. One answer now, three callers.
 */
[[nodiscard]] inline OpButtonGating compute_op_button_gating(const OpButtonState& s) {
    const bool nothing_to_feed = s.slot_has_filament.has_value() && !*s.slot_has_filament;
    return {/*load_disabled=*/s.system_busy || s.print_blocks_op || s.slot_is_loaded ||
                nothing_to_feed,
            /*unload_disabled=*/s.system_busy || !s.unload_available ||
                (s.print_blocks_op && !s.unload_is_cold_lane_op)};
}

// ---------------------------------------------------------------------------
// Load preheat
// ---------------------------------------------------------------------------

/// A resolved preheat target. @c material_name is empty when no source named the
/// material, which is the caller's cue to say "Heating to N°C" without a "for X".
struct PreheatTarget {
    int temp_c = 0;
    std::string material_name;
};

/**
 * @brief The nozzle temperature a LOAD should preheat to for @p mat.
 *
 * nozzle_recommended() — the midpoint of the material's window — NOT nozzle_min.
 *
 * nozzle_min is the bottom edge of the printable range: the temperature below
 * which the material will not flow at all. A load pushes cold filament through
 * the melt zone and usually purges behind it, which is the highest-viscosity
 * demand the hotend ever sees, so sitting exactly on that edge is how you get
 * the grinding/jamming this resolver exists to prevent. Recommended is inside
 * the same window by construction ((min+max)/2 <= max), so it can never ask for
 * a temperature the material cannot take.
 *
 * It is also the number the filament panel already shows everywhere else — the
 * preset buttons and the material temp readout both use nozzle_recommended().
 * Loading at nozzle_min meant one panel advertised two different temperatures
 * for the same material.
 *
 * For a material with no DB entry, build_active_material() synthesises
 * nozzle_max == nozzle_min, so recommended == min and nothing changes there.
 */
[[nodiscard]] inline int load_preheat_temp(const filament::MaterialInfo& mat) {
    return mat.nozzle_recommended();
}

/**
 * @brief The material a LOAD should heat for, from the slot being loaded.
 *
 * Resolution order — and the order is the whole point:
 *
 *   1. The slot the load actually targets. An AMS lane the user picked is the
 *      filament about to pass through the melt zone; nothing outranks it.
 *   2. The external / bypass spool, but ONLY when the load has no AMS lane of
 *      its own (@p target_slot == EXTERNAL_SPOOL_SLOT, or nothing resolved, or
 *      the lane names no material). On an external-spool-only printer this is
 *      the one filament there is.
 *
 * FilamentPanel used to consult the external spool FIRST and unconditionally,
 * then fall back to `AmsSystemInfo::get_active_slot()` — the LOADED lane, not
 * the selected one. Both are wrong for a load and both are silent: selecting
 * tool 3 (PETG) while tool 1 (PLA) was loaded preheated to PLA, and any printer
 * with an external spool assigned preheated every load to that spool's material
 * regardless of the lane. AmsOperationSidebar always had this right; the two now
 * share the rule so they cannot drift apart again.
 *
 * Note the deliberate difference from helix::get_active_material(), which orders
 * AMS-active-slot then external spool. That answers "what is IN the nozzle right
 * now" and is correct for its callers. This answers "what is about to go in",
 * where the target slot — not the active one — is the authority.
 *
 * @param target_slot       Slot the load targets; EXTERNAL_SPOOL_SLOT for bypass,
 *                          < 0 for "none resolved".
 * @param target_slot_info  SlotInfo for @p target_slot, or nullptr when there is
 *                          no backend / no such slot.
 * @param external_spool    The assigned external spool, or nullptr.
 * @return The resolved target, or nullopt when no source names a usable
 *         material — the caller's own tail (a UI material preset, a default)
 *         then applies.
 */
[[nodiscard]] inline std::optional<PreheatTarget>
resolve_load_preheat_material(int target_slot, const SlotInfo* target_slot_info,
                              const SlotInfo* external_spool) {
    auto from = [](const SlotInfo& slot) -> std::optional<PreheatTarget> {
        // A lane with neither a material name nor a vendor temp names nothing.
        // build_active_material() would still hand back a synthetic 220°C
        // default for it, which would mask every lower-priority source the
        // caller has — its own external spool, and the panel's material preset.
        // Same "is this slot speakable" predicate helix::get_active_material()
        // applies before it will build from a slot.
        if (slot.material.empty() && slot.nozzle_temp_min <= 0) {
            return std::nullopt;
        }
        helix::ActiveMaterial mat = helix::build_active_material(slot);
        if (mat.material_info.nozzle_min <= 0) {
            return std::nullopt;
        }
        return PreheatTarget{load_preheat_temp(mat.material_info), mat.display_name};
    };

    // The bypass row IS the external spool — never look at an AMS lane for it.
    if (target_slot == EXTERNAL_SPOOL_SLOT) {
        return external_spool ? from(*external_spool) : std::nullopt;
    }

    if (target_slot_info) {
        if (auto slot_target = from(*target_slot_info)) {
            return slot_target;
        }
    }

    return external_spool ? from(*external_spool) : std::nullopt;
}

} // namespace helix::ui
