// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_op_execute.h"

#include "ui_error_reporting.h"
#include "ui_guided_load_modal.h"
#include "ui_manual_pull_prompt.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_macro_profiles.h"
#include "filament_op_dispatch.h"
#include "filament_op_heater.h"
#include "filament_op_router.h"
#include "i_moonraker_client.h"
#include "lvgl.h"
#include "moonraker_api.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "standard_macros.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>

namespace helix::ui {

// ============================================================================
// Live-state half of the decision
// ============================================================================

OpNozzle resolve_op_nozzle(AmsBackend* backend, int slot, PrinterState& state,
                           const SafetyLimits& limits) {
    std::string extruder;
    if (backend && slot >= 0) {
        extruder =
            ToolState::instance().extruder_name_for_tool(backend->get_slot_info(slot).mapped_tool);
    }
    lv_subject_t* target = extruder.empty()
                               ? nullptr
                               : state.temperature_state().get_extruder_target_subject(extruder);
    if (!target) {
        extruder = state.temperature_state().active_extruder_name();
        target = state.temperature_state().get_active_extruder_target_subject();
    }

    OpNozzle nozzle;
    nozzle.extruder = extruder;
    nozzle.target_c = target ? temperature::deci_to_degrees(lv_subject_get_int(target)) : 0;
    nozzle.floor_c = temperature::extrusion_floor_c(limits, extruder);
    nozzle.ceiling_c = temperature::nozzle_max_temp_c(limits, extruder);
    return nozzle;
}

BackendCaps read_backend_caps(AmsBackend* backend, AmsSystemInfo& info_out, int target_slot) {
    BackendCaps caps;
    if (!backend) {
        return caps;
    }
    info_out = backend->get_system_info();
    caps.present = true;
    caps.requires_slot_selection_for_load = backend->requires_slot_selection_for_load();
    caps.needs_unload_before_load = backend->needs_unload_before_load(info_out, target_slot);
    caps.load_feeds_filament = backend->load_feeds_filament();
    caps.load_swaps_seated_lane = backend->load_swaps_seated_lane();
    // Distinct from !requires_slot_selection_for_load(): plan_load() needs to
    // tell "bypass is suppressing the lane tier" apart from "this backend
    // never wanted a slot", because a named lane wants opposite treatment.
    caps.bypass_active = backend->is_bypass_active();
    // nullopt (cannot tell) and false answer alike: only a confident "filament
    // is at the toolhead that no lane claims" makes the active-head sentinel a
    // resolvable unload target.
    caps.toolhead_unaccounted = backend->toolhead_filament_unaccounted().value_or(false);
    return caps;
}

FilamentHeadView read_filament_head_view(AmsBackend* backend, PrinterState& state,
                                         const AmsSystemInfo* snapshot) {
    FilamentHeadView view;
    if (!backend) {
        return view;
    }
    view.backend_present = true;
    AmsSystemInfo read;
    if (!snapshot) {
        read = backend->get_system_info();
        snapshot = &read;
    }
    const AmsSystemInfo& sys = *snapshot;
    view.max_heads_per_op = backend->max_heads_per_filament_op();
    view.heads_dock = backend->carriage_can_be_empty();
    view.supports_park = backend->supports_tool_park();
    view.system_busy = sys.is_busy();
    view.print_blocks_op = print_blocks_filament_op(state.print_state().get_print_lifecycle(),
                                                    backend->filament_ops_self_home());
    // Which head is on the carriage means something only where heads dock.
    if (view.heads_dock && sys.current_slot >= 0) {
        view.mounted_head = backend->head_for_slot(sys.current_slot).value_or(-1);
    }
    // A machine that cannot tell a fed strand from an inserted one never reads
    // a head as already loaded.
    const bool can_tell_fed = !backend->load_feeds_filament();

    const int count = backend->filament_head_count();
    view.heads.reserve(static_cast<size_t>(std::max(count, 0)));
    for (int head = 0; head < count; ++head) {
        HeadState hs;
        hs.ops = backend->head_filament_ops(head);
        hs.source_slot = backend->head_source_slot(head, sys);
        if (hs.source_slot) {
            hs.source_present = slot_presence(backend->get_slot_info(*hs.source_slot));
            if (can_tell_fed) {
                hs.at_nozzle = backend->can_unload_from_toolhead(*hs.source_slot);
            }
        }
        hs.has_filament = backend->head_has_filament(head);
        // A head with no assigned source still unloads through its own slot,
        // where the slot of the same index feeds it.
        hs.unload_slot = hs.source_slot;
        if (!hs.unload_slot && backend->head_for_slot(head) == head) {
            hs.unload_slot = head;
        }
        view.heads.push_back(std::move(hs));
    }
    return view;
}

int runout_target_slot(AmsBackend* backend, std::optional<int> runout_lane, bool load) {
    if (!backend) {
        return -1;
    }
    const int lane = runout_lane.value_or(-1);
    if (lane < 0) {
        return backend->get_current_slot();
    }
    if (!load) {
        return lane;
    }
    if (const auto head = backend->head_for_slot(lane)) {
        return backend->head_source_slot(*head, backend->get_system_info()).value_or(lane);
    }
    return lane;
}

FilamentOpPlan plan_live_load(const AmsSystemInfo& info, const BackendCaps& caps, int target_slot) {
    const auto& macro_info = StandardMacros::instance().get(StandardMacroSlot::LoadFilament);
    return plan_load(info, caps, target_slot, !macro_info.is_empty(),
                     macro_info.get_source() == MacroSource::CONFIGURED);
}

bool read_unload_target_loaded(AmsBackend* backend, const AmsSystemInfo& info, int target_slot) {
    if (!backend) {
        return false;
    }
    return unload_target_is_loaded(target_slot, backend->slot_is_actively_loaded(target_slot),
                                   backend->slot_has_filament_at_toolhead(target_slot),
                                   info.current_slot == target_slot, info.filament_loaded,
                                   backend->toolhead_filament_unaccounted().value_or(false)) ||
           backend->slot_filament_parked_in_toolhead(target_slot);
}

FilamentOpPlan plan_live_unload(const BackendCaps& caps, int target_slot, bool target_is_loaded) {
    const auto& macro_info = StandardMacros::instance().get(StandardMacroSlot::UnloadFilament);
    return plan_unload(caps, target_slot, target_is_loaded, !macro_info.is_empty(),
                       macro_info.get_source() == MacroSource::CONFIGURED);
}

// ============================================================================
// Preheat
// ============================================================================

PreheatSkip preheat_skip_reason(const FilamentOpPlan& plan, StandardMacroSlot slot,
                                AmsBackend* backend) {
    // The user's claim outranks both detections: they are telling us their own
    // macros heat, or that they want a deliberate cold pull (#978). It holds on
    // every tier, including the raw-gcode fallback that no detection covers.
    if (SafetySettingsManager::instance().get_allow_cold_extrude()) {
        return PreheatSkip::UserOverride;
    }

    switch (plan.tier) {
    case FilamentTier::AmsBackend:
        // supports_auto_heat_on_load() is a claim about LOADS, and ChangeTool
        // counts as one: a seated machine loads by swapping, and the backend
        // heats for that the same way. An unload has its own claim.
        if (backend && (plan.ams_call == AmsCall::Load || plan.ams_call == AmsCall::ChangeTool) &&
            backend->supports_auto_heat_on_load()) {
            return PreheatSkip::BackendSelfHeats;
        }
        if (backend && plan.ams_call == AmsCall::Unload &&
            backend->unload_heats_hotend(plan.ams_arg)) {
            return PreheatSkip::BackendSelfHeats;
        }
        return PreheatSkip::None;

    case FilamentTier::Macro:
        if (filament_macros::macro_heats_hotend(StandardMacros::instance().get(slot).get_macro())) {
            return PreheatSkip::MacroSelfHeats;
        }
        return PreheatSkip::None;

    case FilamentTier::RawGcode:
        // A bare G1 E move needs the hotend above min_extrude_temp or Klipper
        // rejects it outright. Nothing here heats but us.
        return PreheatSkip::None;

    case FilamentTier::Refused:
        return PreheatSkip::None;
    }
    return PreheatSkip::None;
}

const char* preheat_skip_name(PreheatSkip reason) {
    switch (reason) {
    case PreheatSkip::None:
        return "none";
    case PreheatSkip::UserOverride:
        return "allow-cold-load/unload setting";
    case PreheatSkip::BackendSelfHeats:
        return "AMS backend heats on load";
    case PreheatSkip::MacroSelfHeats:
        return "macro heats the hotend itself";
    }
    return "none";
}

// ============================================================================
// Homing
// ============================================================================

bool prerequisite_home_needed(FilamentTier tier, bool toolhead_homed, bool backend_delegates_homing,
                              bool macro_needs_home_first) {
    if (toolhead_homed) {
        return false;
    }

    switch (tier) {
    case FilamentTier::AmsBackend:
        return !backend_delegates_homing;

    case FilamentTier::Macro:
        return macro_needs_home_first;

    case FilamentTier::RawGcode:
    case FilamentTier::Refused:
        return false;
    }
    return false;
}

bool needs_prerequisite_home(const FilamentOpPlan& plan, StandardMacroSlot slot,
                             AmsBackend* backend, bool toolhead_homed) {
    const bool delegates = backend && backend->delegates_homing_to_printer();
    const bool macro_needs_home =
        plan.tier == FilamentTier::Macro &&
        filament_macros::macro_needs_home_first(StandardMacros::instance().get(slot).get_macro());
    return prerequisite_home_needed(plan.tier, toolhead_homed, delegates, macro_needs_home);
}

// ============================================================================
// Surface hooks
// ============================================================================

namespace {

/// Run @p fn through the surface's lifetime wrapper, or directly when it has none.
void guarded(const FilamentOpSurface& surface, helix::SharedCallback<void()> fn) {
    if (surface.guard) {
        surface.guard(std::move(fn));
    } else {
        fn();
    }
}

void begin(const FilamentOpSurface& surface, const FilamentOpPlan& plan) {
    if (surface.on_begin) {
        surface.on_begin(plan);
    }
}

/// What the surface knows for @p op's macro; nothing when it offers no prefill.
std::map<std::string, std::string> known_values(const FilamentOpSurface& surface,
                                                FilamentMacroOp op) {
    return surface.macro_prefill ? surface.macro_prefill(op) : std::map<std::string, std::string>{};
}

/// The op @p ticket names failed: the heater leaves the nozzle hot. Marshalled,
/// because the failure can arrive on a network thread.
void heat_failed(FilamentOpTicket ticket) {
    if (ticket == 0) {
        return;
    }
    helix::ui::queue_update("filament_op_execute::heat_failed", [ticket]() {
        if (auto* heater = get_filament_op_heater()) {
            heater->op_failed(ticket);
        }
    });
}

/// Tier-1 rejection: let the surface unwind, and report unless it did.
void unwind_backend(const FilamentOpSurface& surface, const FilamentOpPlan& plan,
                    const AmsError& err, FilamentOpTicket ticket) {
    heat_failed(ticket);
    bool reported = false;
    if (surface.on_failed) {
        surface.on_failed(plan, err, reported);
    }
    if (!reported) {
        helix::ui::notify_ams_error(err);
    }
}

/// Macro / raw-gcode failure: bookkeeping only, the caller has reported.
/// Marshalled — see FilamentOpSurface::on_async_success.
void unwind_async(const FilamentOpSurface& surface, const FilamentOpPlan& plan,
                  FilamentOpTicket ticket) {
    heat_failed(ticket);
    if (!surface.on_async_failed) {
        return;
    }
    // Marshal first, then guard: token.defer() is main-thread only, so a guard
    // applied on the network thread would itself be the violation.
    helix::ui::queue_update("filament_op_execute::unwind_async", [s = surface, plan]() {
        guarded(s, [s, plan]() { s.on_async_failed(plan); });
    });
}

/// The op completed: the heater cools down what it heated, then the surface's
/// hook runs. Marshalled to the main thread, and the hook guarded.
void finished(const FilamentOpSurface& surface, const helix::SharedCallback<void()>& hook,
              FilamentOpTicket ticket) {
    helix::ui::queue_update("filament_op_execute::finished", [s = surface, hook, ticket]() {
        if (auto* heater = get_filament_op_heater()) {
            heater->op_succeeded(ticket);
        }
        if (hook) {
            guarded(s, hook);
        }
    });
}

/// A tier-1 dispatch completes on the action feed, which the heater watches for
/// its cooldown. Armed before the backend is asked, so a fast op is not missed.
void watch_backend(FilamentOpTicket ticket) {
    if (auto* heater = get_filament_op_heater()) {
        heater->watch_backend_completion(ticket);
    }
}

/// @p surface for a dispatch that runs after a preheat. The surface that asked
/// may be gone by then, and the op outlives it: the dispatch itself runs
/// unguarded, while every hook into the surface goes through its guard and is
/// dropped once the surface is gone. A guard-less surface outlives every
/// dispatch already.
///
/// The macro prefill is read now, at request time, since its answer is needed
/// synchronously. A before_macro hook runs through the guard too, so a surface
/// that set one and is gone sends no macro.
FilamentOpSurface deferred_surface(const FilamentOpSurface& surface) {
    if (!surface.guard) {
        return surface;
    }
    FilamentOpSurface s = surface;
    const auto guard = surface.guard;
    if (surface.macro_prefill) {
        const auto load = surface.macro_prefill(FilamentMacroOp::Load);
        const auto unload = surface.macro_prefill(FilamentMacroOp::Unload);
        s.macro_prefill = [load, unload](FilamentMacroOp op) {
            return op == FilamentMacroOp::Unload ? unload : load;
        };
    }
    if (surface.before_macro) {
        s.before_macro = [guard, hook = surface.before_macro](
                             helix::SharedCallback<void()> send,
                             helix::SharedCallback<void(const MoonrakerError&)> fail) {
            guard([hook, send = std::move(send), fail = std::move(fail)]() { hook(send, fail); });
        };
    }
    if (surface.on_begin) {
        s.on_begin = [guard, hook = surface.on_begin](const FilamentOpPlan& plan) {
            guard([hook, plan]() { hook(plan); });
        };
    }
    if (surface.on_refused) {
        s.on_refused = [guard, hook = surface.on_refused](const FilamentOpPlan& plan) {
            guard([hook, plan]() { hook(plan); });
        };
    }
    if (surface.on_failed) {
        // The guard defers, so the hook cannot claim the report in time; the
        // executor raises the generic toast.
        s.on_failed = [guard, hook = surface.on_failed](const FilamentOpPlan& plan,
                                                        const AmsError& err, bool&) {
            guard([hook, plan, err]() {
                bool ignored = false;
                hook(plan, err, ignored);
            });
        };
    }
    if (surface.on_async_success) {
        s.on_async_success = [guard, hook = surface.on_async_success]() { guard(hook); };
    }
    if (surface.on_async_failed) {
        s.on_async_failed = [guard, hook = surface.on_async_failed](const FilamentOpPlan& plan) {
            guard([hook, plan]() { hook(plan); });
        };
    }
    // Each hook above carries the guard now, so the dispatch around them need not.
    s.guard = [](helix::SharedCallback<void()> fn) { fn(); };
    return s;
}

/// Run @p now through the app's FilamentOpHeater, which calls it at once on a
/// hot nozzle; once the nozzle is hot it calls @p later instead. Either gets the
/// request's ticket. A preheat that ends without running reaches the surface's
/// on_preheat_ended.
void preheat_then(FilamentHeatOp op, PreheatSkip skip, int slot, const FilamentOpSurface& surface,
                  helix::SharedCallback<void(FilamentOpTicket)> now,
                  helix::SharedCallback<void(FilamentOpTicket)> later) {
    FilamentOpHeater* heater = get_filament_op_heater();
    if (!heater) {
        spdlog::warn("{} No FilamentOpHeater — running {} with no preheat", surface.log_tag,
                     filament_heat_op_name(op));
        now(0);
        return;
    }
    PreheatRequest req;
    req.op = op;
    req.log_tag = surface.log_tag;
    req.skip = skip;
    req.slot = slot;
    req.owner = surface.owner ? surface.owner : surface.log_tag;
    if (surface.preheat_fallback_material) {
        req.fallback_material = surface.preheat_fallback_material();
    }
    // The heater runs the op inline or later through one callback; this flag,
    // flipped once run_after_preheat() has returned, tells the two apart.
    auto deferred = std::make_shared<bool>(false);
    req.run = [deferred, now = std::move(now), later = std::move(later)](FilamentOpTicket ticket) {
        if (*deferred) {
            later(ticket);
        } else {
            now(ticket);
        }
    };
    if (surface.on_preheat_ended) {
        req.on_progress = [s = surface](const PreheatProgress& progress) {
            if (progress.phase == PreheatPhase::Cancelled ||
                progress.phase == PreheatPhase::Abandoned) {
                guarded(s, s.on_preheat_ended);
            }
        };
    }
    const auto outcome = heater->run_after_preheat(std::move(req));
    *deferred = true;
    if (outcome == FilamentOpHeater::Outcome::Cancelled && surface.on_preheat_ended) {
        // This request was not run, which a surface waiting on it must hear too.
        guarded(surface, surface.on_preheat_ended);
    }
}

/// The surface a deferred dispatch acts for, decided at dispatch time: one that
/// is gone raises no parameter prompt over whatever screen is up now.
FilamentOpSurface at_dispatch(const FilamentOpSurface& deferred) {
    FilamentOpSurface s = deferred;
    if (s.alive && !s.alive() && s.param_policy == ParamPolicy::Prompt) {
        s.param_policy = ParamPolicy::Unattended;
    }
    return s;
}

/// The error copy a failed macro or fallback raises. A timed-out macro is not a
/// failed one: the printer may still be running it, and telling the user it
/// failed invites them to start a second copy on top. Neither is a dropped
/// socket: the transport vanishing is not the printer's opinion of the macro
/// (prestonbrown/helixscreen#1543).
void report_op_error(const MoonrakerError& error, const char* what) {
    if (error.type == MoonrakerErrorType::TIMEOUT) {
        NOTIFY_WARNING(lv_tr("Macro may still be running — response timed out"));
        return;
    }
    if (error.type == MoonrakerErrorType::CONNECTION_LOST) {
        NOTIFY_WARNING(lv_tr("Connection to printer lost — operation may still be running"));
        return;
    }
    if (std::string(what) == "load") {
        helix::ui::notify_error_tr(TR_NOOP("Failed to load filament: {}"), error);
    } else {
        helix::ui::notify_error_tr(TR_NOOP("Failed to unload: {}"), error);
    }
}

/// Run @p send through the surface's before_macro hook when it has one. A hook
/// failure unwinds and reports the op like a failed macro, and nothing is sent.
/// @p what has static storage duration, like log_tag.
void send_after_before_macro(const FilamentOpSurface& surface, const FilamentOpPlan& plan,
                             const char* what, FilamentOpTicket ticket,
                             helix::SharedCallback<void()> send) {
    if (!surface.before_macro) {
        send();
        return;
    }
    const FilamentOpSurface s = surface;
    surface.before_macro(std::move(send), [s, plan, what, ticket](const MoonrakerError& err) {
        spdlog::error("{} Not sending the {} macro: {}", s.log_tag, what, err.message);
        unwind_async(s, plan, ticket);
        report_op_error(err, what);
    });
}

} // namespace

// ============================================================================
// Load
// ============================================================================

// backend/slot resolution stays with each caller: a dialog whose only target is
// the backend's own active slot does not generalize to every surface.
void execute_filament_load(AmsBackend* backend, int slot, const char* log_tag) {
    FilamentOpSurface surface;
    surface.log_tag = log_tag;
    execute_filament_load(backend, slot, surface);
}

namespace {

/// Send what @p macro_name leaves undone, if anything.
void send_restore_after(const std::string& macro_name) {
    const std::string restore = filament_macros::restore_after(macro_name);
    IMoonrakerAPI* api = get_moonraker_api();
    if (restore.empty() || !api) {
        return;
    }
    spdlog::info("[FilamentOp] After {}: {}", macro_name, restore);
    api->execute_gcode(
        restore, []() {}, [](const MoonrakerError&) {}, /*timeout_ms=*/0, /*silent=*/false,
        /*on_queued=*/nullptr, /*caller_surfaces_errors=*/false);
}

/// Hand the rest of a stock load wizard to the user once its first macro ran.
/// Called from a Moonraker reply thread; the modal opens on the main thread.
void start_guided_load(const std::string& macro_name,
                       const std::map<std::string, std::string>& params) {
    std::optional<filament_macros::GuidedLoad> steps =
        filament_macros::guided_load(macro_name, params);
    if (!steps) {
        return;
    }
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        return;
    }
    auto shared = std::make_shared<const filament_macros::GuidedLoad>(std::move(*steps));
    const nlohmann::json query = filament_macros::guided_load_status_query(*shared);
    const auto open = [shared](bool waits) {
        queue_update("GuidedLoad::open", [shared, waits]() {
            spdlog::info("[GuidedLoad] {} the hand push", waits ? "Waiting for" : "No wait for");
            GuidedLoadModal::show_owned(*shared, waits);
        });
    };
    api->get_client().send_jsonrpc(
        "printer.objects.query", query,
        [shared, open](const nlohmann::json& resp) {
            open(filament_macros::guided_load_waits(resp, *shared));
        },
        // Unanswered, open on the question: every form of the wizard asks it.
        [open](const MoonrakerError& err) {
            spdlog::warn("[GuidedLoad] Wait state not read: {}", err.message);
            open(false);
        });
}

void dispatch_load(AmsBackend* backend, int slot, const FilamentOpSurface& surface,
                   const FilamentOpPlan& plan, FilamentOpTicket ticket) {
    const char* log_tag = surface.log_tag;
    const auto& load_info = StandardMacros::instance().get(StandardMacroSlot::LoadFilament);

    switch (plan.tier) {
    case helix::ui::FilamentTier::AmsBackend: {
        spdlog::info("{} Load via AMS backend (slot {})", log_tag, slot);
        begin(surface, plan);
        watch_backend(ticket);
        AmsError err = (plan.ams_call == helix::ui::AmsCall::ChangeTool)
                           ? backend->change_tool(plan.ams_arg)
                           : backend->load_filament(plan.ams_arg);
        if (!err.success()) {
            spdlog::error("{} Load filament failed: {}", log_tag, err.technical_msg);
            unwind_backend(surface, plan, err, ticket);
            return;
        }
        // Success is NOT reported here: a backend load is fire-and-forget and
        // completes on the action feed, which is why on_async_* skips tier 1.
        return;
    }

    case helix::ui::FilamentTier::Refused:
        // Nothing is armed on a refusal — that is what keeps a refused op from
        // leaving a surface stuck in a phantom "busy" (bundle 9KRXZ62P).
        if (surface.on_refused) {
            surface.on_refused(plan);
            return;
        }
        // SelectSlot: no lane resolved, and a surface with no picker cannot
        // offer one. Never navigate from here: tearing a dialog out from under
        // the user is what on_refused exists to let a surface decide.
        spdlog::info("{} Load refused — no slot resolved", log_tag);
        NOTIFY_WARNING(lv_tr("Select a filament slot to load"));
        return;

    case helix::ui::FilamentTier::Macro: {
        auto* api = get_moonraker_api();
        if (!api) {
            spdlog::warn("{} No API — cannot run the filament macro", log_tag);
            NOTIFY_WARNING(lv_tr("Printer connection unavailable"));
            return;
        }
        const std::string macro_name = load_info.get_macro();
        spdlog::info("{} Using StandardMacros load: {}", log_tag, macro_name);
        const FilamentOpSurface s = surface;
        helix::ui::dispatch_filament_macro(
            macro_name, surface.param_policy,
            [api, s, plan, ticket, macro_name](const helix::MacroParamResult& result) {
                // Under ParamPolicy::Prompt this lands whenever the user presses
                // Run, which can be after the asking surface is gone.
                guarded(s, [api, s, plan, ticket, macro_name, params = result.params]() {
                    begin(s, plan);
                    send_after_before_macro(
                        s, plan, "load", ticket, [api, s, plan, ticket, macro_name, params]() {
                            // execute_macro()'s reply lands when the script has RUN, so
                            // these are completion callbacks, not "started" ones.
                            const bool dispatched = StandardMacros::instance().execute(
                                StandardMacroSlot::LoadFilament, api, params,
                                [s, ticket, macro_name, params]() {
                                    spdlog::info("{} Load filament finished", s.log_tag);
                                    finished(s, s.on_async_success, ticket);
                                    start_guided_load(macro_name, params);
                                },
                                [s, plan, ticket](const MoonrakerError& err) {
                                    spdlog::error("{} Failed to load filament: {}", s.log_tag,
                                                  err.message);
                                    unwind_async(s, plan, ticket);
                                    report_op_error(err, "load");
                                },
                                IMoonrakerAPI::kExtrusionTimeoutMs);
                            if (!dispatched) {
                                // Empty slot or no API: neither callback will ever fire,
                                // so nothing else would release what begin() armed.
                                spdlog::warn("{} Load macro did not dispatch", s.log_tag);
                                unwind_async(s, plan, ticket);
                            }
                        });
                });
            },
            known_values(surface, FilamentMacroOp::Load));
        return;
    }

    case helix::ui::FilamentTier::RawGcode: {
        auto* api = get_moonraker_api();
        if (!api) {
            spdlog::warn("{} No API — cannot send the filament fallback", log_tag);
            NOTIFY_WARNING(lv_tr("Printer connection unavailable"));
            return;
        }
        spdlog::info("{} No backend and no load macro — raw gcode fallback", log_tag);
        begin(surface, plan);
        const FilamentOpSurface s = surface;
        api->execute_gcode(
            helix::ui::filament_load_fallback_gcode(),
            [s, ticket]() {
                spdlog::info("{} Load fallback gcode sent", s.log_tag);
                finished(s, s.on_async_success, ticket);
            },
            [s, plan, ticket](const MoonrakerError& err) {
                spdlog::error("{} Load fallback failed: {}", s.log_tag, err.message);
                unwind_async(s, plan, ticket);
                report_op_error(err, "load");
            },
            IMoonrakerAPI::kExtrusionTimeoutMs, /*silent=*/false, /*on_queued=*/nullptr,
            /*caller_surfaces_errors=*/true); // report_op_error() toasts the failure
        return;
    }
    }
}

} // namespace

