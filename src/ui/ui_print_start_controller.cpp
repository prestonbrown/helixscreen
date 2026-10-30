// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_print_start_controller.cpp
 * @brief Controller for print initiation workflow
 *
 * Extracted from ui_panel_print_select.cpp to separate print start concerns.
 */

#include "ui_print_start_controller.h"

#include "ui_bed_drying_modal.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_panel_print_status.h"
#include "ui_print_preparation_manager.h"
#include "ui_print_select_detail_view.h"
#include "ui_update_queue.h"

#include "active_print_media_manager.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "config.h"
#include "data_root_resolver.h"
#include "filament_sensor_manager.h"
#include "helix_fs.h"
#include "i_moonraker_api.h"
#include "input_sensor_manager.h"
#include "json_utils.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "pre_start_exclude.h"
#include "print_job_ref.h"
#include "printer_state.h"
#include "text_io.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <optional>

#include "hv/json.hpp"

namespace helix::ui {

namespace tio = helix::text_io;
namespace hfs = helix::fs;

// ============================================================================
// Constructor / Destructor
// ============================================================================

PrintStartController::PrintStartController(PrinterState& printer_state, IMoonrakerAPI* api)
    : printer_state_(printer_state), api_(api), gate_list_(helix::default_print_start_gates()) {
    spdlog::debug("[PrintStartController] Created");
}

PrintStartController::~PrintStartController() {
    // The record itself stays in AmsState: the next print start keeps it as its
    // snapshot and restores it at that print's end, and pending_remap.json
    // replays it on the next startup.
    if (owns_restore_observers()) {
        const auto& saved = AmsState::instance().pending_tool_map_restore();
        spdlog::warn("[PrintStartController] Destroyed with pending remap restore "
                     "({} tools on backend {}) — this controller will not restore it",
                     saved.mapping.size(), saved.backend_index);
    }

    // Clear observers before modal cleanup. All four must be reset here, not
    // just the print-state one: each holds a context pointing at this object,
    // and the klippy/AMS guards stay armed across a whole deferred-restore wait
    // (ObserverGuard, never release() — #579).
    print_state_observer_.reset();
    klippy_state_observer_.reset();
    ams_data_observer_.reset();
    backend_retry_observer_.reset();

    // Clean up an open gate dialog - only if LVGL is still initialized
    // (destructor may be called after lv_deinit() during shutdown)
    if (lv_is_initialized()) {
        if (print_gate_modal_) {
            helix::ui::modal_hide(print_gate_modal_);
            print_gate_modal_ = nullptr;
        }
    }
    spdlog::trace("[PrintStartController] Destroyed");
}

// ============================================================================
// Setup
// ============================================================================

void PrintStartController::set_api(IMoonrakerAPI* api) {
    api_ = api;
}

void PrintStartController::set_detail_view(PrintSelectDetailView* detail_view) {
    detail_view_ = detail_view;
}

void PrintStartController::set_prep_manager(PrintPreparationManager* prep) {
    prep_manager_ = prep;
}

void PrintStartController::set_file_facts(helix::FileStartFacts facts) {
    file_facts_ = std::move(facts);
}

PrintPreparationManager* PrintStartController::active_prep_manager() const {
    return detail_view_ ? detail_view_->get_prep_manager() : prep_manager_;
}

void PrintStartController::end_without_print() {
    if (update_print_button_) {
        update_print_button_();
    }
    if (on_print_cancelled_) {
        on_print_cancelled_();
    }
}

void PrintStartController::set_file(const std::string& filename, const std::string& path,
                                    const std::vector<std::string>& filament_colors,
                                    const std::string& thumbnail_path) {
    filename_ = filename;
    path_ = path;
    filament_colors_ = filament_colors;
    thumbnail_path_ = thumbnail_path;
    scan_key_.clear();
}

std::string PrintStartController::file_scan_key() const {
    return scan_key_.empty() ? PrintPreparationManager::scan_key(filename_, path_) : scan_key_;
}

bool PrintStartController::is_ready() const {
    return !filename_.empty() && (detail_view_ != nullptr || prep_manager_ != nullptr);
}

// ============================================================================
// Print Initiation
// ============================================================================

void PrintStartController::initiate() {
    // Safety: reject print starts during startup grace period.
    // Ghost touch events during app initialization can trigger the full print chain
    // (file select → detail view → print button → filament warning confirm) in seconds.
    auto elapsed = std::chrono::steady_clock::now() - app_constants::startup::kProcessStartTime;
    if (elapsed < app_constants::startup::kPrintStartGracePeriod) {
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        spdlog::warn("[PrintStartController] Rejected print start during startup grace period "
                     "({}s < {}s)",
                     secs, app_constants::startup::kPrintStartGracePeriod.count());
        end_without_print();
        return;
    }

    if (divert_to_spool_removal() || refuse_while_filament_op_runs()) {
        end_without_print();
        return;
    }

    // OPTIMISTIC UI: Disable button IMMEDIATELY to prevent double-clicks.
    // This must happen BEFORE any async work or checks that could allow
    // the user to click again while we're processing.
    if (can_print_subject_) {
        lv_subject_set_int(can_print_subject_, 0);
    }

    // Check if a print is already active before allowing a new one to start
    if (!printer_state_.print_state().can_start_new_print()) {
        // RAW_PRINT_STATE_OK: the preparing axis is carried separately here by
        // can_start_new_print(); this arm asks only what the printer reports.
        PrintJobState current_state = printer_state_.print_state().get_print_job_state();
        const char* state_str = print_job_state_to_string(current_state);
        NOTIFY_ERROR(lv_tr("Cannot start print: printer is {}"), state_str);
        spdlog::warn("[PrintStartController] Attempted to start print while printer is in {} state",
                     state_str);
        end_without_print();
        return;
    }

    // Ordered pre-print gate pipeline: insufficient spool weight, bypass
    // engaged on a lane print, unaccounted toolhead filament, required
    // filament present, unresolved tools, material compatibility. Each
    // warning dialog drives its own continuation (proceed resumes at the next
    // gate with a freshly gathered context).
    run_gates_from(0);
}

void PrintStartController::execute_print_start() {
    // OPTIMISTIC UI: Disable button immediately to prevent double-clicks
    if (can_print_subject_) {
        lv_subject_set_int(can_print_subject_, 0);
    }

    auto* prep_manager = active_prep_manager();
    if (!prep_manager) {
        spdlog::error("[PrintStartController] Cannot start print - prep manager not initialized");
        NOTIFY_ERROR(lv_tr("Cannot start print: internal error"));
        end_without_print();
        return;
    }

    std::string filename_to_print = filename_;

    // Read the selected pre-print options for the start log below. Applying
    // them (timelapse included) is owned entirely by prep_manager->start_print().
    auto options = prep_manager->read_options_from_subjects();

    spdlog::info(
        "[PrintStartController] Starting print: {} (pre-print: mesh={}, qgl={}, z_tilt={}, "
        "clean={}, timelapse={})",
        filename_to_print, options.bed_mesh, options.qgl, options.z_tilt, options.nozzle_clean,
        options.timelapse);

    // Apply filament remaps if user changed any mappings. True whenever this
    // print's end owes the printer its own map back.
    bool restore_owed = apply_filament_remaps();
    if (restore_owed) {
        observe_lifecycle_for_restore();
    }

    // Timelapse application now lives SOLELY in
    // PrintPreparationManager::start_print(), which dispatches the timelapse
    // RuntimeCommand in BOTH directions (on/off). The redundant enable-only
    // write that used to live here caused a double write on print start (#1094).

    // Capture callbacks for use in lambdas
    auto on_started = on_print_started_;
    auto on_cancelled = on_print_cancelled_;
    auto update_button = update_print_button_;
    auto show_detail = show_detail_view_;

    // Capture thumbnail path for lambda
    std::string thumbnail_path = thumbnail_path_;
    std::vector<std::string> exclude_picks = std::move(exclude_picks_);
    exclude_picks_.clear();

    // The actual "start the print" step: navigate to the status panel, then
    // delegate to PrintPreparationManager. Hoisted into a continuation so the
    // Snapmaker U1 native pre-print config send (which must land BEFORE
    // PRINT_START) can run first and only invoke this on success.
    // PrinterState outlives the controller, so a bare pointer threaded through
    // the background-thread completion chain cannot dangle.
    PrinterState* ps = &printer_state_;

    auto start_now = [this, ps, prep_manager, filename_to_print, path = path_, thumbnail_path,
                      on_started, on_cancelled, update_button, show_detail, exclude_picks,
                      scan_key = file_scan_key()]() {
        // Navigate to print status panel IMMEDIATELY (optimistic navigation)
        // The busy overlay will show on top during download/upload operations.
        // On failure, we'll navigate back to the detail overlay.
        // Arm the preparing window BEFORE any pre-start work. From here the
        // panel answers "which job is this?" from the preparing job rather than
        // from print_stats, which still describes the PREVIOUS job for the whole
        // duration of a host-side pre-start block.
        printer_state_.print_state().begin_preparing(
            helix::PrintJobRef{filename_to_print, path, ""});

        if (navigate_to_print_status_) {
            spdlog::info("[PrintStartController] Navigating to print status panel (preparing...)");
            if (hide_detail_view_) {
                hide_detail_view_();
            }
            navigate_to_print_status_();
        }

        // Delegate to PrintPreparationManager
        prep_manager->start_print(
            filename_to_print, path,
            // Navigation callback - called when Moonraker confirms print start
            // Sets thumbnail source so PrintStatusPanel loads the correct thumbnail
            // NOTE: Called from background HTTP thread - must defer LVGL calls to main thread
            // The object picks follow it: Klipper resets exclude_object as a print starts.
            helix::ui::with_pre_start_exclusions(
                [filename_to_print, path, thumbnail_path, on_started]() {
                    // Construct full path for metadata lookup (e.g., usb/flowrate_0.gcode)
                    std::string full_path = gcode::join_gcode_path(path, filename_to_print);
                    helix::ui::queue_update(
                        "PrintStartController::execute_print_start",
                        [full_path, thumbnail_path, on_started]() {
                            // begin_preparing() already recorded this job's identity, but a
                            // start that never opened a preparing window still needs it, and
                            // re-stating it is idempotent.
                            get_printer_state().print_state().set_print_identity_override(
                                full_path);

                            // If we have a pre-extracted thumbnail (USB/embedded), set it directly
                            // This bypasses Moonraker metadata lookup which doesn't have USB file
                            // info
                            if (!thumbnail_path.empty()) {
                                // Tag it with the same identity handed to
                                // set_thumbnail_source() above: the manager cannot infer
                                // it here, since Moonraker may not have reported the
                                // print filename yet.
                                helix::get_active_print_media_manager().set_thumbnail_path(
                                    full_path, thumbnail_path);
                                spdlog::debug(
                                    "[PrintStartController] Set extracted thumbnail path: {}",
                                    thumbnail_path);
                            }

                            spdlog::debug("[PrintStartController] Print start confirmed, thumbnail "
                                          "source set: {}",
                                          full_path);
                            if (on_started) {
                                on_started();
                            }
                        });
                },
                exclude_picks),
            // Completion callback
            // NOTE: Called from background HTTP thread - must defer LVGL calls to main thread
            [update_button, show_detail, on_cancelled, ps](bool success, const std::string& error) {
                helix::ui::queue_update(
                    "PrintStartController::execute_print_start",
                    [success, error, update_button, show_detail, on_cancelled, ps]() {
                        // Not end_without_print(): that runs before any navigation, and
                        // refreshes the button and reports the cancel together. Here every
                        // failure reports the cancel, an abandoned start (no error) included,
                        // while the button refresh waits for a failure with an error to
                        // unwind the navigation and re-show the file view it belongs to.
                        if (!success && on_cancelled) {
                            on_cancelled();
                        }
                        if (success) {
                            // Deliberately does NOT end preparation: the RPC
                            // succeeding is when PRINT_START BEGINS. The printer's
                            // own PRINTING report retires the claim (Confirmed).
                            spdlog::debug("[PrintStartController] Print started successfully");
                        } else if (!error.empty()) {
                            NOTIFY_ERROR(lv_tr("Print preparation failed: {}"), error);
                            LOG_ERROR_INTERNAL(
                                "[PrintStartController] Print preparation failed: {}", error);
                            ps->print_state().retire_preparing(helix::PreparingExit::Failed);

                            // Only unwind the optimistic navigation if the print
                            // status overlay is still what the user is looking at.
                            // Preparation can fail a full minute after we pushed it
                            // (upload/prep timeout), by which point they may have
                            // navigated several overlays on — popping blind then
                            // closes THEIR screen, and the re-show below rebuilds
                            // widgets that same pop just destroyed (#1221).
                            // get_cached_overlay() is the widget push_overlay()
                            // actually put on the stack, and it goes null with
                            // destroy-on-close — the same handle the auto-nav gate
                            // checks membership with (print_start_navigation.cpp).
                            if (helix::nav::is_on_top(PrintStatusPanel::get_cached_overlay())) {
                                spdlog::info(
                                    "[PrintStartController] Navigating back to print select "
                                    "after failure");
                                helix::nav::go_back(); // Pop print status overlay

                                // Re-show the detail view so user can retry
                                if (show_detail) {
                                    show_detail();
                                }
                            } else {
                                spdlog::info(
                                    "[PrintStartController] Print status no longer on top — "
                                    "leaving navigation alone after failure");
                            }

                            // Re-enable button on failure
                            if (update_button) {
                                update_button();
                            }
                        }
                    });
            },
            scan_key);
    };

    // Some backends (Snapmaker U1) must emit firmware-native print_task_config
    // gcode BEFORE the print starts (it errors mid-print). ALWAYS-ON for U1 —
    // even with no remap — because SET_PRINT_USED_EXTRUDERS suppresses a
    // spurious-feed runout (the slicer auto-feeds heads the print doesn't use →
    // empty head → runout cancel). Only backends that advertise
    // requires_preprint_send() trigger the pre-send; every other backend takes
    // the unchanged synchronous start path below.
    if (AmsBackend* backend = AmsState::instance().get_backend();
        backend && backend->requires_preprint_send()) {
        const auto file = gather_print_start_context();
        send_backend_preprint_then(file.tools_used, file.effective_remap, start_now,
                                   [this]() { end_without_print(); });
        return; // start_now fires from the send continuation — do NOT also start here
    }

    start_now();
}

void PrintStartController::send_backend_preprint_then(const std::set<int>& tools_used,
                                                      const std::map<int, int>& remap,
                                                      helix::SharedCallback<void()> on_done,
                                                      helix::SharedCallback<void()> on_abort,
                                                      bool routed) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        // No backend — nothing to pre-send; proceed with the start step.
        if (on_done) {
            on_done();
        }
        return;
    }
    // The backend hears of the start as it is dispatched, after the pre-print
    // step went through.
    on_done = [on_done = std::move(on_done)]() {
        if (AmsBackend* b = AmsState::instance().get_backend()) {
            b->note_print_start_from_here();
        }
        if (on_done) {
            on_done();
        }
    };

    // A remap the backend cannot honor stops the print here, before anything
    // is sent, with the same report a printer-side refusal gets below.
    if (const AmsError refused = backend->check_preprint_remap(tools_used, remap);
        !refused.success()) {
        LOG_ERROR_INTERNAL("[PrintStartController] pre-print remap refused: {}",
                           refused.technical_msg);
        report_preprint_failure(refused.user_msg);
        if (on_abort) {
            on_abort();
        }
        return;
    }

    std::string gcode = routed ? backend->build_preprint_gcode(tools_used, remap)
                               : backend->build_unrouted_preprint_gcode(tools_used);

    if (gcode.empty()) {
        // Nothing the firmware needs (e.g. no parsed tools) — start immediately.
        spdlog::debug("[PrintStartController] pre-print config empty — starting print directly");
        if (on_done) {
            on_done();
        }
        return;
    }

    spdlog::info("[PrintStartController] pre-print config:\n{}", gcode);

    if (!api_) {
        spdlog::error("[PrintStartController] No API to send pre-print config — aborting print");
        NOTIFY_ERROR(lv_tr("Print setup failed: internal error"));
        if (on_abort) {
            on_abort();
        }
        return;
    }

    auto tok = lifetime_.token();
    api_->execute_gcode(
        gcode,
        // Success: defer to main thread, then run the real start step.
        [tok, on_done]() mutable {
            tok.defer("PrintStartController::preprint.ok", [on_done]() {
                if (on_done) {
                    on_done();
                }
            });
        },
        // Error: defer to main thread, surface a user-visible failure, and ABORT.
        // Better to fail loud than feed an empty head.
        [tok, on_abort](const MoonrakerError& err) mutable {
            std::string msg = err.message;
            // A refusal HelixScreen raised itself (e.g. spools on the bed) never
            // reached the printer, so it shows its own reason.
            std::string local = err.message_tag ? err.localized_message() : std::string{};
            tok.defer("PrintStartController::preprint.err", [msg, local, on_abort]() {
                LOG_ERROR_INTERNAL("[PrintStartController] pre-print config rejected: {}", msg);
                report_preprint_failure(local);
                // Notify caller to re-enable UI state — do NOT start.
                if (on_abort) {
                    on_abort();
                }
            });
        },
        15000);
}

