// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_op_execute.h"

#include "ui_error_reporting.h"

#include "ams_backend.h"
#include "app_globals.h"
#include "filament_op_dispatch.h"
#include "filament_op_router.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "standard_macros.h"

#include <spdlog/spdlog.h>

#include <string>

namespace helix::ui {

// ============================================================================
// Load
// ============================================================================

// Extracted verbatim from PrintStatusWidget::dispatch_load() (the only caller
// before this file existed); only the hardcoded "[PrintStatusWidget]" prefix
// became `log_tag`. backend/slot resolution stays with each caller — this
// dialog's "backend's own active slot is the only target" reasoning does not
// generalize to every future caller.
void execute_filament_load(AmsBackend* backend, int slot, const char* log_tag) {
    AmsSystemInfo sys;
    helix::ui::BackendCaps caps;
    if (backend) {
        sys = backend->get_system_info();
        caps.present = true;
        caps.requires_slot_selection_for_load = backend->requires_slot_selection_for_load();
        caps.needs_unload_before_load = backend->needs_unload_before_load(sys, slot);
        caps.is_tool_changer = backend->get_type() == AmsType::TOOL_CHANGER;
        // Distinct from !requires_slot_selection_for_load(): plan_load() needs to
        // tell "bypass is suppressing the lane tier" apart from "this backend
        // never wanted a slot", because a named lane wants opposite treatment.
        caps.bypass_active = backend->is_bypass_active();
    }

    const auto& load_info = StandardMacros::instance().get(StandardMacroSlot::LoadFilament);
    const helix::ui::FilamentOpPlan plan = helix::ui::plan_load(
        sys, caps, slot, !load_info.is_empty(), load_info.get_source() == MacroSource::CONFIGURED);

    switch (plan.tier) {
    case helix::ui::FilamentTier::AmsBackend: {
        spdlog::info("{} Load via AMS backend (slot {})", log_tag, slot);
        AmsError err = (plan.ams_call == helix::ui::AmsCall::ChangeTool)
                           ? backend->change_tool(plan.ams_arg)
                           : backend->load_filament(plan.ams_arg);
        if (!err.success()) {
            spdlog::error("{} Load filament failed: {}", log_tag, err.technical_msg);
            helix::ui::notify_ams_error(err);
        }
        return;
    }

    case helix::ui::FilamentTier::Refused:
        // AlreadyMounted: SELECT_TOOL on the carriage tool is a firmware no-op
        // that would leave the dialog looking like it did something (9KRXZ62P).
        // SelectSlot: no lane resolved, and none of these surfaces has a picker.
        // Never navigate either: PanelId::Filament was the old behaviour and it
        // tore the dialog out from under the user. Say what happened, stay put.
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
            return;
        }
        const std::string macro_name = load_info.get_macro();
        spdlog::info("{} Using StandardMacros load: {}", log_tag, macro_name);
        // ParamPolicy::Suppress runs the callback synchronously, so nothing here
        // outlives this call and no token capture is needed inside it.
        helix::ui::dispatch_filament_macro(
            macro_name, helix::ui::ParamPolicy::Suppress,
            [api, log_tag](const helix::MacroParamResult& result) {
                StandardMacros::instance().execute(
                    StandardMacroSlot::LoadFilament, api, result.params,
                    [log_tag]() { spdlog::info("{} Load filament started", log_tag); },
                    [log_tag](const MoonrakerError& err) {
                        spdlog::error("{} Failed to load filament: {}", log_tag, err.message);
                        NOTIFY_ERROR(lv_tr("Failed to load filament: {}"), err.user_message());
                    });
            });
        return;
    }