void execute_filament_load(AmsBackend* backend, int slot, const FilamentOpSurface& surface) {
    // Where this head's load is the user's job, saying so is the whole op.
    if (helix::ui::notify_if_filament_op_unavailable(backend, slot, /*load=*/true)) {
        return;
    }
    AmsSystemInfo sys;
    const BackendCaps caps = read_backend_caps(backend, sys, slot);
    const FilamentOpPlan plan = plan_live_load(sys, caps, slot);
    if (plan.tier == FilamentTier::Refused) {
        // Nothing heats for an op that will not run.
        dispatch_load(backend, slot, surface, plan, 0);
        return;
    }
    const PreheatSkip skip = preheat_skip_reason(plan, StandardMacroSlot::LoadFilament, backend);
    preheat_then(
        FilamentHeatOp::Load, skip, slot, surface,
        [backend, slot, surface, plan](FilamentOpTicket ticket) {
            dispatch_load(backend, slot, surface, plan, ticket);
        },
        [slot, s = deferred_surface(surface)](FilamentOpTicket ticket) {
            AmsBackend* live = AmsState::instance().get_backend();
            AmsSystemInfo live_sys;
            const BackendCaps live_caps = read_backend_caps(live, live_sys, slot);
            dispatch_load(live, slot, at_dispatch(s), plan_live_load(live_sys, live_caps, slot),
                          ticket);
        });
}