void PrintStartController::read_file_tools(const std::string& filename,
                                           helix::SharedCallback<void(std::set<int>)> on_tools,
                                           helix::FileRead read) {
    if (!reprint_facts_) {
        reprint_facts_ = std::make_unique<helix::FileStartFactsLoader>(api_);
    }
    reprint_facts_->set_api(api_);
    reprint_facts_->load(
        filename,
        [on_tools = std::move(on_tools)](helix::FileStartFacts facts) {
            std::set<int> tools = helix::file_tools_used(facts);
            spdlog::info("[PrintStartController] {} tools read from {}", tools.size(),
                         facts.filename);
            on_tools(std::move(tools));
        },
        nullptr, nullptr, read);
}

void PrintStartController::report_preprint_failure(const std::string& reason) {
    NOTIFY_ERROR_MODAL(lv_tr("Print setup failed"), "{}",
                       !reason.empty()
                           ? reason
                           : std::string(lv_tr("The printer rejected the filament configuration. "
                                               "The print was not started.")));
}

bool PrintStartController::refuse_while_filament_op_runs() {
    if (!AmsState::instance().any_operation_running_here()) {
        return false;
    }
    spdlog::warn("[PrintStartController] Print start refused: a filament operation is running");
    NOTIFY_WARNING(lv_tr("Wait for the current filament operation to finish"));
    return true;
}