    case helix::ui::FilamentTier::RawGcode: {
        auto* api = get_moonraker_api();
        if (!api) {
            return;
        }
        spdlog::info("{} No backend and no load macro — raw gcode fallback", log_tag);
        api->execute_gcode(
            helix::ui::filament_load_fallback_gcode(),
            [log_tag]() { spdlog::info("{} Load fallback gcode sent", log_tag); },
            [log_tag](const MoonrakerError& err) {
                spdlog::error("{} Load fallback failed: {}", log_tag, err.message);
                NOTIFY_ERROR(lv_tr("Failed to load filament: {}"), err.user_message());
            },
            IMoonrakerAPI::EXTRUSION_TIMEOUT_MS);
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
    helix::ui::BackendCaps caps;
    if (backend) {
        caps.present = true;
    }

    const auto& unload_info = StandardMacros::instance().get(StandardMacroSlot::UnloadFilament);
    const helix::ui::FilamentOpPlan plan =
        helix::ui::plan_unload(caps, slot, target_is_loaded, !unload_info.is_empty(),
                               unload_info.get_source() == MacroSource::CONFIGURED);

    switch (plan.tier) {
    case helix::ui::FilamentTier::AmsBackend: {
        // Pass plan.ams_arg, not -1: the caller knows which slot it targeted and
        // says so, rather than letting the backend re-resolve current_slot (the
        // U1 wrong-tool unload bug). Same choice FilamentPanel makes.
        AmsError err = backend->unload_filament(plan.ams_arg);
        if (!err.success()) {
            spdlog::error("{} Unload filament failed: {}", log_tag, err.technical_msg);
            helix::ui::notify_ams_error(err);
        }
        return;
    }

    case helix::ui::FilamentTier::Refused:
        // NothingLoaded is plan_unload's only refusal. Say so and stay put —
        // navigating away would tear down the dialog the user is standing in.
        spdlog::info("{} Unload refused — nothing loaded (slot={})", log_tag, slot);
        NOTIFY_WARNING(lv_tr("No filament loaded to unload"));
        return;

    case helix::ui::FilamentTier::Macro: {
        auto* api = get_moonraker_api();
        if (!api) {
            return;
        }
        const std::string macro_name = unload_info.get_macro();
        spdlog::info("{} Using StandardMacros unload: {}", log_tag, macro_name);
        helix::ui::dispatch_filament_macro(
            macro_name, helix::ui::ParamPolicy::Suppress,
            [api, log_tag](const helix::MacroParamResult& result) {
                StandardMacros::instance().execute(
                    StandardMacroSlot::UnloadFilament, api, result.params,
                    [log_tag]() { spdlog::info("{} Unload filament started", log_tag); },
                    [log_tag](const MoonrakerError& err) {
                        spdlog::error("{} Failed to unload filament: {}", log_tag, err.message);
                        NOTIFY_ERROR(lv_tr("Failed to unload: {}"), err.user_message());
                    });
            });
        return;
    }

    case helix::ui::FilamentTier::RawGcode: {
        auto* api = get_moonraker_api();
        if (!api) {
            return;
        }
        spdlog::info("{} No backend and no unload macro — raw gcode fallback", log_tag);
        api->execute_gcode(
            helix::ui::filament_unload_fallback_gcode(),
            [log_tag]() { spdlog::info("{} Unload fallback gcode sent", log_tag); },
            [log_tag](const MoonrakerError& err) {
                spdlog::error("{} Unload fallback failed: {}", log_tag, err.message);
                NOTIFY_ERROR(lv_tr("Failed to unload: {}"), err.user_message());
            },
            IMoonrakerAPI::EXTRUSION_TIMEOUT_MS);
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
// spinner state and a macro-parameter modal with active-material temperature
// prefill, neither of which has an equivalent on a home-tile modal.
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
                        NOTIFY_ERROR(lv_tr("Failed to purge: {}"), err.user_message());
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
            NOTIFY_ERROR(lv_tr("Failed to purge: {}"), err.user_message());
        },
        IMoonrakerAPI::EXTRUSION_TIMEOUT_MS);
}

} // namespace helix::ui