// ============================================================================
// Unload
// ============================================================================

// target_is_loaded is a caller-supplied input rather than recomputed here — see
// unload_target_is_loaded()'s doc comment for why callers must not each answer
// that question inline.
void execute_filament_unload(AmsBackend* backend, int slot, bool target_is_loaded,
                             const char* log_tag) {
    FilamentOpSurface surface;
    surface.log_tag = log_tag;
    execute_filament_unload(backend, slot, target_is_loaded, surface);
}

namespace {

/// plan_unload() against @p backend. It reads only `present` and
/// `toolhead_unaccounted`, so the full read_backend_caps() call (whose
/// needs_unload_before_load() is a per-lane backend query) buys nothing here.
FilamentOpPlan plan_unload_on(AmsBackend* backend, int slot, bool target_is_loaded) {
    BackendCaps caps;
    caps.present = backend != nullptr;
    caps.toolhead_unaccounted = backend && backend->toolhead_filament_unaccounted().value_or(false);
    return plan_live_unload(caps, slot, target_is_loaded);
}

void dispatch_unload(AmsBackend* backend, int slot, const FilamentOpSurface& surface,
                     const FilamentOpPlan& plan, FilamentOpTicket ticket) {
    const char* log_tag = surface.log_tag;
    const auto& unload_info = StandardMacros::instance().get(StandardMacroSlot::UnloadFilament);

    if (plan.tier != FilamentTier::Refused) {
        // Filament is being pulled, so nothing is left to purge: the next load
        // computes its hold temperature fresh instead of inheriting this one's.
        // After any preheat, which heats to that latch.
        get_printer_state().temperature_state().clear_load_latch();
    }

    switch (plan.tier) {
    case helix::ui::FilamentTier::AmsBackend: {
        // Pass plan.ams_arg, not -1: the caller knows which slot it targeted and
        // says so, rather than letting the backend re-resolve current_slot (the
        // U1 wrong-tool unload bug).
        begin(surface, plan);
        watch_backend(ticket);
        AmsError err = backend->unload_filament(plan.ams_arg);
        if (!err.success()) {
            spdlog::error("{} Unload filament failed: {}", log_tag, err.technical_msg);
            unwind_backend(surface, plan, err, ticket);
            return;
        }
        return;
    }

    case helix::ui::FilamentTier::Refused:
        if (surface.on_refused) {
            surface.on_refused(plan);
            return;
        }
        // NothingLoaded is plan_unload's only refusal.
        spdlog::info("{} Unload refused — nothing loaded (slot={})", log_tag, slot);
        NOTIFY_WARNING(lv_tr("No filament loaded to unload"));
        return;

    case helix::ui::FilamentTier::Macro: {
        auto* api = get_moonraker_api();
        if (!api) {
            spdlog::warn("{} No API — cannot run the filament macro", log_tag);
            NOTIFY_WARNING(lv_tr("Printer connection unavailable"));
            return;
        }
        const std::string macro_name = unload_info.get_macro();
        spdlog::info("{} Using StandardMacros unload: {}", log_tag, macro_name);
        const FilamentOpSurface s = surface;
        helix::ui::dispatch_filament_macro(
            macro_name, surface.param_policy,
            [api, s, plan, ticket, macro_name](const helix::MacroParamResult& result) {
                guarded(s, [api, s, plan, ticket, macro_name, params = result.params]() {
                    begin(s, plan);
                    send_after_before_macro(
                        s, plan, "unload", ticket, [api, s, plan, ticket, macro_name, params]() {
                            const bool dispatched = StandardMacros::instance().execute(
                                StandardMacroSlot::UnloadFilament, api, params,
                                [s, ticket, macro_name]() {
                                    spdlog::info("{} Unload filament finished", s.log_tag);
                                    finished(s, s.on_async_success, ticket);
                                    send_restore_after(macro_name);
                                },
                                [s, plan, ticket](const MoonrakerError& err) {
                                    spdlog::error("{} Failed to unload filament: {}", s.log_tag,
                                                  err.message);
                                    unwind_async(s, plan, ticket);
                                    report_op_error(err, "unload");
                                },
                                IMoonrakerAPI::kExtrusionTimeoutMs);
                            if (!dispatched) {
                                spdlog::warn("{} Unload macro did not dispatch", s.log_tag);
                                unwind_async(s, plan, ticket);
                            }
                        });
                });
            },
            known_values(surface, FilamentMacroOp::Unload));
        return;
    }

    case helix::ui::FilamentTier::RawGcode: {
        auto* api = get_moonraker_api();
        if (!api) {
            spdlog::warn("{} No API — cannot send the filament fallback", log_tag);
            NOTIFY_WARNING(lv_tr("Printer connection unavailable"));
            return;
        }
        spdlog::info("{} No backend and no unload macro — raw gcode fallback", log_tag);
        begin(surface, plan);
        const FilamentOpSurface s = surface;
        api->execute_gcode(
            helix::ui::filament_unload_fallback_gcode(),
            [s, ticket]() {
                spdlog::info("{} Unload fallback gcode sent", s.log_tag);
                finished(s, s.on_async_success, ticket);
            },
            [s, plan, ticket](const MoonrakerError& err) {
                spdlog::error("{} Unload fallback failed: {}", s.log_tag, err.message);
                unwind_async(s, plan, ticket);
                report_op_error(err, "unload");
            },
            IMoonrakerAPI::kExtrusionTimeoutMs, /*silent=*/false, /*on_queued=*/nullptr,
            /*caller_surfaces_errors=*/true); // report_op_error() toasts the failure
        return;
    }
    }
}

} // namespace