bool PrintStartController::divert_to_spool_removal() {
    if (!printer_state_.print_state().spool_latch_active()) {
        return false;
    }
    spdlog::warn("[PrintStartController] Print start blocked: spools are latched on the bed");
    helix::ui::on_bed_drying_banner_clicked();
    return true;
}

void PrintStartController::initiate_reprint(const std::string& filename, const std::string& path,
                                            const std::set<int>& tools_used,
                                            helix::SharedCallback<void()> on_started,
                                            helix::SharedCallback<void()> on_error,
                                            int archive_plate) {
    reprint_impl(filename, path, tools_used, std::nullopt, std::move(on_started),
                 std::move(on_error), archive_plate);
}

void PrintStartController::initiate_reprint_with_routing(const std::string& filename,
                                                         const std::set<int>& tools_used,
                                                         const std::map<int, int>& routing,
                                                         helix::SharedCallback<void()> on_started,
                                                         helix::SharedCallback<void()> on_error) {
    reprint_impl(filename, "", tools_used, routing, std::move(on_started), std::move(on_error), 0);
}

void PrintStartController::reprint_impl(const std::string& filename, const std::string& path,
                                        const std::set<int>& tools_used,
                                        const std::optional<std::map<int, int>>& routing,
                                        helix::SharedCallback<void()> on_started,
                                        helix::SharedCallback<void()> on_error, int archive_plate) {
    // path is used for the preparing-job identity; the start RPC takes the bare name.

    if (!api_) {
        spdlog::error("[PrintStartController] initiate_reprint: no API");
        if (on_error) {
            on_error();
        }
        return;
    }

    // A plate of an archive prints only extracted, which takes the preparation
    // manager; started as itself, the printer would print the .3mf instead.
    if (archive_plate > 0 && !active_prep_manager()) {
        spdlog::warn("[PrintStartController] Not reprinting plate {} of {}: nothing to extract it",
                     archive_plate, filename);
        NOTIFY_WARNING(lv_tr("Open the .3mf in Print Files to print this plate again."));
        if (on_error) {
            on_error();
        }
        return;
    }

    if (divert_to_spool_removal() || refuse_while_filament_op_runs()) {
        if (on_error) {
            on_error();
        }
        return;
    }

    helix::warn_printer_stop_check_skipped("a reprint", filename,
                                           "a reprint does not scan the file");

    // The reprint path skips the preparation manager, so it records the job
    // identity itself rather than running with the previous print's override.
    printer_state_.print_state().begin_preparing(helix::PrintJobRef{filename, path, ""});
    const int epoch =
        lv_subject_get_int(printer_state_.print_state().get_preparing_epoch_subject());

    // Lightweight start — the file is already on the printer; no upload/prep.
    // An archive printed as an extracted plate cannot start as itself: it goes
    // the way Print sends it, extracted again through the preparation manager.
    auto start = [this, filename, on_started, on_error, archive_plate, epoch]() {
        // The pre-print send runs first, and Stop can withdraw the job while it
        // does; a withdrawn job must not reach the printer.
        if (lv_subject_get_int(printer_state_.print_state().get_preparing_epoch_subject()) !=
            epoch) {
            spdlog::info("[PrintStartController] Reprint of {} abandoned - no longer being "
                         "prepared",
                         filename);
            if (on_error) {
                on_error();
            }
            return;
        }
        auto tok = lifetime_.token();
        if (archive_plate > 0) {
            auto* prep = active_prep_manager();
            if (!prep) {
                printer_state_.print_state().retire_preparing(helix::PreparingExit::Failed);
                NOTIFY_WARNING(lv_tr("Open the .3mf in Print Files to print this plate again."));
                if (on_error) {
                    on_error();
                }
                return;
            }
            prep->start_archive_reprint(
                filename, archive_plate,
                [tok, on_started, on_error](bool ok, const std::string&) mutable {
                    tok.defer("PrintStartController::archive_reprint.done",
                              [ok, on_started, on_error]() {
                                  if (ok && on_started) {
                                      on_started();
                                  } else if (!ok && on_error) {
                                      on_error();
                                  }
                              });
                });
            return;
        }
        printer_state_.print_state().note_preparing_start_sent();
        api_->job().start_print(
            filename,
            [tok, on_started, ps = &printer_state_, epoch]() mutable {
                // Recorded even if this controller is gone: a pending Stop is
                // waiting on it to send its cancel.
                helix::ui::queue_update("PrintStartController::reprint.answered", [ps, epoch]() {
                    ps->print_state().note_preparing_start_answered(epoch, true);
                });
                tok.defer("PrintStartController::reprint.ok", [on_started]() {
                    if (on_started) {
                        on_started();
                    }
                });
            },
            [tok, on_error, ps = &printer_state_, epoch](const MoonrakerError& err) mutable {
                tok.defer("PrintStartController::reprint.err", [err, on_error, ps, epoch]() {
                    // A lost answer: the printer may be running this start,
                    // so only its own report settles the job.
                    if (ps->print_state().hold_unanswered_start(epoch, err)) {
                        return;
                    }
                    ps->print_state().retire_preparing(helix::PreparingExit::Failed);
                    NOTIFY_ERROR(lv_tr("Failed to reprint: {}"), err.localized_message());
                    if (on_error) {
                        on_error();
                    }
                });
            });
    };

    // Backends that need a pre-print send (Snapmaker U1) emit the firmware-native
    // print_task_config gcode BEFORE the reprint starts, same as the normal start
    // path. On native-send error the modal is shown and on_error runs.
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend && backend->requires_preprint_send()) {
        auto preprint = [this, routing, start, on_error](const std::set<int>& tools_used) {
            // Read again here: the file read below answers later, and the
            // backend may have been replaced meanwhile.
            AmsBackend* backend = AmsState::instance().get_backend();
            if (!backend || !backend->requires_preprint_send()) {
                start();
                return;
            }
            // Where the reprint's routing comes from. The normal start path reads
            // the detail view's colour/type match; a reprint has no detail view, no
            // swatch card and no picker, so the only thing that knows how this job
            // was routed is the printer, and only while its task was configured,
            // which is what last_print_tool_mapping() snapshots. An empty remap is
            // not "no routing": build_preprint_gcode resolves a tool it is not told
            // about to that tool's firmware-default head.
            auto remap = routing ? routing
                                 : helix::FilamentMapper::reprint_remap(
                                       backend->last_print_tool_mapping(), tools_used);
            if (!remap && backend->default_print_routing_is_identity()) {
                // The firmware's own default is tool n = slot n, so the empty remap
                // sends exactly that for the file's tools and overwrites any map the
                // printer still holds for its next print.
                remap = std::map<int, int>{};
            } else if (!remap) {
                // Not known (no configured task observed, e.g. the app started after
                // the print ended). Sending a map here means inventing a routing: an
                // identity map is a confident wrong answer for exactly the files that
                // need one, so none is written. A firmware whose routing returns to
                // identity at each print end still runs the file's tools at their own
                // slots, and can be told which those are; any other gets only the
                // routing-free per-print preferences.
                spdlog::warn("[PrintStartController] Reprint: no recorded print routing; sending "
                             "no extruder map rather than writing a default one over it");
            }

            // The abort path has to retire the job this function armed above.
            // execute_print_start() avoids the problem by arming INSIDE its
            // success-only lambda; reprint arms before the pre-send because the
            // pre-send is itself part of the window. Handing on_error straight
            // through would leave the job armed on a rejected pre-send, and
            // print_in_progress is published from it - so can_start_new_print()
            // would refuse every later print until the watchdog fired.
            auto abort = [this, on_error]() {
                printer_state_.print_state().retire_preparing(helix::PreparingExit::Failed);
                if (on_error) {
                    on_error();
                }
            };
            if (!remap && backend->print_routing_resets_to_identity()) {
                send_backend_preprint_then(tools_used, {}, start, abort, /*routed=*/false);
                return;
            }
            // With the routing unknown only the routing-free part goes out (an
            // empty tool set): the backend's per-print preferences.
            send_backend_preprint_then(remap ? tools_used : std::set<int>{},
                                       remap ? *remap : std::map<int, int>{}, start, abort);
        };
        // The pre-print step tells the firmware which heads to feed and unload,
        // so a reprint with no tools in hand reads them from the file first, the
        // same reading the slicer-start check uses. A file that answers nothing
        // keeps the routing-free step.
        if (tools_used.empty() && archive_plate == 0) {
            read_file_tools(filename, preprint);
        } else {
            preprint(tools_used);
        }
        return; // start fires from the send continuation — do NOT also start here
    }

    start();
}

