// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_completion.h"

#include "app_globals.h"
#include "audio_settings_manager.h"
#include "core/error_reporting.h"
#include "core/gcode_filename.h"
#include "display_wake_request.h"
#include "heater_type.h"
#include "moonraker_api.h"
#include "moonraker_manager.h"
#include "notice.h"
#include "print_completion_alert.h"
#include "printer_state.h"
#include "screensaver.h"
#include "sound_manager.h"
#include "temperature_controller.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

using helix::gcode::get_display_filename;
using helix::gcode::resolve_gcode_filename;

namespace helix {

static PrintStatusShownQuery s_print_status_shown = nullptr;

PrintStatusShownQuery set_print_status_shown_query(PrintStatusShownQuery query) {
    PrintStatusShownQuery previous = s_print_status_shown;
    s_print_status_shown = query;
    return previous;
}

// Helper to cleanup the staged G-code copy after a print ends
static void cleanup_helix_temp_file(const std::string& filename) {
    if (!helix::gcode::is_uploaded_rewrite_path(filename)) {
        return; // Not a copy we staged
    }

    auto* mgr = get_moonraker_manager();
    if (!mgr) {
        spdlog::warn("[PrintComplete] Cannot cleanup temp file - MoonrakerManager not available");
        return;
    }

    auto* api = mgr->api();
    if (!api) {
        spdlog::warn("[PrintComplete] Cannot cleanup temp file - API not available");
        return;
    }

    // Moonraker's delete_file requires full path including root
    std::string full_path = "gcodes/" + filename;
    spdlog::info("[PrintComplete] Cleaning up temp file: {}", full_path);

    api->files().delete_file(
        full_path,
        [filename]() { spdlog::info("[PrintComplete] Deleted temp file: {}", filename); },
        [filename](const MoonrakerError& err) {
            // Log but don't show error to user - cleanup is best-effort
            spdlog::warn("[PrintComplete] Failed to delete temp file {}: {}", filename,
                         err.message);
        });
}

bool started_new_job(PrintState lifecycle) {
    return lifecycle == PrintState::Preparing || lifecycle == PrintState::Printing;
}

bool should_notify_print_ended(PrintState prev, PrintState current, PrintOutcome outcome) {
    const bool is_terminal = (current == PrintState::Complete || current == PrintState::Cancelled ||
                              current == PrintState::Error);
    if (!is_terminal) {
        return false;
    }
    if (prev == PrintState::Printing || prev == PrintState::Paused) {
        return true;
    }

    // A print that dies INSIDE PRINT_START never passes through Printing: a live
    // pre-print phase outranks the job state, so the lifecycle holds at Preparing
    // while print_stats already reads printing, and when the phase clears it goes
    // straight Preparing -> terminal. This arm is the only report of those: the
    // preparing-exit observer stays silent, because the preparing job was retired
    // as Confirmed the moment the printer took it.
    //
    // The outcome is what makes this safe to accept. It is recorded on the
    // job-state transition into a terminal value and cleared by
    // begin_preparing(), so during a HOST-side block - where print_stats still
    // holds the previous job's terminal state and clearing the phase would also
    // derive Preparing -> Complete - it reads NONE. Without that term this arm
    // would announce "Print Complete!" for a print that ended minutes ago.
    return prev == PrintState::Preparing && outcome != PrintOutcome::NONE;
}

PreparingExitAction decide_preparing_exit_action(PreparingExit reason) {
    PreparingExitAction action;
    switch (reason) {
    case PreparingExit::Confirmed:
    case PreparingExit::Superseded:
        // A print is running - ours, or the one that superseded ours. Say
        // nothing and leave the heaters alone.
        break;
    case PreparingExit::EndedByPrinter:
        // The printer's terminal state reaches the completion observer, which
        // reports it; the printer's own end gcode owns the heaters.
        break;
    case PreparingExit::Cancelled:
        action.notify_cancelled = true;
        action.cool_down = true;
        break;
    case PreparingExit::Failed:
    case PreparingExit::TimedOut:
        action.notify_failure = true;
        action.cool_down = true;
        break;
    }
    return action;
}

// Callback for print state changes - triggers completion notifications
static void on_print_state_changed_for_notification(lv_observer_t* observer,
                                                    lv_subject_t* subject) {
    (void)observer;
    // Both halves of the transition come from PrinterPrintState, which computes
    // it once. No arming latch is needed: the published previous value starts
    // at Idle, so booting straight into a terminal state reads as
    // Idle -> Complete and correctly does not notify.
    (void)subject; // the lifecycle subject; read through the typed accessor
    const auto lifecycle = get_printer_state().print_state().get_print_lifecycle();
    // PRINT_STATE_CAST_OK: the PREVIOUS lifecycle has no typed accessor, and
    // this transition check needs the prior value; the prev subject is
    // PrintState-typed by construction, so the pairing is not in question.
    const auto prev_lifecycle = static_cast<PrintState>(
        lv_subject_get_int(get_printer_state().print_state().get_print_lifecycle_prev_subject()));

    spdlog::debug("[PrintComplete] Lifecycle: {} -> {}", static_cast<int>(prev_lifecycle),
                  static_cast<int>(lifecycle));

    const auto outcome = static_cast<PrintOutcome>(
        lv_subject_get_int(get_printer_state().print_state().get_print_outcome_subject()));

    // Take the previous job's result down as soon as a new one is committed.
    // Level, not edge, is deliberate: the handle is cleared on the dialog's
    // death, so once it has been taken down these calls are no-ops.
    if (started_new_job(lifecycle)) {
        publish_ui_request({UiRequestKind::DismissPrintCompletion});
    }

    if (should_notify_print_ended(prev_lifecycle, lifecycle, outcome)) {
        // The terminal job state still drives which message and sound are used.
        // RAW_PRINT_STATE_OK: terminal-outcome formatting is about what the
        // printer reported, and the outcome enum derives from it directly.
        const auto current = get_printer_state().print_state().get_print_job_state();
        // Get filename from PrinterState and format for display
        const char* raw_filename =
            lv_subject_get_string(get_printer_state().print_state().get_print_filename_subject());
        std::string resolved_filename =
            (raw_filename && raw_filename[0]) ? resolve_gcode_filename(raw_filename) : "";
        std::string display_name =
            !resolved_filename.empty() ? get_display_filename(resolved_filename) : "Unknown";

        // Cleanup temp files - delete .helix_temp/modified_* files after print ends
        // Do this before anything else so it happens regardless of notification settings
        if (raw_filename && raw_filename[0]) {
            cleanup_helix_temp_file(raw_filename);
        }

        // Play sound for terminal state (independent of CompletionAlertMode —
        // sound respects its own sounds_enabled toggle via SoundManager::play())
        switch (current) {
        case PrintJobState::COMPLETE:
            SoundManager::instance().play("print_complete", SoundPriority::EVENT);
            break;
        case PrintJobState::ERROR:
            SoundManager::instance().play("error_alert", SoundPriority::EVENT);
            break;
        case PrintJobState::CANCELLED:
            SoundManager::instance().play("print_cancelled", SoundPriority::EVENT);
            break;
        default:
            break;
        }

        const bool on_print_status = s_print_status_shown && s_print_status_shown();

        auto mode = AudioSettingsManager::instance().get_completion_alert_mode();

#if HELIX_ENABLE_SCREENSAVER
        const bool saver_running = ScreensaverManager::instance().shows_print_end();
#else
        const bool saver_running = false;
#endif
        const helix::PrintEnding ending =
            (current == PrintJobState::COMPLETE)    ? helix::PrintEnding::COMPLETE
            : (current == PrintJobState::CANCELLED) ? helix::PrintEnding::CANCELLED
                                                    : helix::PrintEnding::ERROR;
        const helix::CompletionAlertPlan plan =
            helix::plan_completion_alert(ending, mode, on_print_status, saver_running);

        spdlog::debug("[PrintComplete] Print {} - on_print_status={}, mode={}, saver={}, "
                      "wake={}, modal={}, toast={}",
                      (current == PrintJobState::COMPLETE)    ? "complete"
                      : (current == PrintJobState::CANCELLED) ? "cancelled"
                                                              : "failed",
                      on_print_status, static_cast<int>(mode), saver_running, plan.wake, plan.modal,
                      plan.toast);

        if (helix::should_turn_heaters_off(
                ending, get_printer_state().profile_state().firmware_owns_error_heaters())) {
            // Proactively turn off heaters on print error to prevent
            // indefinite heating (Klipper doesn't auto-off on error)
            if (auto* mgr = get_moonraker_manager()) {
                if (auto* client = mgr->client()) {
                    spdlog::info("[PrintComplete] Turning off heaters after print error");
                    client->gcode_script("TURN_OFF_HEATERS");
                }
            }
        }

        if (plan.wake) {
            helix::request_display_wake();
        }
        if (plan.toast) {
            NoticeSeverity severity = (current == PrintJobState::COMPLETE)
                                          ? NoticeSeverity::SUCCESS
                                          : NoticeSeverity::WARNING;
            std::string message = std::string("Print ") +
                                  ((current == PrintJobState::COMPLETE) ? "complete" : "canceled") +
                                  ": " + display_name;
            // Empty for a completed print (decide_print_stop_reason).
            const char* reason = lv_subject_get_string(
                get_printer_state().print_state().get_print_stop_reason_subject());
            if (reason && *reason) {
                message += std::string(" - ") + reason;
            }
            publish_notice({severity, message, 5000});
        }
        if (plan.modal) {
            UiRequest request{UiRequestKind::ShowPrintCompletion};
            request.code = static_cast<int>(current);
            request.text = display_name;
            publish_ui_request(request);
        }
    }
}

ObserverGuard init_print_completion_observer() {
    spdlog::debug("[PrintComplete] Observer registered, awaiting first Moonraker update");
    return ObserverGuard(get_printer_state().print_state().get_print_lifecycle_subject(),
                         on_print_state_changed_for_notification, nullptr);
}

namespace {

void apply_preparing_exit(PreparingExit reason) {
    const PreparingExitAction action = decide_preparing_exit_action(reason);

    if (action.notify_cancelled) {
        NOTIFY_INFO(lv_tr("Print canceled"));
    } else if (action.notify_failure) {
        NOTIFY_ERROR(lv_tr("Print did not start"));
    }

    if (!action.cool_down) {
        return;
    }

    // A pre-start block heats to print temperature. With no print to own that
    // heat, drop it. Routed through TemperatureController because it is the
    // single authority for heater targets (lint-enforced); a raw gcode send
    // would bypass its bookkeeping. Toast suppressed - the notification above
    // already told the user what happened.
    if (auto* temps = get_temperature_controller()) {
        helix::SendOptions opts;
        opts.toast = false;
        temps->set_target(HeaterType::Nozzle, 0.0, opts);
        temps->set_target(HeaterType::Bed, 0.0, opts);
        spdlog::info("[PrintComplete] Start ended as {} - heaters dropped",
                     preparing_exit_name(reason));
    }
}

void on_preparing_epoch_changed(lv_observer_t*, lv_subject_t* subject) {
    static int s_prev_epoch = 0;
    const int epoch = lv_subject_get_int(subject);
    const int prev = s_prev_epoch;
    s_prev_epoch = epoch;

    // Only a fall to 0 is a retirement; a bump is a new job starting.
    if (epoch != 0 || prev == 0) {
        return;
    }
    apply_preparing_exit(get_printer_state().print_state().last_preparing_exit());
}

} // namespace

ObserverGuard init_preparing_exit_observer() {
    return ObserverGuard(get_printer_state().print_state().get_preparing_epoch_subject(),
                         on_preparing_epoch_changed, nullptr);
}

void cleanup_stale_helix_temp_files(IMoonrakerAPI* api) {
    if (!api) {
        spdlog::warn("[PrintComplete] Cannot cleanup stale temp files - API not available");
        return;
    }

    // Every staging directory: a printer's copies sit in the visible one when
    // its Klipper cannot print from a dot-directory, and either may hold
    // leftovers from before that was known.
    for (const std::string& dir : helix::gcode::staging_dirs()) {
        // Moonraker returns ALL files in root, not just the path requested, so
        // the listing is filtered by path prefix here.
        api->files().list_files(
            "gcodes", dir, false,
            [api, dir](const std::vector<FileInfo>& files) {
                const std::string prefix = dir + "/";
                int cleanup_count = 0;
                for (const auto& file : files) {
                    if (file.is_dir || file.path.rfind(prefix, 0) != 0) {
                        continue;
                    }
                    cleanup_count++;

                    // Moonraker's delete_file requires full path including root
                    std::string filepath = "gcodes/" + file.path;
                    api->files().delete_file(
                        filepath,
                        [filepath]() {
                            spdlog::debug("[PrintComplete] Deleted stale temp file: {}", filepath);
                        },
                        [filepath](const MoonrakerError& err) {
                            spdlog::warn("[PrintComplete] Failed to delete stale temp file {}: {}",
                                         filepath, err.message);
                        });
                }

                if (cleanup_count > 0) {
                    spdlog::info("[PrintComplete] Cleaning up {} stale temp files from {}",
                                 cleanup_count, dir);
                } else {
                    spdlog::debug("[PrintComplete] No stale temp files to clean up in {}", dir);
                }
            },
            [dir](const MoonrakerError& err) {
                // Directory doesn't exist or can't be listed - that's fine
                spdlog::debug("[PrintComplete] No {} directory to clean: {}", dir, err.message);
            });
    }
}

} // namespace helix
