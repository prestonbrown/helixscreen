// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file filament_op_execute.h
 * @brief Shared EXECUTION of a Load/Unload/Purge, once filament_op_dispatch.h
 *        has already decided which tier to take.
 *
 * filament_op_dispatch.h (plan_load(), plan_unload()) centralizes the DECISION
 * of which tier a filament operation takes. This file runs that tier: building
 * BackendCaps, switching over FilamentTier, dispatching the configured macro or
 * falling back to raw gcode. Every entry point heats the nozzle first through
 * FilamentOpHeater (filament_op_heater.h) when the op needs it, so a surface on
 * this ladder cannot dispatch onto a cold nozzle.
 */

#pragma once

#include "filament_action_set.h"
#include "filament_op_dispatch.h"
#include "filament_op_router.h"
#include "filament_op_slot_resolver.h"
#include "helix_shared_callback.h"
#include "moonraker_types.h"
#include "standard_macros.h"

#include <functional>
#include <map>
#include <optional>
#include <string>

namespace helix {
class AmsBackend;
struct AmsError;
class IMoonrakerAPI;
class PrinterState;
} // namespace helix

namespace helix::ui {

// ============================================================================
// Live-state half of the decision
// ============================================================================
//
// filament_op_dispatch.h states the tier rules over plain values, so they stay
// testable with no printer and no display. Getting from a live AmsBackend and
// the StandardMacros registry TO those values is mechanical, and identical for
// every surface — which is what makes a hand-written copy of it at each call
// site a place for the surfaces to drift apart. Written once, here.

/**
 * @brief Read the planner's backend answers off a live AmsBackend.
 *
 * @param backend     May be null; every field then keeps its "no backend" default.
 * @param info_out    Filled with get_system_info() when a backend exists, left
 *                    untouched otherwise. plan_load() reads it, and callers need
 *                    it afterwards to resolve the slot the op is about.
 * @param target_slot The lane the plan targets. needs_unload_before_load() is a
 *                    per-lane question, so it must be the SAME slot.
 */
[[nodiscard]] BackendCaps read_backend_caps(AmsBackend* backend, AmsSystemInfo& info_out,
                                            int target_slot);

/**
 * @brief Read the filament action decision's inputs off a live AmsBackend.
 *
 * selected_head stays -1: a surface with a selection sets it itself.
 *
 * @param backend  May be null; the view then reports no backend.
 * @param snapshot A get_system_info() reading of @p backend already in hand
 *                 (AmsState's shared one); null reads the backend.
 */
[[nodiscard]] FilamentHeadView read_filament_head_view(AmsBackend* backend, PrinterState& state,
                                                       const AmsSystemInfo* snapshot = nullptr);

/**
 * @brief print_blocks_filament_load() over a live backend and printer.
 *
 * The backend's two homing answers, whether its load begins by unloading the
 * toolhead, and the live homed_axes, so every Load
 * surface asks the question AmsSubscriptionBackend::refuse_load_if_printing()
 * answers. Null @p backend: no firmware macro can home, as for
 * print_blocks_filament_op().
 *
 * @warning Main thread only: reads the homed_axes subject.
 */
[[nodiscard]] bool print_blocks_load_now(const AmsBackend* backend, PrinterState& state);

/**
 * @brief The slot a runout's Load or Unload names.
 *
 * The runout's own lane decides the head. An Unload takes that lane, which is
 * what sits in the head; a Load takes the head's Assigned Tool slot, falling
 * back to the lane. A runout that names no lane uses the active slot.
 *
 * @param backend      May be null; the answer is then -1.
 * @param runout_lane  FilamentSensorManager::real_runout_lane(): nullopt or -1
 *                     when the runout names no lane.
 */
[[nodiscard]] int runout_target_slot(AmsBackend* backend, std::optional<int> runout_lane,
                                     bool load);

/// The extruder a filament op heats, and the numbers a nozzle prefill holds to.
struct OpNozzle {
    std::string extruder; ///< Klipper extruder name
    int target_c = 0;     ///< Its live target, whole degrees
    int floor_c = 0;      ///< temperature::extrusion_floor_c() for it
    int ceiling_c = 0;    ///< temperature::nozzle_max_temp_c() for it
};

/**
 * @brief Resolve the extruder an op on @p slot heats, with its target and limits.
 *
 * The slot's SlotInfo::mapped_tool names a tool, and that tool's extruder is the
 * one. The active extruder stands in when there is no backend or slot, the slot
 * maps to no tool, the tool names no extruder, or the printer state does not know
 * that extruder.
 *
 * @param backend May be null.
 * @param slot    The lane the op acts on; negative for none.
 */
[[nodiscard]] OpNozzle resolve_op_nozzle(AmsBackend* backend, int slot, PrinterState& state,
                                         const SafetyLimits& limits);

/// plan_load() with the StandardMacros LoadFilament slot read off the registry.
[[nodiscard]] FilamentOpPlan plan_live_load(const AmsSystemInfo& info, const BackendCaps& caps,
                                            int target_slot);

/// plan_unload() with the StandardMacros UnloadFilament slot read off the registry.
/// @param target_is_loaded From read_unload_target_loaded() — never answered inline.
[[nodiscard]] FilamentOpPlan plan_live_unload(const BackendCaps& caps, int target_slot,
                                              bool target_is_loaded);

/// unload_target_is_loaded() with the per-lane answers read off a live
/// backend. False when @p backend is null: with no backend there is no lane to
/// ask about, and plan_unload() gates its tier 1 on the backend anyway.
[[nodiscard]] bool read_unload_target_loaded(AmsBackend* backend, const AmsSystemInfo& info,
                                             int target_slot);

// ============================================================================
// Preheat
// ============================================================================

/**
 * @brief Why heating the hotend ourselves before a filament op would be redundant.
 *
 * Three independent reasons, and a surface that knows only one of them imposes a
 * preheat the other two would have skipped. Ask preheat_skip_reason() rather
 * than any single term.
 */
enum class PreheatSkip {
    None,             ///< Preheat as usual
    UserOverride,     ///< "Allow cold load/unload" — the user's macros do their own heating
    BackendSelfHeats, ///< AmsBackend::supports_auto_heat_on_load() / _on_unload()
    MacroSelfHeats,   ///< helix::filament_macros::macro_heats_hotend()
};

/**
 * @brief Should this surface skip its own preheat before dispatching @p plan?
 *
 * Reads SafetySettingsManager and StandardMacros, so it answers about the state
 * the op is about to run against.
 *
 * The tier matters: a macro that heats itself is only a reason to skip when the
 * macro is what will actually run, and the same for the backend. Pass the plan
 * the caller is about to dispatch, not one from before a slot changed.
 *
 * @param plan    The plan about to be dispatched.
 * @param slot    Which StandardMacros slot @p plan resolves against.
 * @param backend May be null.
 */
[[nodiscard]] PreheatSkip preheat_skip_reason(const FilamentOpPlan& plan, StandardMacroSlot slot,
                                              AmsBackend* backend);

/// Short name for @p reason, for log lines. Never null.
[[nodiscard]] const char* preheat_skip_name(PreheatSkip reason);

// ============================================================================
// Homing
// ============================================================================

/**
 * @brief The homing decision, from facts already gathered.
 *
 * Homes only when something known needs it: on tier 1 a backend that does not
 * delegate homing to the printer, on tier 2 a macro profile that needs a home
 * first. An unrecognized macro runs as it is, unhomed (#1819). The raw-gcode
 * tier moves E only, which Klipper runs unhomed.
 *
 * Each claim is read against its own TIER. A backend's claim is about the gcode
 * that backend emits, so it says nothing once bypass has dropped the op to the
 * user's macro; a macro's claim is about that macro run alone, so it says
 * nothing about a backend that merely composes it with moves of its own.
 *
 * @param tier                     The tier the op dispatches on.
 * @param toolhead_homed           Already homed needs nothing.
 * @param backend_delegates_homing AmsBackend::delegates_homing_to_printer().
 * @param macro_needs_home_first   filament_macros::macro_needs_home_first().
 */
[[nodiscard]] bool prerequisite_home_needed(FilamentTier tier, bool toolhead_homed,
                                            bool backend_delegates_homing,
                                            bool macro_needs_home_first);

/**
 * @brief Must the toolhead be homed before this op is dispatched?
 *
 * A surface that gets a yes homes first, with no confirmation: asking for the op
 * is consent to the home it requires. Same shape as preheat_skip_reason(), for
 * the other thing a surface would otherwise do redundantly. Gathers the facts
 * for prerequisite_home_needed(), which holds the rule.
 *
 * @param plan           The plan about to be dispatched.
 * @param slot           Which StandardMacros slot @p plan resolves against.
 * @param backend        May be null.
 * @param toolhead_homed helix::toolhead_is_homed() — already homed needs nothing.
 */
[[nodiscard]] bool needs_prerequisite_home(const FilamentOpPlan& plan, StandardMacroSlot slot,
                                           AmsBackend* backend, bool toolhead_homed);

// ============================================================================
// The surface half of a dispatch
// ============================================================================

/**
 * @brief What one surface does around a filament op, so every surface can share
 *        the ladder that decides and runs it.
 *
 * The tier decision, the backend entry point, the macro tier and the raw-gcode
 * fallback are the same everywhere and belong to the executor. What genuinely
 * differs is bookkeeping the surface shows the user: a guard, an on-button
 * spinner, a stepper. Those hang here rather than justifying a second ladder.
 *
 * Every callback is optional. The defaults describe a surface that outlives any
 * dispatch it starts and shows nothing while one runs.
 */
struct FilamentOpSurface {
    /// Prefixes the executor's log lines. Static storage duration, per the note
    /// below — the executor captures it in callbacks that outlive the call.
    const char* log_tag = "[Filament]";