// ============================================================================
// Gate Pipeline
// ============================================================================

helix::PrintStartContext PrintStartController::gather_print_start_context() const {
    helix::PrintStartContext ctx;
    ctx.filament_color_count = filament_colors_.size();

    auto& ams = AmsState::instance();
    ctx.ams_available = ams.is_available();
    ctx.any_bypass_active = ams.any_bypass_active();
    ctx.lanes_picked_at_print = ams.collect_firmware_routing().chosen_at_print;
    ctx.external_spool = ams.get_external_spool_info();
    ctx.filament_diameter_mm = printer_state_.get_discovery().filament_diameter_mm();
    ctx.has_active_backend = ams.get_backend() != nullptr;
    ctx.multiple_toolheads = helix::ToolState::instance().has_multiple_toolheads();
    for (int i = 0; i < ams.backend_count(); ++i) {
        if (auto* backend = ams.get_backend(i)) {
            // One presenting no filament system feeds the external spool alone,
            // so the printer's own sensors answer for it, not its lanes.
            if (backend->get_system_info().presents_filament_system()) {
                ctx.ams_manages_filament = true;
            }
            if (backend->auto_unloads_after_print()) {
                ctx.any_auto_unload_backend = true;
            }
            ctx.toolhead_unaccounted.push_back(backend->toolhead_filament_unaccounted());
            ctx.toolhead_clearable.push_back(backend->can_clear_unaccounted_toolhead());
        } else {
            ctx.toolhead_unaccounted.push_back(std::nullopt);
            ctx.toolhead_clearable.push_back(false);
        }
    }

    if (detail_view_) {
        ctx.has_file_facts = true;
        ctx.metadata = detail_view_->get_file_metadata();
        ctx.mappings = detail_view_->get_filament_mappings();
        ctx.tool_info = detail_view_->get_filament_tool_info();
        ctx.available_slots = detail_view_->get_available_slots();
        ctx.filament_materials = detail_view_->get_filament_materials();
        ctx.tools_used = detail_view_->get_tools_used();
        ctx.effective_remap = detail_view_->get_effective_remap();
        ctx.tool_grams = detail_view_->get_tool_grams();
        ctx.fit_file = detail_view_->get_fit_file();
    } else if (file_facts_) {
        helix::fill_file_context(ctx, *file_facts_);
    }
    ctx.fit_machine = helix::current_fit_machine();
    if (!ctx.has_file_facts) {
        ctx.printer_stop.not_run_reason = "no file detail view";
    } else if (auto* prep = active_prep_manager()) {
        ctx.printer_stop = prep->printer_stop_check_for(file_scan_key());
    } else {
        ctx.printer_stop.not_run_reason = "no file scanner";
    }
    if (ctx.ams_manages_filament && ctx.has_active_backend) {
        ctx.empty_required_lanes =
            helix::FilamentSensorManager::instance().find_empty_required_lanes(ctx.tools_used,
                                                                               ctx.effective_remap);
    }

    auto& sensors = helix::FilamentSensorManager::instance();
    ctx.runout_enabled = sensors.is_master_enabled();
    ctx.runout_available = sensors.is_sensor_available(helix::FilamentSensorRole::RUNOUT);
    ctx.runout_detected = sensors.is_filament_detected(helix::FilamentSensorRole::RUNOUT);

    auto& inputs = helix::sensors::InputSensorManager::instance();
    ctx.door_check_enabled = inputs.door_check_enabled();
    ctx.open_doors = inputs.open_doors();
    return ctx;
}

void PrintStartController::run_gates_from(size_t index) {
    const auto ctx = gather_print_start_context();
    // A start with no detail view has no screen that read the file, so an
    // unrun printer-stopping command check ends it, at every (re-)entry: the
    // macros can be read again while a dialog waits.
    if (!detail_view_ && file_facts_ &&
        ctx.printer_stop.state == helix::PrinterStopCheck::State::NotRun) {
        spdlog::warn("[PrintStartController] Not starting {}: printer-stopping command check "
                     "did not run ({})",
                     filename_, ctx.printer_stop.not_run_reason);
        if (on_file_unchecked_) {
            on_file_unchecked_();
        }
        end_without_print();
        return;
    }
    if (index == 0 && ctx.printer_stop.state == helix::PrinterStopCheck::State::NotRun) {
        helix::warn_printer_stop_check_skipped(detail_view_ ? "the print select panel"
                                                            : "a queued job start",
                                               filename_, ctx.printer_stop.not_run_reason);
    }
    for (size_t i = index; i < gate_list_.size(); ++i) {
        const auto result = gate_list_[i].evaluate(ctx);
        if (result.verdict == helix::CheckResult::Verdict::Pass) {
            continue;
        }

        if (print_gate_modal_) {
            helix::ui::modal_hide(print_gate_modal_);
            print_gate_modal_ = nullptr;
        }
        gate_resume_index_ = i;
        // The modal closes itself on a button press and deletes itself after
        // any close. Every close that is not Proceed resolves like Cancel: the
        // print button comes back and nothing starts, including a backdrop tap
        // or ESC.
        auto cancel = [this] { on_gate_cancel(); };

        if (result.verdict == helix::CheckResult::Verdict::Block) {
            // A hard stop explains itself and offers only OK.
            spdlog::warn("[PrintStartController] Gate '{}' blocked the print start",
                         gate_list_[i].name);
            helix::ui::AlertOptions alert_opts;
            alert_opts.on_dismiss = cancel;
            alert_opts.owner_token = lifetime_.token();
            print_gate_modal_ =
                helix::ui::modal_alert(result.title.c_str(), result.body.c_str(),
                                       ModalSeverity::Error, lv_tr("OK"), cancel, alert_opts);
            if (!print_gate_modal_) {
                spdlog::error("[PrintStartController] Failed to create gate dialog for '{}'",
                              gate_list_[i].name);
                end_without_print();
            }
            return;
        }

        helix::ui::ConfirmOptions opts;
        opts.on_cancel = cancel;
        opts.on_dismiss = cancel;
        opts.owner_token = lifetime_.token();
        print_gate_modal_ = helix::ui::modal_confirm(
            result.title.c_str(), result.body.c_str(),
            result.severity == helix::GateSeverity::Error  ? ModalSeverity::Error
            : result.severity == helix::GateSeverity::Info ? ModalSeverity::Info
                                                           : ModalSeverity::Warning,
            result.proceed_label.c_str(), [this] { on_gate_proceed(); }, opts);
        if (!print_gate_modal_) {
            spdlog::error("[PrintStartController] Failed to create gate dialog for '{}'",
                          gate_list_[i].name);
            end_without_print();
            return;
        }
        spdlog::debug("[PrintStartController] Gate '{}' warned - showing dialog",
                      gate_list_[i].name);
        return; // the dialog drives continuation
    }
    execute_print_start();
}

