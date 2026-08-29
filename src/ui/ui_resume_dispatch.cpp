// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_resume_dispatch.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_update_queue.h"

#include "ams_state.h"
#include "app_globals.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_job_api.h"
#include "printer_state.h"
#include "standard_macros.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <utility>

namespace helix::ui {

namespace {

/// Restart-from-beginning dispatch for the modal shown when
/// virtual_sdcard.is_active=false makes RESUME a silent no-op. Invoked from the
/// modal's confirm callback with captured copies, so nothing here can dangle
/// with the dialog or leak on a dismissal.
void restart_from_beginning(IMoonrakerAPI* api, const std::string& filename,
                            const std::string& log_prefix,
                            const std::function<void()>& on_failure) {
    if (!api) {
        spdlog::error("{} restart_confirm: api is null", log_prefix);
        if (on_failure)
            on_failure();
        return;
    }
    if (filename.empty()) {
        spdlog::error("{} restart_confirm: filename is empty — cannot start", log_prefix);
        NOTIFY_ERROR(lv_tr("Cannot restart — no filename"));
        if (on_failure)
            on_failure();
        return;
    }

    spdlog::info("{} Restart-from-beginning confirmed; running "
                 "SDCARD_RESET_FILE + CANCEL_PRINT_BASE then start_print({})",
                 log_prefix, filename);

    // Multi-line gcode is accepted by Klipper's gcode.script. Both lines
    // run synchronously before "ok" returns, so on_success only fires
    // after CANCEL_PRINT_BASE has dropped the print_stats.state to standby.
    api->execute_gcode(
        "SDCARD_RESET_FILE\nCANCEL_PRINT_BASE",
        [api, filename, log_prefix, on_failure]() {
            queue_update("ui_resume_dispatch::restart_start_print", [api, filename, log_prefix,
                                                                     on_failure]() {
                api->job().start_print(
                    filename,
                    [log_prefix, filename]() {
                        spdlog::info("{} Restart succeeded for {}", log_prefix, filename);
                    },
                    [log_prefix, on_failure](const MoonrakerError& err) {
                        spdlog::error("{} start_print after restart failed: {}", log_prefix,
                                      err.message);
                        auto user_msg = err.user_message();
                        queue_update("ui_resume_dispatch::restart_start_error",
                                     [user_msg = std::move(user_msg), on_failure]() {
                                         NOTIFY_ERROR(lv_tr("Failed to restart: {}"), user_msg);
                                         if (on_failure)
                                             on_failure();
                                     });
                    });
            });
        },
        [log_prefix, on_failure](const MoonrakerError& err) {
            spdlog::error("{} restart prep gcode failed: {}", log_prefix, err.message);
            auto user_msg = err.user_message();
            queue_update("ui_resume_dispatch::restart_prep_error",
                         [user_msg = std::move(user_msg), on_failure]() {
                             NOTIFY_ERROR(lv_tr("Failed to clear print state: {}"), user_msg);
                             if (on_failure)
                                 on_failure();
                         });
        });
}

} // namespace

void show_restart_required_modal(IMoonrakerAPI* api, const std::string& filename,
                                 std::string log_prefix, std::function<void()> on_failure) {
    // Klipper's print_stats.message describes the cause of the abort
    // (Snapmaker firmware writes e.g. "Dirty bed detected" / "Filament Sensor:
    // Runout Detected"). When present, surface it so the user knows why
    // restart is needed instead of seeing only generic copy.
    const char* fw_msg = lv_subject_get_string(get_printer_state().get_print_message_subject());
    std::string body =
        (fw_msg && *fw_msg)
            ? fmt::format(lv_tr("Reason: {}\n\nThe printer cannot resume this print. "
                                "Restart from the beginning?"),
                          fw_msg)
            : std::string(lv_tr("The printer halted this print and cannot resume it. "
                                "Restart from the beginning?"));

    // Cancel and dismissal answer the question the same way: the user chose not
    // to restart, which the caller learns through on_failure.
    auto declined = [log_prefix, on_failure]() {
        spdlog::info("{} Restart-from-beginning modal cancelled by user", log_prefix);
        if (on_failure) {
            on_failure();
        }
    };

    lv_obj_t* modal = modal_confirm(
        lv_tr("Print Was Terminated"), body.c_str(), ModalSeverity::Warning, lv_tr("Restart"),
        [api, filename, log_prefix, on_failure]() {
            restart_from_beginning(api, filename, log_prefix, on_failure);
        },
        declined, nullptr, declined);
    if (!modal) {
        spdlog::error("{} Failed to create restart-from-beginning modal", log_prefix);
        if (on_failure) {
            on_failure();
        }
    }
}

void dispatch_prepared_resume(IMoonrakerAPI* api, std::string log_prefix,
                              std::function<void()> on_failure) {
    if (!api) {
        spdlog::warn("{} dispatch_prepared_resume: api is null", log_prefix);
        if (on_failure)
            on_failure();
        return;
    }

    // The macro-dispatch closure. The success path stays on whichever
    // thread the API delivers it — we only spdlog::info() there (thread
    // safe). The error path bounces through queue_update so the toast,
    // lv_tr() lookup, and `on_failure` body all run on the main thread
    // even though StandardMacros::execute may invoke this callback from
    // the libhv WebSocket event-loop thread on JSON-RPC failure.
    auto dispatch = [api, log_prefix, on_failure]() {
        StandardMacros::instance().execute(
            StandardMacroSlot::Resume, api,
            [log_prefix]() { spdlog::info("{} Resume command sent successfully", log_prefix); },
            [log_prefix, on_failure](const MoonrakerError& err) {
                spdlog::error("{} Failed to resume: {}", log_prefix, err.message);
                auto user_msg = err.user_message();
                helix::ui::queue_update("dispatch_prepared_resume::on_macro_error",
                                        [user_msg = std::move(user_msg), on_failure]() {
                                            NOTIFY_ERROR(lv_tr("Failed to resume: {}"), user_msg);
                                            if (on_failure)
                                                on_failure();
                                        });
            },
            /*timeout_ms=*/0, /*suppress_auto_toast=*/true);
    };

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        dispatch();
        return;
    }

    // prepare_for_resume's on_ready contract guarantees main-thread
    // invocation, so the prep-failure branch doesn't need its own
    // queue_update bounce.
    int slot = backend->get_current_slot();
    backend->prepare_for_resume(slot, [api, dispatch = std::move(dispatch), log_prefix,
                                       on_failure](const AmsError& err) mutable {
        if (err.result == AmsResult::RESUME_REQUIRES_RESTART) {
            // virtual_sdcard.is_active=false — RESUME would no-op.
            // Surface the restart-from-beginning modal instead of
            // firing the resume macro chain. Filename comes from
            // PrinterState (subscribed via print_stats.filename).
            std::string filename =
                lv_subject_get_string(get_printer_state().get_print_filename_subject());
            spdlog::warn("{} RESUME_REQUIRES_RESTART — showing restart modal (file: {})",
                         log_prefix, filename);
            show_restart_required_modal(api, filename, log_prefix, std::move(on_failure));
            return;
        }
        if (!err.success()) {
            spdlog::error("{} prepare_for_resume failed: {}", log_prefix, err.technical_msg);
            helix::ui::notify_ams_error(err, lv_tr("Resume preparation failed"));
            if (on_failure)
                on_failure();
            return;
        }
        dispatch();
    });
}

} // namespace helix::ui