void execute_filament_unload(AmsBackend* backend, int slot, bool target_is_loaded,
                             const FilamentOpSurface& surface) {
    // Every caller here means "take the filament out". Where that is manual,
    // saying so is the whole operation: no plan, no heat, nothing sent.
    if (helix::ui::notify_if_filament_op_unavailable(backend, slot, /*load=*/false)) {
        return;
    }
    const FilamentOpPlan plan = plan_unload_on(backend, slot, target_is_loaded);
    if (plan.tier == FilamentTier::Refused) {
        dispatch_unload(backend, slot, surface, plan, 0);
        return;
    }
    const PreheatSkip skip = preheat_skip_reason(plan, StandardMacroSlot::UnloadFilament, backend);
    preheat_then(
        FilamentHeatOp::Unload, skip, slot, surface,
        [backend, slot, surface, plan](FilamentOpTicket ticket) {
            dispatch_unload(backend, slot, surface, plan, ticket);
        },
        [slot, target_is_loaded, s = deferred_surface(surface)](FilamentOpTicket ticket) {
            AmsBackend* live = AmsState::instance().get_backend();
            dispatch_unload(live, slot, at_dispatch(s),
                            plan_unload_on(live, slot, target_is_loaded), ticket);
        });
}

// ============================================================================
// Purge
// ============================================================================