void PrintStartController::on_gate_proceed() {
    print_gate_modal_ = nullptr;
    // The answered dialog hides itself only after this callback returns, and
    // the next dialog's blurred backdrop captures whatever is on screen when it
    // opens. Running the next gate on the following tick keeps the answered
    // dialog out of that capture.
    lifetime_.defer("PrintStartController::next_gate",
                    [this]() { run_gates_from(gate_resume_index_ + 1); });
}

void PrintStartController::on_gate_cancel() {
    print_gate_modal_ = nullptr;
    end_without_print();
    spdlog::debug("[PrintStartController] Print cancelled by user (gate: {})",
                  gate_list_[gate_resume_index_].name);
}

// ============================================================================
// Filament Remap Lifecycle
// ============================================================================

bool PrintStartController::should_warn_remap_unsupported(const AmsBackend& backend) {
    // A backend that applies the remap via its firmware-native pre-print path
    // (Snapmaker U1) honors the user's choice through build_preprint_gcode()
    // even though it writes no persistent table — so the toast would be a stale
    // false alarm. Only warn when the backend has no route at all, or has one it
    // cannot use yet.
    //
    // Which is exactly the shared rule, called rather than restated:
    // AmsState::effective_auto_match() asks the same question to decide whether
    // the auto-color toggle is a live control, and a second copy of the test
    // would drift into a printer that warns the remap is unsupported while still
    // honoring it (or the reverse).
    return !helix::printer::can_remap(backend);
}

namespace {
// Only a lane the user picked, or one auto color matching chose, asks the
// printer to change. Every other entry is a seed that means "follow the
// printer", and a seed taken before the printer's table last moved would
// overwrite it. A free-lane fallback is a guess with no colour behind it, so it
// never overwrites the printer's table either (prestonbrown/helixscreen#1862).
bool sends_remap(const helix::ToolMapping& m) {
    return !m.is_auto && m.mapped_slot >= 0 &&
           (m.user_picked || m.reason == helix::ToolMapping::MatchReason::COLOR_MATCH);
}

// True when @p live already holds every lane @p saved restores. A tool unmapped
// before the print (-1) has no lane to go back to and is never sent, so it
// matches whatever the live table says.
bool restore_landed(const std::vector<int>& saved, const std::vector<int>& live) {
    for (size_t i = 0; i < saved.size(); ++i) {
        if (saved[i] >= 0 && (i >= live.size() || live[i] != saved[i])) {
            return false;
        }
    }
    return true;
}
} // namespace

bool PrintStartController::apply_filament_remaps() {
    const auto mappings = gather_print_start_context().mappings;
    if (mappings.empty()) {
        return false;
    }

    auto& ams = AmsState::instance();
    // A restore still pending from an earlier print means the backend's live
    // table holds that print's map. The seed showed this print's un-picked rows
    // on the printer's own lanes (AmsState::collect_firmware_routing()), so
    // those rows are put back on them; the rest of the record waits for this
    // print's end like any other restore.
    // Copied: the record is replaced below while the send loop still asks it.
    PendingToolMapRestore pending = ams.pending_tool_map_restore();
    const auto lane_for = [&pending](const helix::ToolMapping& m) {
        if (sends_remap(m)) {
            return m.mapped_slot;
        }
        if (!pending.empty() && !m.is_auto && m.mapped_slot >= 0 &&
            m.mapped_backend == pending.backend_index && m.tool_index >= 0 &&
            m.tool_index < static_cast<int>(pending.mapping.size()) &&
            pending.mapping[static_cast<size_t>(m.tool_index)] == m.mapped_slot) {
            return m.mapped_slot;
        }
        return -1;
    };

    // A pending record goes on even when nothing is asked for: this print's end
    // owes it a restore, and only the code below arms one.
    if (pending.empty() &&
        std::none_of(mappings.begin(), mappings.end(),
                     [&lane_for](const helix::ToolMapping& m) { return lane_for(m) >= 0; })) {
        spdlog::debug("[PrintStartController] No mapping asks for a remap — the printer's "
                      "table stands");
        return false;
    }

    if (!ams.is_available()) {
        spdlog::warn("[PrintStartController] AMS not available for remapping");
        return false;
    }

    // Use the backend of the first mapping that asks for a lane; with none,
    // the pending record's, so a print that remaps nothing never drops it.
    // Multi-backend remap is not yet supported — warn if mappings span backends.
    int backend_idx = pending.empty() ? 0 : pending.backend_index;
    bool backend_set = false;
    for (const auto& m : mappings) {
        if (m.mapped_backend >= 0 && lane_for(m) >= 0) {
            if (!backend_set) {
                backend_idx = m.mapped_backend;
                backend_set = true;
            } else if (m.mapped_backend != backend_idx) {
                spdlog::warn("[PrintStartController] Mappings span multiple backends "
                             "({} and {}) — only backend {} will be remapped",
                             backend_idx, m.mapped_backend, backend_idx);
                break;
            }
        }
    }

    auto* backend = ams.get_backend(backend_idx);
    if (!backend) {
        spdlog::warn("[PrintStartController] Backend {} not found", backend_idx);
        return false;
    }

    // Two routes honor the user's choice without a mapping table to write, and
    // both must skip the generic set_tool_mapping() path SILENTLY:
    //   - a firmware-native pre-print send (Snapmaker U1: requires_preprint_send
    //     → build_preprint_gcode emits SET_PRINT_EXTRUDER_MAP /
    //     SET_PRINT_USED_EXTRUDERS), fired later from execute_print_start
    //   - GcodeRewrite, which rewrites the job file instead of moving a tool
    //     number in firmware
    // Only warn when the backend can do NONE of the three.
    if (!helix::printer::can_write_mapping_table(*backend)) {
        if (should_warn_remap_unsupported(*backend)) {
            spdlog::warn("[PrintStartController] Backend (idx={}) does not support editable tool "
                         "mapping — {} explicit remap(s) will be ignored",
                         backend_idx, mappings.size());
            NOTIFY_WARNING("{}",
                           lv_tr("Filament Mapping isn't supported on this printer, so the print "
                                 "uses the tools assigned on the printer"));
        } else {
            spdlog::debug("[PrintStartController] Backend (idx={}) applies remap via its "
                          "pre-print path — skipping generic remap send (no warning)",
                          backend_idx);
        }
        return false;
    }

    // A record for another backend, or one this backend's type no longer
    // matches, does not describe the table about to be written.
    if (!pending.empty() &&
        (pending.backend_index != backend_idx || !pending.matches_type(backend->get_type()))) {
        spdlog::warn("[PrintStartController] Dropping the pending restore for backend {} ({}): "
                     "this print remaps backend {} ({})",
                     pending.backend_index, ams_type_to_string(pending.backend_type), backend_idx,
                     ams_type_to_string(backend->get_type()));
        ams.clear_pending_tool_map_restore();
        clear_persisted_remap_state();
        pending = {};
    }

    // A retry or confirmation still armed for an earlier print would replay or
    // judge the printer's map against the remaps sent below.
    backend_retry_observer_.reset();
    klippy_state_observer_.reset();
    ams_data_observer_.reset();
    awaiting_restore_confirmation_ = false;

    const std::vector<int> current = backend->get_tool_mapping();
    const bool kept_pending = !ams.pending_tool_map_restore().empty();
    if (kept_pending) {
        ams.claim_pending_tool_map_restore();
        spdlog::info("[PrintStartController] Keeping the unrestored firmware mapping ({} tools) "
                     "from backend {} as this print's snapshot",
                     ams.pending_tool_map_restore().mapping.size(), backend_idx);
    } else {
        ams.set_pending_tool_map_restore(backend_idx, backend->get_type(), current);
        spdlog::info("[PrintStartController] Saved firmware mapping ({} tools) from backend {}",
                     current.size(), backend_idx);
    }

    // Send remap commands for changed mappings only.
    // Note: set_tool_mapping() is fire-and-forget (async G-code via WebSocket).
    // There is a small race window where the print could start before Klipper
    // processes all remap commands. In practice the window is negligible because
    // prep_manager->start_print() also sends an async request to Moonraker.
    int remaps_sent = 0;
    for (const auto& m : mappings) {
        const int lane = lane_for(m);
        if (lane < 0) {
            continue;
        }
        // A slot number names a lane only within its own backend.
        if ((m.mapped_backend < 0 ? 0 : m.mapped_backend) != backend_idx) {
            spdlog::warn("[PrintStartController] T{} is mapped to backend {}, not the remapped "
                         "backend {}: not sent",
                         m.tool_index, m.mapped_backend, backend_idx);
            continue;
        }

        // Check if this mapping differs from current firmware mapping
        int current_slot = -1;
        if (m.tool_index >= 0 && m.tool_index < static_cast<int>(current.size())) {
            current_slot = current[static_cast<size_t>(m.tool_index)];
        }

        if (lane == current_slot) {
            spdlog::debug("[PrintStartController] T{} already mapped to slot {} — skipping",
                          m.tool_index, lane);
            continue;
        }

        spdlog::info("[PrintStartController] Remapping T{}: slot {} -> slot {}", m.tool_index,
                     current_slot, lane);

        auto err = backend->set_tool_mapping(m.tool_index, lane);
        if (err.result != AmsResult::SUCCESS) {
            spdlog::error("[PrintStartController] Failed to remap T{}: {}", m.tool_index,
                          err.technical_msg);
            // Continue trying other remaps — partial success is better than none
        } else {
            ++remaps_sent;
        }
    }

    if (remaps_sent > 0) {
        spdlog::info("[PrintStartController] Sent {} remap command(s)", remaps_sent);
        persist_remap_state();
        return true;
    }

    // Nothing sent. A record this print found pending still describes the
    // printer's own map: it stays, and this print's end restores it.
    if (kept_pending) {
        if (restore_landed(ams.pending_tool_map_restore().mapping, current)) {
            spdlog::info("[PrintStartController] The firmware mapping already matches the "
                         "pending restore — nothing left to restore");
            finish_restore();
            return false;
        }
        persist_remap_state();
        return true;
    }
    ams.clear_pending_tool_map_restore();
    return false;
}

