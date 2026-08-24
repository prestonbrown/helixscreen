// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_plr_prompt.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_filename_utils.h"
#include "ui_modal.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "moonraker_error.h"
#include "plr_backend.h"
#include "printer_state.h"
#include "snapmaker_resume.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <utility>

namespace helix::ui {

namespace {

/// The plan the currently-visible prompt will execute.
///
/// Module state rather than heap user_data on purpose: the modal is one-shot and
/// exclusive (only one recovery prompt can exist), the two buttons already carry
/// the borrowed IMoonrakerAPI* as their user_data, and a backdrop/ESC dismissal
/// fires NEITHER handler — so any per-show heap context would leak on that path.
/// A by-value plan whose std::strings own their storage sidesteps both problems
/// and keeps the resume authorization frozen at offer time.
// DECLARATIVE_OK: modal action payload, not UI state — nothing to bind a subject to.
PlrRecoveryPlan g_active_plan;

// Both button handlers receive the borrowed IMoonrakerAPI* via user_data. The
// gcode error callback may arrive on the libhv WebSocket thread, so the coded
// message is extracted on that thread (pure string work) and the toast is
// bounced onto the main thread via queue_update — never capture widgets or
// `this` (there is no owning object; the modal is fire-and-forget).
//
// fail_fmt_tr is the ALREADY-TRANSLATED format string (lv_tr(...) called by
// the caller, at click time, with a literal). lv_tr(...) returns a
// static-lifetime pointer (same precedent as the button labels above), so
// capturing it into the deferred lambda is safe. Translating at the call site
// rather than re-calling lv_tr() on a stored raw format string keeps the
// literal directly adjacent to lv_tr( in the source, which the translation
// sync tool's extractor requires to discover the key — lv_tr(variable) is
// invisible to it.
//
// log_tag must be a string LITERAL: it is captured by pointer into a callback
// that outlives this frame. Never pass plan.resume_gcode.c_str().
void run_recovery_gcode(IMoonrakerAPI* api, const std::string& gcode, const char* fail_fmt_tr,
                        const char* log_tag) {
    api->execute_gcode(
        gcode, [log_tag]() { spdlog::info("[PLR] {} accepted by firmware", log_tag); },
        [fail_fmt_tr, log_tag](const MoonrakerError& err) {
            spdlog::error("[PLR] {} failed: {}", log_tag, err.message);
            std::string detail =
                helix::snapmaker_extract_coded_msg(err.message, err.user_message());
            helix::ui::queue_update("ui_plr_prompt::recovery_error",
                                    [fail_fmt_tr, detail = std::move(detail)]() {
                                        NOTIFY_ERROR(fmt::runtime(fail_fmt_tr), detail);
                                    });
        });
}

/// Report a discard/resume failure that arrived from a non-gcode (JSON-RPC)
/// path. Same thread discipline as run_recovery_gcode's error leg.
void report_action_error(const MoonrakerError& err, const char* fail_fmt_tr, const char* log_tag) {
    spdlog::error("[PLR] {} failed: {}", log_tag, err.message);
    std::string detail = err.user_message();
    helix::ui::queue_update("ui_plr_prompt::recovery_error",
                            [fail_fmt_tr, detail = std::move(detail)]() {
                                NOTIFY_ERROR(fmt::runtime(fail_fmt_tr), detail);
                            });
}

void on_plr_resume(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PLR] on_plr_resume");
    auto* api = static_cast<IMoonrakerAPI*>(lv_event_get_user_data(e));
    Modal::hide(Modal::get_top());
    if (!api) {
        spdlog::error("[PLR] resume: api is null");
        return;
    }
    // Last line of defence for the probe-before-resume invariant. The prompt is
    // only ever shown with resume_allowed() true, so reaching this branch means
    // a caller bypassed evaluate_offer — refuse rather than send a command that
    // can crash the toolhead through a tall part.
    if (!g_active_plan.resume_allowed()) {
        spdlog::error("[PLR] resume REFUSED: no authorized recovery plan "
                      "(backend={}) — see docs/devel/POWER_LOSS_RECOVERY.md",
                      static_cast<int>(g_active_plan.backend));
        return;
    }
    spdlog::info("[PLR] User chose Resume — running '{}'", g_active_plan.resume_gcode);
    run_recovery_gcode(api, g_active_plan.resume_gcode, lv_tr("Recovery failed: {}"), "Resume");
    LVGL_SAFE_EVENT_CB_END();
}

void on_plr_discard(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PLR] on_plr_discard");
    auto* api = static_cast<IMoonrakerAPI*>(lv_event_get_user_data(e));
    Modal::hide(Modal::get_top());
    if (!api) {
        spdlog::error("[PLR] discard: api is null");
        return;
    }
    if (!g_active_plan.discard_gcode.empty()) {
        spdlog::info("[PLR] User chose Discard — running '{}'", g_active_plan.discard_gcode);
        run_recovery_gcode(api, g_active_plan.discard_gcode,
                           lv_tr("Failed to discard recovery data: {}"), "Discard");
    } else if (!g_active_plan.discard_rpc_method.empty()) {
        spdlog::info("[PLR] User chose Discard — calling '{}'", g_active_plan.discard_rpc_method);
        const char* fail_fmt = lv_tr("Failed to discard recovery data: {}");
        api->cancel_continue_print([]() { spdlog::info("[PLR] Discard accepted by firmware"); },
                                   [fail_fmt](const MoonrakerError& err) {
                                       report_action_error(err, fail_fmt, "Discard");
                                   });
    } else {
        spdlog::warn("[PLR] discard: plan carries no discard action");
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace

std::string plr_prompt_body(const std::string& file_path, const char* with_file_fmt,
                            const char* generic_body) {
    const char* generic = generic_body ? generic_body : "";
    if (file_path.empty() || !with_file_fmt) {
        return generic;
    }
    std::string name = helix::gcode::get_display_filename(file_path);
    if (name.empty()) {
        return generic;
    }
    try {
        return fmt::format(fmt::runtime(with_file_fmt), name);
    } catch (const std::exception& ex) {
        // A mistranslated placeholder must never abort through the LVGL C
        // dispatch frame — fall back to the generic body.
        spdlog::warn("[PLR] body format failed: {}", ex.what());
        return generic;
    }
}

PlrPromptStrings plr_prompt_strings(PlrBackendType backend, const PlrPromptStrings& creality,
                                    const PlrPromptStrings& standard) {
    return backend == PlrBackendType::CREALITY ? creality : standard;
}

void show_plr_recovery_prompt(IMoonrakerAPI* api, const helix::PlrRecoveryPlan& plan) {
    if (!api) {
        spdlog::warn("[PLR] show_plr_recovery_prompt: api is null — skipping");
        return;
    }
    if (!plan.resume_allowed()) {
        // Showing a Resume button that must refuse is worse than showing
        // nothing. evaluate_offer already filters this; belt and braces.
        spdlog::warn("[PLR] show_plr_recovery_prompt: plan has no authorized resume — skipping");
        return;
    }

    g_active_plan = plan;

    // Literals stay here (the translation extractor needs them lexically next to
    // lv_tr); only the choice between them is factored out, so it can be tested
    // without the XML engine. Rationale for the split copy: plr_prompt_strings().
    const PlrPromptStrings creality{
        lv_tr("The printer lost power while printing {}. The resumed layer may not line up "
              "exactly."),
        lv_tr("The printer lost power during a print. The resumed layer may not line up "
              "exactly.")};
    const PlrPromptStrings standard{
        lv_tr("The printer lost power while printing {}. Resume where it left off?"),
        lv_tr("The printer lost power during a print. It can resume where it left off.")};

    const PlrPromptStrings chosen = plr_prompt_strings(plan.backend, creality, standard);
    std::string body = plr_prompt_body(plan.recovery_file, chosen.with_file, chosen.generic);

    // Resume = primary/confirm, Discard = secondary/cancel. modal_show_confirmation
    // wires both non-null handlers directly (no auto-close wrapper), passing
    // `api` as borrowed user_data — the handlers keep their own Modal::hide +
    // null-check. lv_tr(...) returns static-lifetime strings, which the modal
    // stores by pointer, so they must outlive the modal (they do).
    lv_obj_t* dialog = modal_show_confirmation(lv_tr("Resume interrupted print?"), body.c_str(),
                                               ModalSeverity::Info, lv_tr("Resume"), on_plr_resume,
                                               on_plr_discard, api, lv_tr("Discard"));
    if (!dialog) {
        spdlog::error("[PLR] Failed to create recovery prompt modal");
        return;
    }

    spdlog::info("[PLR] Recovery prompt shown (backend={}, recovery_file='{}')",
                 static_cast<int>(plan.backend), plan.recovery_file);
}

} // namespace helix::ui
