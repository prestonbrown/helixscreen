// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file filament_op_slot_resolver.h
 * @brief The per-surface decisions every filament op surface must answer alike.
 *
 * filament_op_dispatch.h answers *which tier* an op takes once the user has
 * committed to it. This header answers the questions asked BEFORE that, each of
 * which had grown a private copy on every surface:
 *
 *   - which slot do this tool's buttons act on?  resolve_op_button_slot()
 *   - may Load / Unload be pressed right now?    compute_op_button_gating()
 *   - may Reset / Check slots be pressed?        compute_machine_op_gating()
 *   - how hot must the nozzle be to load it?     resolve_load_preheat_material()
 *
 * All of them are display-free and take plain values, so the whole decision is
 * testable in a binary with no printer and no screen.
 */

#include "active_material_provider.h"
#include "ams_types.h"
#include "filament_database.h"
#include "filament_op_dispatch.h" // kExternalSpoolSlot / kActiveHeadSlot, the sentinels both headers key on
#include "print_lifecycle_state.h"

#include <algorithm>
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
 * An unaccounted toolhead (filament at the nozzle that no lane claims) resolves
 * to kActiveHeadSlot before the map is consulted: the selected tool's lane is
 * not what is in the nozzle, and the AD5X IFS module publishes four virtual
 * tools with an identity map, so a dropdown that always names a tool would
 * otherwise never reach the active head (prestonbrown/helixscreen#1324).
 *
 * @param sys                  Backend system info (tool_to_slot_map, current_slot).
 * @param selected_tool        Dropdown/active tool index (>= 0).
 * @param tool_count           Number of tools the printer exposes (>1 == toolchanger).
 * @param toolhead_unaccounted The backend's confident toolhead_filament_unaccounted().
 * @return Global slot index whose load-state gates the buttons,
 *         kExternalSpoolSlot when bypass is engaged, kActiveHeadSlot for an
 *         unaccounted toolhead, or -1 if none.
 */
[[nodiscard]] inline int resolve_op_button_slot(const AmsSystemInfo& sys, int selected_tool,
                                                int tool_count, bool toolhead_unaccounted = false) {
    // Bypass is not a lane, so no tool->slot map entry can describe it. While
    // the external spool feeds the toolhead it IS what these buttons act on:
    // CFS publishes a tool->slot map from the box's own `map` (identity fallback
    // on the flat dialect), so resolving through it here handed back lane 0 and
    // gated Unload on an empty bay with filament plainly in the nozzle. AFC and
    // Happy Hare land on the same sentinel from bypass_state / the selector.
    if (sys.current_slot == kExternalSpoolSlot) {
        return kExternalSpoolSlot;
    }
    if (toolhead_unaccounted) {
        return kActiveHeadSlot;
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

/// Enabled/disabled state of a surface's Load, Unload and Purge buttons.
struct OpButtonGating {
    bool load_disabled = false;
    bool unload_disabled = false;
    /// Purge asks strictly less than Load: there is no lane to be empty and
    /// nothing has to already be at the toolhead, only a machine free to
    /// extrude. Stated here so the next surface to grow a Purge button reads
    /// the answer instead of deriving a second one.
    bool purge_disabled = false;
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

    /// Whether a print refuses LOAD in particular: print_blocks_filament_load(),
    /// which lets a paused job load on a backend whose firmware home is skipped
    /// once homed. Unset means Load answers to print_blocks_op like the rest.
    std::optional<bool> print_blocks_load{};

    /// Load feeds filament into a head the machine already shows as loaded
    /// (AmsBackend::load_feeds_filament()), so "loaded" does not refuse it.
    bool load_feeds_filament = false;

    /// This surface's Unload button runs a COLD lane op (Eject / Recover) rather
    /// than a heated toolhead unload. Deliberate asymmetry, not an oversight:
    /// #995 / #1199 keep the cold lane ops reachable mid-print because they move
    /// no toolhead — the backend lets them through check_preconditions(false),
    /// which never consults the print state at all. Do not flatten this into a
    /// blanket print gate.
    bool unload_is_cold_lane_op = false;
};

/// The print half of the gating question is defined once, beside
/// job_holds_machine(), because AmsSubscriptionBackend::refuse_if_printing() is
/// its other caller and the printer layer must not reach through a UI header to
/// get it. Re-exported here so this header stays the one place a filament-op
/// surface has to read.
using helix::print_blocks_filament_load;
using helix::print_blocks_filament_op;

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
    return {/*load_disabled=*/s.system_busy || s.print_blocks_load.value_or(s.print_blocks_op) ||
                (s.slot_is_loaded && !s.load_feeds_filament) || nothing_to_feed,
            /*unload_disabled=*/s.system_busy || !s.unload_available ||
                (s.print_blocks_op && !s.unload_is_cold_lane_op),
            /*purge_disabled=*/s.system_busy || s.print_blocks_op};
}

/// Enabled/disabled state of the AMS sidebar's Reset and Check-slots buttons.
struct MachineOpGating {
    bool reset_disabled = false;
    bool check_gates_disabled = false;
};

/**
 * @brief Gate the AMS sidebar's Reset and Check-slots buttons on the print.
 *
 * Neither button routes through check_preconditions(true): every backend's
 * reset() calls check_preconditions() with the default
 * requires_toolhead_motion = false, and check_all_gates() checks only that the
 * backend is running. refuse_if_printing() therefore never runs for either, and
 * the affordance is the only guard there is.
 *
 * The two answer differently because their hazard does:
 *
 *   - **Reset** is one button over many backends, and on most of them it is a
 *     state-only fault clear the user reaches for *during* a job (CFS
 *     BOX_ERROR_CLEAR, AFC's lane-picker prompt). Greying those would delete
 *     the recovery path exactly when it is needed, so only a backend whose
 *     reset moves filament may be greyed — AmsBackend::reset_moves_filament().
 *   - **Check slots** appears only where supports_gate_check() is true, and
 *     probing a gate is physical by definition: Happy Hare's MMU_CHECK_GATE
 *     parks the toolhead and unloads/reloads each gate to test its sensor. A
 *     print always blocks it.
 *
 * @param print_blocks_op       print_blocks_filament_op(lifecycle, self_homes).
 *                              PAUSED is already folded in there, so a paused
 *                              job keeps both buttons live on backends that do
 *                              not self-home — the pause-then-fix workflow.
 * @param reset_moves_filament  AmsBackend::reset_moves_filament(); false when
 *                              there is no backend, since a Reset that reaches
 *                              no backend moves nothing.
 */
[[nodiscard]] inline MachineOpGating compute_machine_op_gating(bool print_blocks_op,
                                                               bool reset_moves_filament) {
    return {/*reset_disabled=*/print_blocks_op && reset_moves_filament,
            /*check_gates_disabled=*/print_blocks_op};
}

/**
 * @brief Whether Clear Spool must refuse on this lane right now.
 *
 * Clearing erases what HelixScreen and the firmware remember about the lane,
 * and while a job holds the machine, the lane actively loaded into the toolhead
 * is the one that job is drawing from. Clearing it mid-job wipes the material
 * and colour every print surface is displaying for the running print, so the
 * gesture refuses until the job ends. Other lanes stay clearable (the job is
 * not printing from them), and a free machine clears any lane, loaded or not.
 *
 * job_holds_machine(), deliberately NOT print_blocks_filament_op(): that
 * predicate's whole point is letting PAUSED through for the pause-then-swap
 * filament ops, but a clear is not moving filament: it is deleting the
 * feeding lane's identity out from under the job that will resume from it.
 * Preparing refuses for the same reason: the job is committed and will draw
 * from this lane.
 *
 * Both surfaces that ask (the context menu's affordance and
 * ams_dispatch_backend_action()'s guard) call this, so the rule cannot fork
 * between what is offered and what is refused.
 *
 * @param lifecycle             The derived PrintState (print_lifecycle subject).
 * @param slot_actively_loaded  AmsBackend::slot_is_actively_loaded(slot): the
 *                              backend's own answer for "is this the lane at
 *                              the toolhead".
 */
[[nodiscard]] inline bool clear_spool_blocked_by_print(PrintState lifecycle,
                                                       bool slot_actively_loaded) {
    return job_holds_machine(lifecycle) && slot_actively_loaded;
}

/**
 * @brief The AMS sidebar's Unload button as an OpButtonState.
 *
 * That button has a fixed shape: its availability is the aggregate "something is
 * loaded" flag rather than a per-slot answer, and it is ALWAYS the heated
 * toolhead unload — the cold lane ops (Eject / Recover), which stay reachable
 * mid-print, live on the AMS context menu instead. The field mapping is the part
 * that must not fork: hardcoding unload_is_cold_lane_op = false is what makes
 * PRINTING refuse here, so a surface that filled it any other way would silently
 * re-open bundle JX2FVRB9's dead end.
 *
 * Split out of AmsOperationSidebar::read_unload_gating_state(), which is now the
 * singleton reads plus one call to this.
 *
 * @param filament_loaded    ams_filament_loaded — something is at the toolhead.
 * @param system_busy        AmsSystemInfo::is_busy(); false when there is no backend.
 * @param lifecycle          The derived PrintState (print_lifecycle subject).
 * @param backend_self_homes AmsBackend::filament_ops_self_home(); false when
 *                           there is no backend, per print_blocks_filament_op().
 */
/// The two stops every sidebar filament op shares: an op already running, and
/// a print owning the toolhead. print_blocks_filament_op(), not the raw
/// print_active subject: PRINTING always refuses, but a PAUSED print ALLOWS the
/// op on every backend whose filament macro does not home itself (only AD5X IFS
/// does). Gating on print_active would grey the buttons through the pause that
/// is the entire recovery workflow.
[[nodiscard]] inline OpButtonState op_blocked_state(bool system_busy, PrintState lifecycle,
                                                    bool backend_self_homes) {
    OpButtonState s;
    s.system_busy = system_busy;
    s.print_blocks_op = print_blocks_filament_op(lifecycle, backend_self_homes);
    return s;
}

[[nodiscard]] inline OpButtonState build_unload_gating_state(bool filament_loaded, bool system_busy,
                                                             PrintState lifecycle,
                                                             bool backend_self_homes) {
    OpButtonState s = op_blocked_state(system_busy, lifecycle, backend_self_homes);
    s.unload_available = filament_loaded;
    s.unload_is_cold_lane_op = false;
    return s;
}

/**
 * @brief The external (bypass) spool's Load/Unload/Purge buttons as an OpButtonState.
 *
 * Sibling of build_unload_gating_state(), and for the same reason: the field
 * mapping is what must not fork. Two answers are fixed for bypass and are the
 * whole point of stating them once —
 *
 *  - @c slot_has_filament stays unset. Nothing reports whether a spool is
 *    physically on the external holder, so a surface that guessed false would
 *    grey out Load on a spool the user is looking at. Unset keeps Load live and
 *    lets the dispatch's own refusal do the explaining.
 *  - @c unload_is_cold_lane_op is false. A bypass unload pulls filament back out
 *    through a heated toolhead; it is not one of the cold lane ops (#995/#1199)
 *    that stay reachable mid-print.
 *
 * @param bypass_loaded      read_unload_target_loaded() for kExternalSpoolSlot.
 * @param system_busy        AmsSystemInfo::is_busy(); false with no backend.
 * @param lifecycle          The derived PrintState.
 * @param backend_self_homes AmsBackend::filament_ops_self_home(); false with none.
 */
[[nodiscard]] inline OpButtonState build_external_spool_gating_state(bool bypass_loaded,
                                                                     bool system_busy,
                                                                     PrintState lifecycle,
                                                                     bool backend_self_homes) {
    OpButtonState s;
    s.slot_is_loaded = bypass_loaded;
    s.slot_has_filament = std::nullopt;
    s.unload_available = bypass_loaded;
    s.system_busy = system_busy;
    s.print_blocks_op = print_blocks_filament_op(lifecycle, backend_self_homes);
    s.unload_is_cold_lane_op = false;
    return s;
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
 * @brief The nozzle temperature a filament operation heats to: @p material_c,
 *        raised to @p held_c when the nozzle is already set hotter.
 *
 * A nozzle held above the incoming material stays there, so the filament already
 * in the melt zone still flows out ahead of it. AmsOperationSidebar's backend
 * load preheat decides by this with @p held_c as the latched last non-zero target;
 * nozzle_temp_prefill() decides by it with the live target.
 */
[[nodiscard]] inline int filament_op_nozzle_temp(int material_c, int held_c) {
    return std::max(material_c, held_c);
}

/**
 * @brief The material a LOAD should heat for, from the slot being loaded.
 *
 * Resolution order — and the order is the whole point:
 *
 *   1. The slot the load actually targets. An AMS lane the user picked is the
 *      filament about to pass through the melt zone; nothing outranks it.
 *   2. The external / bypass spool, but ONLY when the load has no AMS lane of
 *      its own (@p target_slot == kExternalSpoolSlot, or nothing resolved, or
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
 * @param target_slot       Slot the load targets; kExternalSpoolSlot for bypass,
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
        // A firmware that publishes its own load temperature measured it for
        // the load; the DB midpoint is a print-range derivation standing in
        // for one, so the firmware's number wins where it exists.
        const int temp = (mat.firmware_temps && mat.firmware_temps->load_c)
                             ? *mat.firmware_temps->load_c
                             : load_preheat_temp(mat.material_info);
        return PreheatTarget{temp, mat.display_name};
    };

    // The bypass row IS the external spool — never look at an AMS lane for it.
    if (target_slot == kExternalSpoolSlot) {
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