void PrintStartController::drop_unreported_entries(const AmsBackend& backend,
                                                   const std::vector<int>& current) {
    // A whole print has run without a lane the record names, so the printer
    // as it is now is that tool's map: its live lane becomes the entry. Only
    // once the backend has reported its lanes: before that, every lane reads
    // as absent.
    if (backend.get_system_info().total_slots <= 0) {
        return;
    }
    auto& ams = AmsState::instance();
    const std::vector<int> mapping = ams.pending_tool_map_restore().mapping;
    for (size_t i = 0; i < mapping.size(); ++i) {
        const int want = mapping[i];
        if (want < 0 || backend.can_set_tool_mapping(static_cast<int>(i), want).success()) {
            continue;
        }
        const int live = i < current.size() ? current[i] : -1;
        spdlog::info("[PrintStartController] T{}'s saved lane {} is still not reported after a "
                     "print — leaving it on lane {}",
                     i, want, live);
        ams.set_pending_tool_map_entry(static_cast<int>(i), live);
    }
}

void PrintStartController::observe_lifecycle_for_restore() {
    auto* subject = printer_state_.print_state().get_print_lifecycle_subject();
    if (!subject) {
        spdlog::warn("[PrintStartController] No print lifecycle subject, cannot auto-restore "
                     "mapping");
        return;
    }

    // The observer fires on attach, and registration happens BEFORE start_now
    // calls begin_preparing(), so even the derived lifecycle can still hold
    // the PREVIOUS job's terminal value (Moonraker leaves print_stats.state
    // terminal for the whole preparing window). Terminal states are ignored
    // until THIS print has been observed live: the latch arms on Preparing/
    // Printing/Paused, and the terminal value that follows can only be this
    // job's end (#1386).
    print_active_since_remap_ = false;
    restore_epoch_ = AmsState::instance().pending_tool_map_restore().epoch;
    print_state_observer_ = observe_print_lifecycle<PrintStartController>(
        subject, this,
        [](PrintStartController* self, PrintState state) {
            if (job_holds_machine(state)) {
                self->print_active_since_remap_ = true;
                return;
            }
            if (self->print_active_since_remap_ &&
                (state == PrintState::Complete || state == PrintState::Cancelled ||
                 state == PrintState::Error)) {
                self->print_active_since_remap_ = false;
                // Another controller's print start since this one armed owns
                // the record, and restores it at its own print's end.
                if (!self->still_owns_restore()) {
                    self->print_state_observer_.reset();
                    return;
                }
                self->restore_filament_mapping();
                self->print_state_observer_.reset();
            }
        },
        printer_state_.get_subjects_lifetime());

    spdlog::debug("[PrintStartController] Observing print lifecycle for mapping restore");
}

void PrintStartController::observe_klippy_state_for_restore() {
    if (klippy_state_observer_) {
        return; // Already waiting — a second deferral must not stack observers
    }

    auto* subject = printer_state_.network_state().get_klippy_state_subject();
    if (!subject) {
        spdlog::warn("[PrintStartController] No klippy state subject — deferred restore cannot "
                     "self-resolve; pending_remap.json will replay on next startup");
        return;
    }

    // Fires immediately with the current value, which is by definition not READY
    // here (we only get called from the not-ready branch), so the first fire is a
    // no-op. Without this the snapshot would sit until a full app restart, which
    // is the only other thing that replays pending_remap.json.
    restore_epoch_ = AmsState::instance().pending_tool_map_restore().epoch;
    klippy_state_observer_ = observe<int>(
        subject, this,
        [](PrintStartController* self, int state_val) {
            if (static_cast<KlippyState>(state_val) != KlippyState::READY) {
                return;
            }
            if (!self->still_owns_restore()) {
                self->klippy_state_observer_.reset();
                return;
            }
            spdlog::info(
                "[PrintStartController] Klipper ready — retrying deferred mapping restore");
            self->klippy_state_observer_.reset();
            self->restore_filament_mapping();
        },
        printer_state_.get_subjects_lifetime());

    spdlog::debug("[PrintStartController] Observing klippy state for deferred mapping restore");
}