    /// Macro-tier parameter policy. A surface the user tapped a button on can
    /// afford to raise the parameter modal; one layered under a live dialog
    /// must not put a second modal on top of it.
    ParamPolicy param_policy = ParamPolicy::Suppress;

    /// Arm what this surface shows while the op runs. Called once, immediately
    /// before the chosen tier runs — never on a refusal, which is what keeps a
    /// refused op from leaving a surface stuck in a phantom "busy".
    helix::SharedCallback<void(const FilamentOpPlan&)> on_begin;

    /// Unwind what on_begin armed, after a tier-1 dispatch the backend rejected
    /// outright. Owning the report is the surface's option: an unset hook leaves
    /// the executor to raise the generic AMS-error toast, while a hook that sets
    /// `reported` true has raised its own and suppresses that.
    helix::SharedCallback<void(const FilamentOpPlan&, const AmsError&, bool& reported)> on_failed;

    /// Unwind after a macro or raw-gcode dispatch failed. Separate from
    /// on_failed because there is no AmsError there — the executor has already
    /// reported through report_op_error(), and this is bookkeeping only.
    helix::SharedCallback<void(const FilamentOpPlan&)> on_async_failed;

    /// Present a refusal. Unset raises the shared toasts.
    helix::SharedCallback<void(const FilamentOpPlan&)> on_refused;