namespace {

void dispatch_purge(const char* log_tag, FilamentOpTicket ticket) {
    auto* api = get_moonraker_api();
    if (!api) {
        heat_failed(ticket);
        return;
    }

    // Two tiers, not three: no AmsBackend exposes a purge entry point, so there
    // is no plan_purge() to route through. The macro tier and the fallback are
    // the whole ladder here.
    const auto& purge_info = StandardMacros::instance().get(StandardMacroSlot::Purge);
    if (!purge_info.is_empty()) {
        const std::string macro_name = purge_info.get_macro();
        spdlog::info("{} Using StandardMacros purge: {}", log_tag, macro_name);
        helix::ui::dispatch_filament_macro(
            macro_name, helix::ui::ParamPolicy::Suppress,
            [api, log_tag, ticket](const helix::MacroParamResult& result) {
                const bool dispatched = StandardMacros::instance().execute(
                    StandardMacroSlot::Purge, api, result.params,
                    [log_tag, ticket]() {
                        spdlog::info("{} Purge finished", log_tag);
                        finished(FilamentOpSurface{}, nullptr, ticket);
                    },
                    [log_tag, ticket](const MoonrakerError& err) {
                        heat_failed(ticket);
                        spdlog::error("{} Failed to purge: {}", log_tag, err.message);
                        helix::ui::notify_error_tr(TR_NOOP("Failed to purge: {}"), err);
                    });
                if (!dispatched) {
                    // Neither callback will fire.
                    heat_failed(ticket);
                }
            });
        return;
    }

    spdlog::info("{} No purge macro configured — raw gcode fallback", log_tag);
    api->execute_gcode(
        helix::ui::filament_purge_fallback_gcode(),
        [log_tag, ticket]() {
            spdlog::info("{} Purge fallback gcode sent", log_tag);
            finished(FilamentOpSurface{}, nullptr, ticket);
        },
        [log_tag, ticket](const MoonrakerError& err) {
            heat_failed(ticket);
            spdlog::error("{} Purge fallback failed: {}", log_tag, err.message);
            helix::ui::notify_error_tr(TR_NOOP("Failed to purge: {}"), err);
        },
        IMoonrakerAPI::kExtrusionTimeoutMs);
}

} // namespace

void execute_filament_purge(const char* log_tag) {
    if (!get_moonraker_api()) {
        return;
    }
    // A purge pushes whatever is already in the melt zone: the loaded lane names it.
    AmsBackend* backend = AmsState::instance().get_backend();
    FilamentOpSurface surface;
    surface.log_tag = log_tag;
    preheat_then(
        FilamentHeatOp::Purge, preheat_skip_reason_unplanned(),
        backend ? backend->get_current_slot() : -1, surface,
        [log_tag](FilamentOpTicket ticket) { dispatch_purge(log_tag, ticket); },
        [log_tag](FilamentOpTicket ticket) { dispatch_purge(log_tag, ticket); });
}

} // namespace helix::ui