void PrintStartController::restore_filament_mapping() {
    auto& ams = AmsState::instance();
    if (ams.pending_tool_map_restore().empty()) {
        return; // Nothing to restore
    }
    // Copied: the record is cleared below while this still reads it.
    PendingToolMapRestore saved = ams.pending_tool_map_restore();

    // A restore sent to a halted Klipper is refused, and set_tool_mapping()
    // cannot tell us: every native backend routes into
    // AmsSubscriptionBackend::execute_gcode(), which returns success
    // unconditionally and reports refusals only through an async log callback.
    // So the readiness check has to happen HERE, before we treat the snapshot as
    // spent. Getting this wrong stranded the printer on the print's mapping with
    // the recovery record deleted (#1270), and a halted Klipper at print end is
    // the normal shape of a cancelled or errored print — precisely when restore
    // runs.
    const auto klippy = static_cast<KlippyState>(
        lv_subject_get_int(printer_state_.network_state().get_klippy_state_subject()));
    if (klippy != KlippyState::READY) {
        spdlog::info("[PrintStartController] Klipper not ready (state={}) — deferring restore of "
                     "{} mapping(s); snapshot and pending_remap.json retained",
                     static_cast<int>(klippy), saved.mapping.size());
        observe_klippy_state_for_restore();
        return; // NOT delivered: keep the record and the persisted file
    }

    auto* backend = ams.get_backend(saved.backend_index);
    if (!backend) {
        spdlog::warn("[PrintStartController] Backend {} gone — cannot restore mapping",
                     saved.backend_index);
        ams.clear_pending_tool_map_restore();
        return;
    }
    if (!saved.matches_type(backend->get_type())) {
        spdlog::warn("[PrintStartController] Backend {} is now {}, not the {} its mapping was "
                     "saved from — dropping the restore",
                     saved.backend_index, ams_type_to_string(backend->get_type()),
                     ams_type_to_string(saved.backend_type));
        finish_restore();
        return;
    }
    // Only a table-writing route can take a record back. Without one every
    // entry is refused before it is sent, and the record would retry for good.
    if (backend->get_remap_strategy() != AmsBackend::RemapStrategy::Native) {
        spdlog::info("[PrintStartController] Backend {} has no mapping table to restore into — "
                     "dropping the restore",
                     saved.backend_index);
        finish_restore();
        return;
    }

    // Get current mapping to compare
    auto current_mapping = backend->get_tool_mapping();

    if (saved.outlived_a_print) {
        drop_unreported_entries(*backend, current_mapping);
        saved = ams.pending_tool_map_restore();
    }

    int restores_sent = 0;
    int restores_refused = 0;
    for (size_t i = 0; i < saved.mapping.size(); ++i) {
        int saved_slot = saved.mapping[i];
        int current_slot = (i < current_mapping.size()) ? current_mapping[i] : -1;

        // A tool that was unmapped before the print has no lane to go back to;
        // every backend refuses slot -1.
        if (saved_slot >= 0 && saved_slot != current_slot) {
            spdlog::info("[PrintStartController] Restoring T{}: slot {} -> slot {}", i,
                         current_slot, saved_slot);
            auto err = backend->set_tool_mapping(static_cast<int>(i), saved_slot);
            if (err.result != AmsResult::SUCCESS) {
                ++restores_refused;
                spdlog::error("[PrintStartController] Failed to restore T{}: {}", i,
                              err.technical_msg);
            } else {
                ++restores_sent;
            }
        }
    }

    spdlog::info("[PrintStartController] Sent {} restore command(s) on print end", restores_sent);

    // A refused entry names a lane the backend does not currently report —
    // typically a unit that detached since the snapshot was taken. Keep the
    // snapshot and pending_remap.json: the next replay (once the backend would
    // accept every entry, or at the next startup) skips the entries that took
    // and retries the refused ones, so a reattached unit gets its routing back.
    // Finishing here would turn a temporary detach into a permanent loss of the
    // pre-print mapping.
    //
    // Retention has no clock, but it is bounded. A later print start keeps
    // the record, and when that print ends an entry whose lane the backend
    // still does not report takes the tool's live lane
    // (drop_unreported_entries()), so a unit that never returns costs at most
    // one more print. A tool the user maps by hand outside a print takes that
    // mapping as its entry (adopt_hand_tool_mapping()), so no replay reverts
    // it. Otherwise the record ends when a replay delivers what is left.
    if (restores_refused > 0) {
        spdlog::warn("[PrintStartController] {} restore command(s) refused — snapshot and "
                     "pending_remap.json retained for replay once the backend accepts them",
                     restores_refused);
        observe_backend_for_retry();
        return;
    }

    // Backends that echo their mapping back let us wait for firmware truth
    // instead of trusting a send. Those that don't fall back to "sent to a ready
    // Klipper counts as delivered" — weaker, but waiting for a confirmation that
    // can never arrive would strand the record forever.
    if (!backend->reports_firmware_tool_mapping()) {
        spdlog::debug("[PrintStartController] Backend does not echo its tool mapping — treating "
                      "the send as delivery");
        finish_restore();
        return;
    }

    // Nothing was sent AND the mapping already matches: already correct, no
    // firmware round trip is coming to confirm. Done.
    if (restores_sent == 0) {
        finish_restore();
        return;
    }

    restore_generation_at_send_ = backend->firmware_tool_mapping_generation();
    awaiting_restore_confirmation_ = true;
    spdlog::info("[PrintStartController] Awaiting firmware confirmation of restore (generation {})",
                 restore_generation_at_send_);
    observe_ams_data_for_confirmation();
}

bool PrintStartController::retained_restore_sendable() const {
    const auto& saved = AmsState::instance().pending_tool_map_restore();
    if (saved.empty()) {
        return false;
    }
    auto* backend = AmsState::instance().get_backend(saved.backend_index);
    if (!backend) {
        return false;
    }
    // Before a backend's first report its bounds are not the printer's: CFS
    // accepts any encodable bay until a box frame says how many units exist.
    // Nothing is replayed until the lane set has actually been reported.
    if (backend->get_system_info().total_slots <= 0) {
        return false;
    }
    const auto current = backend->get_tool_mapping();
    for (size_t i = 0; i < saved.mapping.size(); ++i) {
        const int want = saved.mapping[i];
        const int have = (i < current.size()) ? current[i] : -1;
        if (want >= 0 && want != have &&
            !backend->can_set_tool_mapping(static_cast<int>(i), want).success()) {
            return false;
        }
    }
    return true;
}

void PrintStartController::observe_backend_for_retry() {
    // Refused although the backend would take every entry now: the refusal came
    // from past the precondition, and retrying on each data tick would repeat it
    // forever. Startup replays it.
    if (retained_restore_sendable()) {
        spdlog::warn("[PrintStartController] Backend accepts the refused entries yet refused "
                     "them — no in-session retry; pending_remap.json replays on next startup");
        return;
    }
    if (backend_retry_observer_) {
        return;
    }
    auto* subject = AmsState::instance().get_ams_data_revision_subject();
    if (!subject) {
        return;
    }
    restore_epoch_ = AmsState::instance().pending_tool_map_restore().epoch;
    backend_retry_observer_ = observe<int>(
        subject, this,
        [](PrintStartController* self, int) {
            if (!self->still_owns_restore()) {
                self->backend_retry_observer_.reset();
                return;
            }
            // A remap mid-job re-routes the tools of the print in progress.
            if (job_holds_machine(self->printer_state_.print_state().get_print_lifecycle()) ||
                !self->retained_restore_sendable()) {
                return;
            }
            spdlog::info("[PrintStartController] Backend now accepts the retained mapping — "
                         "retrying restore");
            self->backend_retry_observer_.reset();
            self->restore_filament_mapping();
        },
        AmsState::instance().get_subjects_lifetime());
}

void PrintStartController::finish_restore() {
    awaiting_restore_confirmation_ = false;
    ams_data_observer_.reset();
    backend_retry_observer_.reset();
    klippy_state_observer_.reset();
    AmsState::instance().clear_pending_tool_map_restore();
    clear_persisted_remap_state();
}

bool PrintStartController::still_owns_restore() const {
    const auto& saved = AmsState::instance().pending_tool_map_restore();
    return !saved.empty() && saved.epoch == restore_epoch_;
}

bool PrintStartController::owns_restore_observers() const {
    return (print_state_observer_ || klippy_state_observer_ || backend_retry_observer_ ||
            ams_data_observer_) &&
           !AmsState::instance().pending_tool_map_restore().empty();
}

void PrintStartController::observe_ams_data_for_confirmation() {
    if (ams_data_observer_) {
        return;
    }

    auto* subject = AmsState::instance().get_ams_data_revision_subject();
    if (!subject) {
        return;
    }

    ams_data_observer_ = observe<int>(
        subject, this, [](PrintStartController* self, int) { self->check_restore_confirmed(); },
        AmsState::instance().get_subjects_lifetime());
}

void PrintStartController::check_restore_confirmed() {
    const auto& saved = AmsState::instance().pending_tool_map_restore();
    if (!awaiting_restore_confirmation_ || saved.empty()) {
        return;
    }

    auto* backend = AmsState::instance().get_backend(saved.backend_index);
    if (!backend) {
        // Backend disappeared mid-wait. Nothing can confirm now, and holding the
        // record would block every future restore, so give up on THIS one but
        // leave the persisted file for the next startup to replay.
        spdlog::warn("[PrintStartController] Backend {} gone while awaiting restore confirmation",
                     saved.backend_index);
        awaiting_restore_confirmation_ = false;
        ams_data_observer_.reset();
        return;
    }

    // The revision subject is a coarse wake-up — it fires for our own optimistic
    // writes too. The generation is what separates "the printer said so" from
    // "we said so", so an unmoved generation means this tick proves nothing.
    const uint64_t generation = backend->firmware_tool_mapping_generation();
    if (generation == restore_generation_at_send_) {
        return;
    }

    auto current = backend->get_tool_mapping();
    for (size_t i = 0; i < saved.mapping.size(); ++i) {
        const int want = saved.mapping[i];
        const int have = (i < current.size()) ? current[i] : -1;
        // A tool unmapped before the print was never sent back to -1.
        if (want >= 0 && want != have) {
            // Firmware reported something, but not what we asked for. Keep
            // waiting: a multi-lane restore lands one delta at a time, so a
            // partial match is the normal intermediate state, not a failure.
            spdlog::debug("[PrintStartController] Restore not yet confirmed: T{} reads slot {}, "
                          "want {} (generation {})",
                          i, have, want, generation);
            return;
        }
    }

    spdlog::info("[PrintStartController] Firmware confirmed restore of {} mapping(s) (generation "
                 "{} -> {})",
                 saved.mapping.size(), restore_generation_at_send_, generation);
    finish_restore();
}

// ============================================================================
// Crash Recovery Persistence
// ============================================================================

static constexpr const char* kPendingRemapFilename = "pending_remap.json";

static std::string pending_remap_path() {
    return hfs::join_path(helix::get_user_config_dir(), kPendingRemapFilename);
}