    /// The op finished, on the tiers that have a completion signal (macro and
    /// raw gcode). Tier 1 completion arrives through the backend's action feed
    /// instead, so this does not fire there.
    ///
    /// Marshalled to the main thread by the executor, along with on_async_failed:
    /// both are reached from a Moonraker reply, which lands on a network thread,
    /// and a hook that touched a subject or a widget there would corrupt LVGL.
    /// on_begin, on_failed and on_refused are NOT marshalled — those run inline
    /// on whichever thread called the executor, which is the UI thread for every
    /// surface that has a button.
    helix::SharedCallback<void()> on_async_success;

    /// Wrap a callback that may outlive this surface. Unset runs it directly,
    /// which is correct ONLY for a surface that outlives every dispatch it
    /// starts. A surface torn down with a panel must supply the token.defer()
    /// wrapper here, or a macro-parameter modal answered after the teardown
    /// reaches freed memory.
    helix::SharedCallback<void(helix::SharedCallback<void()>)> guard;

    /// Parameter values this surface already knows for the macro @p op runs, keyed
    /// by parameter name (nozzle_temp_prefill() in filament_op_router.h). Called
    /// once, on the dispatching thread, just before the macro tier asks
    /// dispatch_filament_macro(), which sends only the names the macro reads and
    /// skips the prompt when they fill every one. Unset offers nothing, and under
    /// ParamPolicy::Suppress the values are not used.
    helix::SharedCallback<std::map<std::string, std::string>(FilamentMacroOp op)> macro_prefill;

    /// Runs just before the macro tier sends its macro, once any parameter prompt
    /// has been answered: call `send` to go ahead, or `fail` to stop with that
    /// error, which unwinds and reports the op the way a failed macro does and
    /// sends nothing. Unset sends straight away.
    helix::SharedCallback<void(helix::SharedCallback<void()> send,
                               helix::SharedCallback<void(const MoonrakerError&)> fail)>
        before_macro;

