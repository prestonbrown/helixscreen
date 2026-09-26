// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_op_router.h"

#include "filament_op_slot_resolver.h"
#include "macro_executor.h"
#include "macro_param_defaults.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <lvgl.h>

namespace helix::ui {

namespace {

/// Storage for the installed prompter. Empty means "use the shared modal".
ParamPrompter& prompter_slot() {
    static ParamPrompter prompter;
    return prompter;
}

/// Storage for the installed home-confirm prompter. Empty means "proceed
/// immediately" -- see request_home_confirmation().
HomeConfirmPrompter& home_confirm_prompter_slot() {
    static HomeConfirmPrompter prompter;
    return prompter;
}

void show_shared_param_modal(const std::string& macro_name, const helix::CachedMacroInfo& cached,
                             const std::map<std::string, std::string>& prefill,
                             helix::MacroExecuteCallback on_execute) {
    if (cached.knowledge == helix::MacroParamKnowledge::KNOWN_PARAMS) {
        get_filament_param_modal().show_for_macro(lv_screen_active(), macro_name, cached.params,
                                                  std::move(on_execute), prefill);
        return;
    }
    get_filament_param_modal().show_for_unknown_params(lv_screen_active(), macro_name,
                                                       std::move(on_execute));
}

} // namespace

helix::MacroParamModal& get_filament_param_modal() {
    static helix::MacroParamModal modal;
    return modal;
}

void set_filament_param_prompter(ParamPrompter prompter) {
    prompter_slot() = std::move(prompter);
}

bool dispatch_filament_macro(const std::string& macro_name, ParamPolicy policy,
                             helix::MacroExecuteCallback run,
                             const std::map<std::string, std::string>& known_values) {
    if (!run) {
        spdlog::error("[FilamentRouter] No run callback for macro '{}'", macro_name);
        return false;
    }

    const helix::CachedMacroInfo cached = helix::MacroParamCache::instance().get(macro_name);

    // Saved defaults: ask off runs with the saved values, ask on prefills the
    // modal with them. Either way the surface's own policy still applies.
    const helix::MacroParamDefaultRecord defaults =
        helix::MacroParamDefaults::instance().get(macro_name);

    helix::MacroRunRequest req;
    req.prompt_for_params = policy != ParamPolicy::Suppress && defaults.ask_for_params;
    req.known_values = known_values;
    req.saved_values = defaults.values;

    const helix::MacroRunDecision decision = helix::decide_macro_run(cached, req);

    if (decision.action == helix::MacroRunAction::Run) {
        if (decision.params.empty()) {
            // KNOWN_NO_PARAMS, or a surface that must not stack a second modal —
            // run straight through with an empty result.
            spdlog::debug("[FilamentRouter] Executing '{}' with no parameters (policy={})",
                          macro_name, policy == ParamPolicy::Suppress ? "suppress" : "no-params");
            run({});
        } else {
            spdlog::info("[FilamentRouter] Every parameter of '{}' is known — running without a "
                         "prompt",
                         macro_name);
            run(helix::macro_param_result_from_values(cached.params, decision.params));
        }
        return false;
    }

    spdlog::info("[FilamentRouter] Macro '{}' takes parameters — prompting ({} prefilled)",
                 macro_name, decision.params.size());
    const ParamPrompter& prompter = prompter_slot();
    if (prompter) {
        prompter(macro_name, cached, decision.params, std::move(run));
    } else {
        show_shared_param_modal(macro_name, cached, decision.params, std::move(run));
    }
    return true;
}

std::map<std::string, std::string> nozzle_temp_prefill(FilamentMacroOp op, int extruder_target_c,
                                                       std::optional<int> material_temp_c,
                                                       int min_extrude_c, int max_nozzle_c) {
    const int temp_c = filament_op_nozzle_temp(material_temp_c.value_or(0), extruder_target_c);
    if (temp_c <= 0 || temp_c <= min_extrude_c || temp_c > max_nozzle_c) {
        return {};
    }
    const std::string value = std::to_string(temp_c);
    switch (op) {
    case FilamentMacroOp::Purge:
        return {{"PURGE_TEMP", value}};
    case FilamentMacroOp::Load:
    case FilamentMacroOp::Unload:
        break;
    }
    return {{"EXTRUDER_TEMP", value}, {"NOZZLE_TEMP", value}, {"TEMP", value}};
}

void set_home_confirm_prompter(HomeConfirmPrompter prompter) {
    home_confirm_prompter_slot() = std::move(prompter);
}

void request_home_confirmation(std::function<void()> on_confirm, std::function<void()> on_cancel) {
    const HomeConfirmPrompter& prompter = home_confirm_prompter_slot();
    if (!prompter) {
        // No prompter installed: proceed exactly as before this seam existed.
        on_confirm();
        return;
    }
    prompter(std::move(on_confirm), std::move(on_cancel));
}

std::string filament_load_fallback_gcode() {
    // Fast move through bowden (56mm at 20mm/s) then slow push into the melt
    // zone (24mm at 5mm/s).
    constexpr int LOAD_FAST_MM = 56;
    constexpr int LOAD_FAST_SPEED = 20 * 60; // 20 mm/s → 1200 mm/min
    constexpr int LOAD_SLOW_MM = 24;
    constexpr int LOAD_SLOW_SPEED = 5 * 60; // 5 mm/s → 300 mm/min
    return fmt::format("M83\nG1 E{} F{}\nG1 E{} F{}", LOAD_FAST_MM, LOAD_FAST_SPEED, LOAD_SLOW_MM,
                       LOAD_SLOW_SPEED);
}

std::string filament_unload_fallback_gcode() {
    // Tip-shape (push 3mm, quick pull 5mm, dwell) then retract 80mm.
    constexpr int UNLOAD_MM = 80;
    constexpr int UNLOAD_SPEED = 20 * 60;   // 20 mm/s → 1200 mm/min
    constexpr int TIP_PUSH_SPEED = 5 * 60;  // 5 mm/s → 300 mm/min
    constexpr int TIP_PULL_SPEED = 60 * 60; // 60 mm/s → 3600 mm/min
    return fmt::format("M83\nG1 E3 F{}\nG1 E-5 F{}\nG4 P500\nG1 E-{} F{}", TIP_PUSH_SPEED,
                       TIP_PULL_SPEED, UNLOAD_MM, UNLOAD_SPEED);
}

std::string filament_purge_fallback_gcode() {
    constexpr int PURGE_FALLBACK_MM = 50;
    constexpr int PURGE_FALLBACK_SPEED_MM_MIN = 10 * 60; // 10 mm/s → 600 mm/min
    return fmt::format("M83\nG1 E{} F{}", PURGE_FALLBACK_MM, PURGE_FALLBACK_SPEED_MM_MIN);
}

} // namespace helix::ui