namespace {
// The record file holds one entry per printer under "printers", keyed by the
// printer id, because every configured printer shares the config dir and a
// print on one must not overwrite another's unrestored map. A file with
// backend_index at its top level is a single entry naming no printer; it is
// read as the active printer's, which is all a single-printer setup has.

// nullopt when the file exists but cannot be read: it may still hold other
// printers' records, so nothing rebuilds it from empty. A file that reads but
// does not parse is moved aside to <path>.corrupt and read as empty.
std::optional<nlohmann::json> read_remap_file() {
    nlohmann::json j = nlohmann::json::object();
    const std::string path = pending_remap_path();
    if (hfs::exists(path)) {
        auto text = tio::read_file(path);
        if (!text) {
            spdlog::error("[PrintStartController] Cannot read {} — leaving it untouched", path);
            return std::nullopt;
        }
        auto parsed = nlohmann::json::parse(*text, nullptr, false);
        if (parsed.is_object()) {
            j = std::move(parsed);
        } else {
            const std::string aside = path + ".corrupt";
            if (!hfs::rename(path, aside)) {
                spdlog::error("[PrintStartController] {} is not a JSON object and could not be "
                              "moved aside: {} — leaving it untouched",
                              path, std::strerror(errno));
                return std::nullopt;
            }
            spdlog::error("[PrintStartController] {} is not a JSON object — moved to {}", path,
                          aside);
        }
    }
    if (j.contains("backend_index")) {
        nlohmann::json converted = nlohmann::json::object();
        converted["printers"][Config::get_instance()->get_active_printer_id()] = std::move(j);
        return converted;
    }
    if (!j.contains("printers") || !j["printers"].is_object()) {
        j["printers"] = nlohmann::json::object();
    }
    return j;
}

void write_remap_file(const nlohmann::json& j) {
    const std::string path = pending_remap_path();
    const auto* printers = helix::json_util::find_member(j, "printers");
    if (!printers || !printers->is_object() || printers->empty()) {
        if (hfs::exists(path) && !hfs::remove(path)) {
            spdlog::warn("[PrintStartController] Failed to clear remap state: {}",
                         std::strerror(errno));
        }
        return;
    }
    if (!hfs::create_directories(std::string(hfs::parent_path(path)))) {
        spdlog::warn("[PrintStartController] Failed to persist remap state: {}",
                     std::strerror(errno));
        return;
    }
    if (tio::write_file_atomic(path, helix::json_util::safe_dump(j, 2))) {
        spdlog::debug("[PrintStartController] Wrote remap state to {}", path);
    }
}
} // namespace

void PrintStartController::persist_remap_state() {
    const auto& saved = AmsState::instance().pending_tool_map_restore();
    if (saved.empty()) {
        return;
    }

    nlohmann::json entry;
    entry["backend_index"] = saved.backend_index;
    entry["backend_type"] = static_cast<int>(saved.backend_type);
    entry["tool_mapping"] = saved.mapping;

    auto j = read_remap_file();
    if (!j) {
        return;
    }
    (*j)["printers"][Config::get_instance()->get_active_printer_id()] = std::move(entry);
    write_remap_file(*j);
}

void PrintStartController::clear_persisted_remap_state() {
    auto j = read_remap_file();
    if (!j) {
        return;
    }
    (*j)["printers"].erase(Config::get_instance()->get_active_printer_id());
    write_remap_file(*j);
}

void PrintStartController::recover_pending_remap() {
    const auto read = read_remap_file();
    if (!read) {
        return;
    }
    const nlohmann::json& file = *read;
    const std::string active_id = Config::get_instance()->get_active_printer_id();
    const auto* printers = helix::json_util::find_member(file, "printers");
    const auto* entry = (printers && printers->is_object())
                            ? helix::json_util::find_member(*printers, active_id.c_str())
                            : nullptr;
    if (!entry || !entry->is_object()) {
        return;
    }
    const nlohmann::json& j = *entry;

    int backend_idx = helix::json_util::safe_int(j, "backend_index", -1);
    std::vector<int> mapping;
    bool mapping_bad = false;
    if (const auto* tm = helix::json_util::find_member(j, "tool_mapping")) {
        if (!tm->is_array()) {
            mapping_bad = true;
        } else {
            mapping.reserve(tm->size());
            for (const auto& e : *tm) {
                if (!e.is_number()) {
                    mapping_bad = true;
                    break;
                }
                mapping.push_back(e.get<int>());
            }
        }
    }
    if (mapping_bad) {
        spdlog::warn("[PrintStartController] Failed to load pending remap: bad tool_mapping");
        clear_persisted_remap_state();
        return;
    }

    if (backend_idx < 0 || mapping.empty()) {
        spdlog::debug("[PrintStartController] Invalid pending remap entry — removing");
        clear_persisted_remap_state();
        return;
    }

    auto& ams = AmsState::instance();
    if (!ams.pending_tool_map_restore().empty()) {
        spdlog::debug("[PrintStartController] A pending remap is already held in memory — "
                      "not reloading pending_remap.json");
        return;
    }

    // Load saved state and decide between immediate vs deferred restore.
    const int type_val = helix::json_util::safe_int(j, "backend_type", 0);
    ams.set_pending_tool_map_restore(backend_idx, static_cast<AmsType>(type_val),
                                     std::move(mapping));
    const auto& saved = ams.pending_tool_map_restore();

    // If a print is still active, the user's remap is still LOAD-BEARING —
    // reverting now would silently swap filament routing under a running
    // print. Defer the restore until the print reaches a terminal state.
    // observe_lifecycle_for_restore registers an observer that arms on
    // the first live lifecycle value and fires restore_filament_mapping on
    // the terminal value that follows, so the attach-fire on the current
    // PRINTING/PAUSED value arms it rather than firing early.
    // RAW_PRINT_STATE_OK: suppresses the observer's registration-fire
    // while a job runs; a preparing job has no mapping to restore.
    auto current_state = printer_state_.print_state().get_print_job_state();
    // The mapping is restored on a TERMINAL state; this only suppresses the
    // observer's immediate registration-fire while a job is running. The wire
    // question is the right one: a preparing job has no mapping to restore.
    bool print_active = printer_has_job(current_state);

    if (print_active) {
        spdlog::info("[PrintStartController] Crash recovery: found pending remap "
                     "({} tools, backend {}) — print still active (state={}), "
                     "deferring restore until print ends",
                     saved.mapping.size(), saved.backend_index, static_cast<int>(current_state));
        observe_lifecycle_for_restore();
    } else {
        spdlog::info("[PrintStartController] Crash recovery: found pending remap "
                     "({} tools, backend {}) — restoring",
                     saved.mapping.size(), saved.backend_index);
        restore_filament_mapping();
    }
}

void adopt_hand_tool_mapping(int backend_index, int tool, int slot) {
    auto& ams = AmsState::instance();
    const auto& saved = ams.pending_tool_map_restore();
    if (saved.empty() || saved.backend_index != backend_index ||
        job_holds_machine(get_printer_state().print_state().get_print_lifecycle())) {
        return;
    }
    if (!ams.set_pending_tool_map_entry(tool, slot)) {
        return;
    }
    spdlog::info("[PrintStartController] T{} mapped to slot {} by hand — the pending restore "
                 "keeps it there",
                 tool, slot);

    auto* backend = ams.get_backend(backend_index);
    // On a lane-keyed table the edit took the lane from whichever tool held
    // it, so a saved entry naming that lane would take it back from @p tool.
    if (backend) {
        const auto live = backend->get_tool_mapping();
        const auto saved = ams.pending_tool_map_restore().mapping;
        for (size_t i = 0; i < saved.size(); ++i) {
            if (static_cast<int>(i) == tool || saved[i] != slot) {
                continue;
            }
            const int now = i < live.size() ? live[i] : -1;
            if (now != slot) {
                spdlog::info("[PrintStartController] T{}'s saved slot {} went to T{} by hand — "
                             "the pending restore leaves T{} on slot {}",
                             i, slot, tool, i, now);
                ams.set_pending_tool_map_entry(static_cast<int>(i), now);
            }
        }
    }
    if (backend &&
        restore_landed(ams.pending_tool_map_restore().mapping, backend->get_tool_mapping())) {
        spdlog::info("[PrintStartController] Nothing left to restore after the hand edit");
        ams.clear_pending_tool_map_restore();
        PrintStartController::clear_persisted_remap_state();
        return;
    }
    PrintStartController::persist_remap_state();
}

} // namespace helix::ui