    /// The material to heat for when neither the target lane nor the external
    /// spool names one (a material preset the surface offers). Asked once, when
    /// the op is requested. Unset heats to kDefaultLoadPreheatTemp there.
    helix::SharedCallback<std::optional<PreheatTarget>()> preheat_fallback_material;

    /// The preheat this op waited on ended without running it: the user
    /// withdrew it, another op replaced it, or it could not complete. Unset is
    /// right for a surface whose user saw the heater's toast and has nothing
    /// waiting on the op; a surface with a continuation (a flow that moves on
    /// once the op finishes) must set it, or that flow stalls silently. Runs on
    /// the main thread, through the guard.
    helix::SharedCallback<void()> on_preheat_ended;

    /// Names this surface to the preheat: the same op on the same slot from the
    /// same owner is a re-tap. Unset uses log_tag, which every caller passes as a
    /// literal naming its surface.
    const void* owner = nullptr;

    /// Whether the surface is still there for the user, for a dispatch that runs
    /// after a preheat. One that is not raises no parameter prompt: the dispatch
    /// downgrades ParamPolicy::Prompt to Unattended, which sends the prefill read
    /// at request time. Unset counts as alive.
    helix::SharedCallback<bool()> alive;
};

/// @note **`log_tag` must have static storage duration.** All three functions
/// capture the raw pointer in lambdas that outlive the call — the macro-tier
/// and raw-gcode paths hand their success/error callbacks to Moonraker and
/// return immediately. A string literal (what every caller passes: a bracketed
/// class name) satisfies this; a `std::string::c_str()` or a stack buffer would
/// dangle by the time the reply lands. Deliberately `const char*` rather than
/// `std::string` so that constraint is visible instead of being paid for on
/// every dispatch.

/// Execute a load for `slot` on `backend` (which may be null), resolving the
/// tier through plan_load() and running it. `log_tag` prefixes the spdlog lines
/// so a reader can still tell which surface asked.
///
/// Unless the plan is refused, the op goes through FilamentOpHeater first: it
/// runs now on a hot nozzle, or once the nozzle is hot. A deferred op re-plans
/// against AmsState's backend at that moment, since the backend or its lanes
/// can change while the nozzle heats. A request while a preheat is pending
/// cancels that preheat instead.
void execute_filament_load(AmsBackend* backend, int slot, const char* log_tag);

/// Load, with this surface's bookkeeping hung off the shared ladder.
void execute_filament_load(AmsBackend* backend, int slot, const FilamentOpSurface& surface);

/// Unload counterpart. `target_is_loaded` comes from unload_target_is_loaded()
/// in filament_op_dispatch.h - do not answer that question inline, that
/// divergence is what the helper exists to prevent.
void execute_filament_unload(AmsBackend* backend, int slot, bool target_is_loaded,
                             const char* log_tag);

/// Unload counterpart of the surface-taking load overload.
void execute_filament_unload(AmsBackend* backend, int slot, bool target_is_loaded,
                             const FilamentOpSurface& surface);

/// Purge counterpart. Two tiers only (the configured macro, then
/// filament_purge_fallback_gcode() from filament_op_router.h) — no AmsBackend
/// exposes a purge entry point, so there is no plan_purge() to route through.
/// Heats for the loaded lane's material first, like the load and unload, and
/// sends the backend's purge preamble ahead of the purge itself.
void execute_filament_purge(const char* log_tag);

/// Run @p purge once the active filament backend's purge preamble
/// (AmsBackend::purge_preamble_gcode(), asked with whether a job holds the
/// machine right now) has completed. Every Purge surface sends its macro or raw
/// fallback through this, so the preamble cannot be skipped by one of them.
///
/// No backend or an empty preamble: @p purge runs synchronously, nothing is
/// sent. Otherwise the preamble goes out first and @p purge runs on the main
/// thread once it succeeds; if it fails, @p on_error runs on the main thread
/// instead and @p purge never does.
void run_after_purge_preamble(helix::IMoonrakerAPI* api, const char* log_tag,
                              std::function<void()> purge,
                              std::function<void(const MoonrakerError&)> on_error);

} // namespace helix::ui
