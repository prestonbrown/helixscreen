// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "home_macro_run.h"

#include "ui_modal.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "macro_executor.h"
#include "macro_param_cache.h"
#include "macro_param_defaults.h"
#include "macro_param_modal.h"
#include "printer_state.h"
#include "safety_settings_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace helix {

namespace {

// One parameter modal for every home control: two widgets each holding their own would
// stomp on MacroParamModal's single active instance.
MacroParamModal& get_shared_param_modal() {
    static MacroParamModal modal;
    return modal;
}

// Everything a dialog callback needs, by value: the control that asked may be destroyed
// while the dialog is open.
struct MacroExecCtx {
    std::string macro_name;
    IMoonrakerAPI* api;
    lv_obj_t* parent_screen;
    MacroParamDefaultRecord defaults;
    std::string log_tag;
};

void execute(IMoonrakerAPI* api, const std::string& name, const MacroParamResult& params,
             const std::string& log_tag) {
    execute_macro_gcode(api, name, params, log_tag.c_str(), get_printer_state().get_discovery());
}

// Reached after the dangerous-macro dialog, or from a run headed for the parameter modal,
// so it never asks "Run X?": the control's setting and the Safety toggle do not apply
// here, and the decision weighs parameters alone.
void run_macro_after_confirm(MacroExecCtx ctx) {
    auto cached = MacroParamCache::instance().get(ctx.macro_name);

    MacroRunRequest req;
    req.prompt_for_params = ctx.defaults.ask_for_params;
    req.saved_values = ctx.defaults.values;

    const MacroRunDecision decision = decide_macro_run(cached, req);

    if (decision.action == MacroRunAction::Run) {
        execute(ctx.api, ctx.macro_name,
                macro_param_result_from_values(cached.params, decision.params), ctx.log_tag);
        return;
    }
    if (!ctx.parent_screen) {
        return;
    }

    auto on_result = [api = ctx.api, name = ctx.macro_name, log_tag = ctx.log_tag](
                         const MacroParamResult& result) { execute(api, name, result, log_tag); };
    if (decision.action == MacroRunAction::Prompt) {
        get_shared_param_modal().show_for_macro(ctx.parent_screen, ctx.macro_name, cached.params,
                                                std::move(on_result), decision.params);
    } else {
        get_shared_param_modal().show_for_unknown_params(ctx.parent_screen, ctx.macro_name,
                                                         std::move(on_result), decision.params);
    }
}

} // namespace

void run_home_macro(IMoonrakerAPI* api, const HomeMacroRun& run) {
    const std::string log_tag = fmt::format("[{}]", run.caller_tag ? run.caller_tag : "HomeMacro");
    if (!api) {
        spdlog::warn("{} No API available", log_tag);
        return;
    }
    const std::string& macro_name = run.macro_name;
    auto cached = MacroParamCache::instance().get(macro_name);

    // Saved defaults: with confirmation off the control runs the macro outright, so it
    // sends the saved values; with it on, ask must also be on for the param modal.
    const MacroParamDefaultRecord defaults = MacroParamDefaults::instance().get(macro_name);

    MacroRunRequest req;
    req.dangerous = is_dangerous_macro(macro_name, get_printer_state().get_discovery());
    req.prompt_for_params = run.require_confirmation && defaults.ask_for_params;
    req.confirm_plain_run = run.require_confirmation &&
                            SafetySettingsManager::instance().get_macro_require_confirmation();
    req.saved_values = defaults.values;

    const MacroRunDecision decision = decide_macro_run(cached, req);
    const MacroRunAction action = decision.action;

    // A dangerous macro is always confirmed: a home-screen control is one accidental tap
    // away from EMERGENCY_STOP / FIRMWARE_RESTART (#925). The control's opt-out cannot
    // disarm it.
    if (action == MacroRunAction::ConfirmDangerous) {
        if (!run.parent_screen) {
            spdlog::warn("{} No parent screen for dangerous-macro confirm", log_tag);
            return;
        }
        spdlog::warn("{} Dangerous macro requested: {}", log_tag, macro_name);
        const std::string display = get_display_name(macro_name, DeviceType::MACRO);
        const std::string msg =
            fmt::format(lv_tr("{} may cause unintended changes. Are you sure?"), display);
        ui::ConfirmOptions opts;
        opts.on_cancel = [macro_name, log_tag] {
            spdlog::debug("{} Dangerous macro cancelled: {}", log_tag, macro_name);
        };
        ui::modal_confirm(
            lv_tr("Run Dangerous Macro?"), msg.c_str(), ::ModalSeverity::Warning, lv_tr("Run"),
            [macro_name, api, parent = run.parent_screen, defaults, log_tag] {
                run_macro_after_confirm({macro_name, api, parent, defaults, log_tag});
            },
            opts);
        return;
    }

    // One tap runs the macro with no dialog: the control's "Require Confirmation?" opt-out
    // suppresses the Safety confirmation as well as the parameter modal. A run
    // confirmation with no parent screen also lands here, since there is nowhere to hang
    // the dialog. Either way the saved values go with it.
    if (action == MacroRunAction::Run ||
        (action == MacroRunAction::ConfirmRun && !run.parent_screen)) {
        execute(api, macro_name, macro_param_result_from_values(cached.params, decision.params),
                log_tag);
        return;
    }

    // The Settings > Safety run confirmation, for a run without a parameter modal.
    if (action == MacroRunAction::ConfirmRun) {
        const MacroParamResult params =
            macro_param_result_from_values(cached.params, decision.params);
        const std::string display = get_display_name(macro_name, DeviceType::MACRO);
        const std::string msg = fmt::format(lv_tr("Run {}?"), display);
        ui::modal_confirm(
            lv_tr("Run Macro?"), msg.c_str(), ::ModalSeverity::Info, lv_tr("Run"),
            [macro_name, api, params, log_tag] { execute(api, macro_name, params, log_tag); });
        return;
    }

    // Parameters known or unknown: the parameter modal is itself the confirmation step.
    run_macro_after_confirm({macro_name, api, run.parent_screen, defaults, log_tag});
}

} // namespace helix
