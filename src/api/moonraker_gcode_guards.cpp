// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_gcode_guards.h"

#include "gcode_homing.h"
#include "json_utils.h"
#include "moonraker_validation.h"
#include "platform_info.h"
#include "plugin_permissions.h"
#include "printer_state.h"
#include "spdlog/spdlog.h"
#include "spool_latch_gate.h"
#include "translation_loader.h"

namespace helix {

const char* spool_latch_message() {
    return TR_NOOP("Spools are on the bed: remove them and confirm before moving the printer");
}

const char* spool_latch_restart_message() {
    return TR_NOOP("Spools are on the bed: tap the drying banner to confirm they are off, then "
                   "restart.");
}

} // namespace helix

namespace helix::api {

bool reject_homing_during_active_print(
    const std::string& gcode, helix::PrinterState& state, bool silent,
    const helix::SharedCallback<void(const MoonrakerError&)>& on_error, const char* log_tag) {
    if (!helix::is_homing_gcode(gcode)) {
        return false;
    }
    // RAW_PRINT_STATE_OK: this guard must NOT move to job_holds_machine().
    // PrintPreparationManager sends the user's configured pre-start block through
    // execute_gcode() from inside the preparing window
    // (ui_print_preparation_manager.cpp), and is_homing_gcode() matches any line
    // whose first token is G28 — on the K2 that block is the forced bed mesh.
    // Widening this to Preparing makes the app refuse its own pre-start G-code
    // and breaks print start on every printer whose block homes.
    //
    // The window is not left open: a firmware-side PRINT_START already reads
    // PRINTING here, and during a host-side block the app is the only thing
    // driving the toolhead — every affordance that could send a competing G28
    // (the motion/calibration XML bindings, the AMS filament ops, the bypass
    // tile) is disabled by job_holds_machine().
    // RAW_PRINT_STATE_OK: see the full reason above - widening this refuses the
    // app's own pre-start G-code.
    const helix::PrintJobState pstate = state.print_state().get_print_job_state();
    if (pstate != helix::PrintJobState::PRINTING && pstate != helix::PrintJobState::PAUSED) {
        return false;
    }
    if (!silent) {
        spdlog::warn("{} Refusing homing G-code during active print (state={}): '{}'", log_tag,
                     static_cast<int>(pstate), gcode.substr(0, 60));
    }
    if (on_error) {
        on_error(MoonrakerError::refusal(
            "printer.gcode.script", TR_NOOP("Homing is disabled while a print is in progress")));
    }
    return true;
}

bool reject_motion_while_spools_on_bed(
    const std::string& gcode, helix::PrinterState& state, bool silent,
    const helix::SharedCallback<void(const MoonrakerError&)>& on_error, const char* log_tag) {
    if (!state.print_state().spool_latch_active() ||
        helix::spool_latch_allows(gcode, state.print_state().spool_latch_extra_tokens())) {
        return false;
    }
    if (!silent) {
        spdlog::warn("{} Refusing G-code while spools are on the bed: '{}'", log_tag,
                     gcode.substr(0, 60));
    }
    if (on_error) {
        on_error(MoonrakerError::refusal("printer.gcode.script", helix::spool_latch_message()));
    }
    return true;
}

bool reject_job_while_spools_on_bed(
    const helix::PrinterState* state, const char* method,
    const helix::SharedCallback<void(const MoonrakerError&)>& on_error) {
    if (!state || !state->print_state().spool_latch_active()) {
        return false;
    }
    spdlog::warn("[Moonraker API] Refusing {} while spools are on the bed", method);
    if (on_error) {
        on_error(MoonrakerError::refusal(method, helix::spool_latch_message()));
    }
    return true;
}

bool reject_restart_while_spools_on_bed(
    const helix::PrinterState* state, const char* method,
    const helix::SharedCallback<void(const MoonrakerError&)>& on_error) {
    if (!state || !state->print_state().spool_latch_active()) {
        return false;
    }
    spdlog::warn("[Moonraker API] Refusing {} while spools are on the bed", method);
    if (on_error) {
        on_error(MoonrakerError::refusal(method, helix::spool_latch_restart_message()));
    }
    return true;
}

#if HELIX_HAS_PLUGINS
bool reject_plugin_rpc(helix::PrinterState& state, const std::string& method,
                       const nlohmann::json& params,
                       const helix::SharedCallback<void(const MoonrakerError&)>& on_error) {
    static constexpr const char* kTag = "[Plugin]";
    const auto str = [&params](const char* key) {
        return helix::json_util::safe_string(params, key, "");
    };

    if (method == "printer.print.start") {
        return moonraker_internal::reject_invalid_path(str("filename"), method.c_str(), on_error) ||
               reject_job_while_spools_on_bed(&state, method.c_str(), on_error);
    }
    // Both start a print from a linked copy of a file, checked here only for its
    // paths and the spool latch. A plugin's print_edited `edits` can inject any
    // gcode into that copy, past the printer.gcode.script motion guards: the
    // same power as printer.print.start of a file the plugin uploaded itself.
    if (method == "server.helix.print_modified" || method == "server.helix.print_edited") {
        return moonraker_internal::reject_invalid_path(str("original_filename"), method.c_str(),
                                                       on_error) ||
               moonraker_internal::reject_invalid_path(str("temp_file_path"), method.c_str(),
                                                       on_error) ||
               reject_job_while_spools_on_bed(&state, method.c_str(), on_error);
    }
    // The rest of server.helix.* heats, moves, deletes or drives the resume wizard, which
    // only a confirmed tap on the screen may do.
    if (method.rfind("server.helix.", 0) == 0 &&
        !helix::plugin::is_readonly_moonraker_method(method)) {
        spdlog::warn("{} Refusing plugin call to {}", kTag, method);
        if (on_error) {
            on_error(MoonrakerError::not_ready(method, method + " is not available to plugins"));
        }
        return true;
    }
    // Firmware that supervises HelixScreen owns the software and the configuration
    // (#2154): no plugin updates components or rewrites the config root there.
    if (helix::firmware_supervised()) {
        const bool updates =
            method.rfind("machine.update.", 0) == 0 && method != "machine.update.status";
        const bool file_write = method == "server.files.delete_file" ||
                                method == "server.files.copy" || method == "server.files.move" ||
                                method == "server.files.post_directory" ||
                                method == "server.files.delete_directory";
        if (updates || (file_write && (moonraker_internal::names_config_root("", str("path")) ||
                                       moonraker_internal::names_config_root("", str("source")) ||
                                       moonraker_internal::names_config_root("", str("dest"))))) {
            spdlog::warn("{} Refusing plugin call to {}: {}", kTag, method,
                         helix::kFirmwareSupervisedConfigRefusal);
            if (on_error) {
                on_error(
                    MoonrakerError::not_ready(method, helix::kFirmwareSupervisedConfigRefusal));
            }
            return true;
        }
    }
    if (method == "printer.print.resume" || method == "server.job_queue.start") {
        return reject_job_while_spools_on_bed(&state, method.c_str(), on_error);
    }
    if (method == "printer.restart" || method == "printer.firmware_restart" ||
        (method == "machine.services.restart" &&
         str("service").find("klipper") != std::string::npos)) {
        return reject_restart_while_spools_on_bed(&state, method.c_str(), on_error);
    }
    if (method == "printer.gcode.script") {
        const std::string script = str("script");
        return reject_homing_during_active_print(script, state, false, on_error, kTag) ||
               reject_motion_while_spools_on_bed(script, state, false, on_error, kTag);
    }
    return false;
}
#endif // HELIX_HAS_PLUGINS

} // namespace helix::api
