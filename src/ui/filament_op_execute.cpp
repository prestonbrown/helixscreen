// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_op_execute.h"

#include "ui_error_reporting.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"

#include "ams_backend.h"
#include "app_globals.h"
#include "filament_macro_profiles.h"
#include "filament_op_dispatch.h"
#include "filament_op_router.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "standard_macros.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <map>
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
    lv_subject_t* target = extruder.empty() ? nullptr : state.get_extruder_target_subject(extruder);
    if (!target) {
        extruder = state.active_extruder_name();
        target = state.get_active_extruder_target_subject();
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
    caps.is_tool_changer = backend->get_type() == AmsType::TOOL_CHANGER;
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
        // supports_auto_heat_on_load() is a claim about LOADS only. ChangeTool
        // counts as one — a seated machine loads by swapping, and the backend
        // heats for that the same way — but AmsCall::Unload does not, and no
        // backend claims to heat for it.
        if (backend && (plan.ams_call == AmsCall::Load || plan.ams_call == AmsCall::ChangeTool) &&
            backend->supports_auto_heat_on_load()) {
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

bool needs_prerequisite_home(const FilamentOpPlan& plan, StandardMacroSlot slot,
                             AmsBackend* backend, bool toolhead_homed) {
    if (toolhead_homed) {
        return false;
    }

    switch (plan.tier) {
    case FilamentTier::AmsBackend:
        return !(backend && backend->delegates_homing_to_printer());

    case FilamentTier::Macro:
        return !filament_macros::macro_homes_if_needed(
            StandardMacros::instance().get(slot).get_macro());

    case FilamentTier::RawGcode:
        // The fallback extrudes and retracts E only, which Klipper runs unhomed.
        return false;

    case FilamentTier::Refused:
        return false;
    }
    return true;
}

// ============================================================================
// Surface hooks
// ============================================================================

namespace {

/// Run @p fn through the surface's lifetime wrapper, or directly when it has none.
void guarded(const FilamentOpSurface& surface, std::function<void()> fn) {
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

/// Tier-1 rejection: let the surface unwind, and report unless it did.
void unwind_backend(const FilamentOpSurface& surface, const FilamentOpPlan& plan,
                    const AmsError& err) {
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
void unwind_async(const FilamentOpSurface& surface, const FilamentOpPlan& plan) {
    if (!surface.on_async_failed) {
        return;
    }
    // Marshal first, then guard: token.defer() is main-thread only, so a guard
    // applied on the network thread would itself be the violation.
    helix::ui::queue_update("filament_op_execute::unwind_async", [s = surface, plan]() {
        guarded(s, [s, plan]() { s.on_async_failed(plan); });
    });
}

/// Completion hook, marshalled to the main thread and then guarded.
void finished(const FilamentOpSurface& surface, const std::function<void()>& hook) {
    if (!hook) {
        return;
    }
    helix::ui::queue_update("filament_op_execute::finished",
                            [s = surface, hook]() { guarded(s, hook); });
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
                             const char* what, std::function<void()> send) {
    if (!surface.before_macro) {
        send();
        return;
    }
    const FilamentOpSurface s = surface;
    surface.before_macro(std::move(send), [s, plan, what](const MoonrakerError& err) {
        spdlog::error("{} Not sending the {} macro: {}", s.log_tag, what, err.message);
        unwind_async(s, plan);
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

void execute_filament_load(AmsBackend* backend, int slot, const FilamentOpSurface& surface) {
    const char* log_tag = surface.log_tag;
    AmsSystemInfo sys;
    const helix::ui::BackendCaps caps = read_backend_caps(backend, sys, slot);

    const auto& load_info = StandardMacros::instance().get(StandardMacroSlot::LoadFilament);
    const helix::ui::FilamentOpPlan plan = plan_live_load(sys, caps, slot);

    switch (plan.tier) {
    case helix::ui::FilamentTier::AmsBackend: {
        spdlog::info("{} Load via AMS backend (slot {})", log_tag, slot);
        begin(surface, plan);
        AmsError err = (plan.ams_call == helix::ui::AmsCall::ChangeTool)
                           ? backend->change_tool(plan.ams_arg)
                           : backend->load_filament(plan.ams_arg);
        if (!err.success()) {
            spdlog::error("{} Load filament failed: {}", log_tag, err.technical_msg);
            unwind_backend(surface, plan, err);
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
        // AlreadyMounted: SELECT_TOOL on the carriage tool is a firmware no-op
        // that would leave the dialog looking like it did something.
        // SelectSlot: no lane resolved, and a surface with no picker cannot
        // offer one. Never navigate from here: tearing a dialog out from under
        // the user is what on_refused exists to let a surface decide.
        if (plan.refusal == helix::ui::FilamentRefusal::AlreadyMounted) {
            spdlog::info("{} Load refused — tool {} already mounted", log_tag, slot);
            NOTIFY_INFO(lv_tr("That tool is already loaded"));
        } else {
            spdlog::info("{} Load refused — no slot resolved", log_tag);
            NOTIFY_WARNING(lv_tr("Select a filament slot to load"));
        }
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
            [api, s, plan](const helix::MacroParamResult& result) {
                // Under ParamPolicy::Prompt this lands whenever the user presses
                // Run, which can be after the asking surface is gone.
                guarded(s, [api, s, plan, params = result.params]() {
                    begin(s, plan);
                    send_after_before_macro(s, plan, "load", [api, s, plan, params]() {
                        // execute_macro()'s reply lands when the script has RUN, so
                        // these are completion callbacks, not "started" ones.
                        const bool dispatched = StandardMacros::instance().execute(
                            StandardMacroSlot::LoadFilament, api, params,
                            [s]() {
                                spdlog::info("{} Load filament finished", s.log_tag);
                                finished(s, s.on_async_success);
                            },
                            [s, plan](const MoonrakerError& err) {
                                spdlog::error("{} Failed to load filament: {}", s.log_tag,
                                              err.message);
                                unwind_async(s, plan);
                                report_op_error(err, "load");
                            },
                            IMoonrakerAPI::EXTRUSION_TIMEOUT_MS);
                        if (!dispatched) {
                            // Empty slot or no API: neither callback will ever fire,
                            // so nothing else would release what begin() armed.
                            spdlog::warn("{} Load macro did not dispatch", s.log_tag);
                            unwind_async(s, plan);
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
            [s]() {
                spdlog::info("{} Load fallback gcode sent", s.log_tag);
                finished(s, s.on_async_success);
            },
            [s, plan](const MoonrakerError& err) {
                spdlog::error("{} Load fallback failed: {}", s.log_tag, err.message);
                unwind_async(s, plan);
                report_op_error(err, "load");
            },
            IMoonrakerAPI::EXTRUSION_TIMEOUT_MS, /*silent=*/false, /*on_queued=*/nullptr,
            /*caller_surfaces_errors=*/true); // report_op_error() toasts the failure
        return;
    }
    }
}

// ============================================================================
// Unload
// ============================================================================

// Extracted verbatim from FilamentRunoutHandler::dispatch_unload()
// (ui_filament_runout_handler.cpp), the cleanest of the three pre-existing
// unload bodies. target_is_loaded is a caller-supplied input rather than
// recomputed here — see unload_target_is_loaded()'s doc comment for why the
// three existing callers must not each answer that question inline.
void execute_filament_unload(AmsBackend* backend, int slot, bool target_is_loaded,
                             const char* log_tag) {
    FilamentOpSurface surface;
    surface.log_tag = log_tag;
    execute_filament_unload(backend, slot, target_is_loaded, surface);
}

void execute_filament_unload(AmsBackend* backend, int slot, bool target_is_loaded,
                             const FilamentOpSurface& surface) {
    const char* log_tag = surface.log_tag;
    // plan_unload() reads only `present` and `toolhead_unaccounted`, so the full
    // read_backend_caps() call (whose needs_unload_before_load() is a per-lane
    // backend query) buys nothing here.
    helix::ui::BackendCaps caps;
    caps.present = backend != nullptr;
    caps.toolhead_unaccounted = backend && backend->toolhead_filament_unaccounted().value_or(false);

    const auto& unload_info = StandardMacros::instance().get(StandardMacroSlot::UnloadFilament);
    const helix::ui::FilamentOpPlan plan = plan_live_unload(caps, slot, target_is_loaded);

    switch (plan.tier) {
    case helix::ui::FilamentTier::AmsBackend: {
        // Pass plan.ams_arg, not -1: the caller knows which slot it targeted and
        // says so, rather than letting the backend re-resolve current_slot (the
        // U1 wrong-tool unload bug).
        begin(surface, plan);
        AmsError err = backend->unload_filament(plan.ams_arg);
        if (!err.success()) {
            spdlog::error("{} Unload filament failed: {}", log_tag, err.technical_msg);
            unwind_backend(surface, plan, err);
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
            [api, s, plan](const helix::MacroParamResult& result) {
                guarded(s, [api, s, plan, params = result.params]() {
                    begin(s, plan);
                    send_after_before_macro(s, plan, "unload", [api, s, plan, params]() {
                        const bool dispatched = StandardMacros::instance().execute(
                            StandardMacroSlot::UnloadFilament, api, params,
                            [s]() {
                                spdlog::info("{} Unload filament finished", s.log_tag);
                                finished(s, s.on_async_success);
                            },
                            [s, plan](const MoonrakerError& err) {
                                spdlog::error("{} Failed to unload filament: {}", s.log_tag,
                                              err.message);
                                unwind_async(s, plan);
                                report_op_error(err, "unload");
                            },
                            IMoonrakerAPI::EXTRUSION_TIMEOUT_MS);
                        if (!dispatched) {
                            spdlog::warn("{} Unload macro did not dispatch", s.log_tag);
                            unwind_async(s, plan);
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
            [s]() {
                spdlog::info("{} Unload fallback gcode sent", s.log_tag);
                finished(s, s.on_async_success);
            },
            [s, plan](const MoonrakerError& err) {
                spdlog::error("{} Unload fallback failed: {}", s.log_tag, err.message);
                unwind_async(s, plan);
                report_op_error(err, "unload");
            },
            IMoonrakerAPI::EXTRUSION_TIMEOUT_MS, /*silent=*/false, /*on_queued=*/nullptr,
            /*caller_surfaces_errors=*/true); // report_op_error() toasts the failure
        return;
    }
    }
}

// ============================================================================
// Purge
// ============================================================================

// Modeled on FilamentRunoutHandler::dispatch_purge() (ui_filament_runout_handler.cpp),
// the one pre-existing purge dispatch NOT entangled with panel UI state.
// FilamentPanel::execute_purge() (ui_panel_filament.cpp) was deliberately not
// the source for this extraction: it drives that panel's operation_guard_
// spinner state and a macro-parameter modal with a nozzle-temperature prefill,
// neither of which has an equivalent on a home-tile modal.
void execute_filament_purge(const char* log_tag) {
    auto* api = get_moonraker_api();
    if (!api) {
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
            [api, log_tag](const helix::MacroParamResult& result) {
                StandardMacros::instance().execute(
                    StandardMacroSlot::Purge, api, result.params,
                    [log_tag]() { spdlog::info("{} Purge started", log_tag); },
                    [log_tag](const MoonrakerError& err) {
                        spdlog::error("{} Failed to purge: {}", log_tag, err.message);
                        helix::ui::notify_error_tr(TR_NOOP("Failed to purge: {}"), err);
                    });
            });
        return;
    }

    spdlog::info("{} No purge macro configured — raw gcode fallback", log_tag);
    api->execute_gcode(
        helix::ui::filament_purge_fallback_gcode(),
        [log_tag]() { spdlog::info("{} Purge fallback gcode sent", log_tag); },
        [log_tag](const MoonrakerError& err) {
            spdlog::error("{} Purge fallback failed: {}", log_tag, err.message);
            helix::ui::notify_error_tr(TR_NOOP("Failed to purge: {}"), err);
        },
        IMoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

} // namespace helix::ui
