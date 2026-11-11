// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_print_state_status.cpp
 * @brief PrinterPrintState::update_from_status and the step per status object it calls.
 *
 * The rest of PrinterPrintState lives in printer_print_state.cpp.
 */

#include "firmware_error_envelope.h"
#include "json_utils.h"
#include "layer_estimate.h"
#include "pause_descriptor.h"
#include "plr_backend.h"
#include "printer_print_state.h"
#include "printer_state.h" // For enum definitions
#include "sound_manager.h"
#include "unit_conversions_json.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <string>

namespace helix {

void PrinterPrintState::update_from_status(const nlohmann::json& status, bool whole_objects) {
    // Layer tracking has two sources within a single status update:
    //   primary  — print_stats.info.{current_layer,total_layer} (slicer
    //              SET_PRINT_STATS_INFO; authoritative when present)
    //   fallback — virtual_sdcard.{layer,layer_count} (Klipper-side; populated
    //              only when sdcard print is active)
    // If both arrive in the same update, primary wins. The flags are scoped to
    // this one update.
    InfoLayerFields from_info;
    // IMPORTANT: Process print_stats BEFORE virtual_sdcard.
    // The print_state_enum_ observer fires synchronously and reads print_progress_
    // for mid-print detection (should_start_print_collector). If virtual_sdcard is
    // processed first, progress is already non-zero when the observer fires, causing
    // false mid-print detection and preventing the print start collector from activating.
    if (auto stats_it = status.find("print_stats"); stats_it != status.end()) {
        apply_print_stats(status, *stats_it, from_info);
    }
    // After print_stats: an M117 delta in this frame must land after the
    // end-of-print clear that print_stats' state edge performs.
    apply_display_status(status, whole_objects);
    apply_extruder_filament_used(status);
    // Before virtual_sdcard and the Z derivation, which read the cached Z
    // whichever fields this update carries.
    cache_gcode_z(status);
    apply_virtual_sdcard(status, from_info);
    apply_plr_interrupted(status);
    derive_layer_from_z();
    apply_layer_pause_object(status);
    refresh_layer_pause();
}

PrinterPrintState::PrintDurations
PrinterPrintState::read_print_durations(const nlohmann::json& stats) {
    PrintDurations durations;
    if (auto it = stats.find("print_duration"); it != stats.end() && it->is_number()) {
        durations.print_s = it->get<double>();
    }
    // total_duration = wall-clock elapsed since job started (includes prep, pauses)
    if (auto it = stats.find("total_duration"); it != stats.end() && it->is_number()) {
        durations.total_s = it->get<double>();
    }
    return durations;
}

void PrinterPrintState::apply_print_stats(const nlohmann::json& status, const nlohmann::json& stats,
                                          InfoLayerFields& from_info) {
    // plr_backend owns which print_stats key signals PLR support and the
    // numeric-only rule. Latch UP only: status arrives as deltas, so a later
    // frame without the key says nothing, and clearing on absence would
    // manufacture a spurious 1->0->1 edge and re-fire the probe.
    // PlrOfferController resets this on the disconnect edge instead.
    if (helix::plr_parse_power_loss_signal(status) &&
        lv_subject_get_int(&plr_power_loss_signal_) != 1) {
        spdlog::info("[PrinterPrintState] PLR power-loss signal present");
        lv_subject_set_int(&plr_power_loss_signal_, 1);
    }

    // Seed print_duration_ BEFORE updating print_state_enum_. The state-change
    // observer in MoonrakerManager fires synchronously when print_state_enum_
    // changes and uses print_duration as a mid-print-attach signal: at a normal
    // print start print_duration is 0; when helix-screen connects to a printer
    // already mid-print, the initial subscription payload carries
    // print_duration > 0. Without this pre-pass, print_duration_ would still
    // read 0 at the moment of the state transition, the collector would
    // wrongly start, and the lifecycle would park in Preparing — leaving
    // "Starting Print..." stuck and dropping print_elapsed updates.
    const PrintDurations durations = read_print_durations(stats);
    apply_print_durations(durations);

    // Parsed before state: Klipper sets the error and its message in one frame,
    // and an observer of the terminal transition reads the message that
    // explains it.
    const bool frame_has_message = apply_print_message(stats);

    ++print_stats_frames_;
    const bool frame_has_exception = helix::pause_descriptor::frame_carries_cause(stats);
    const bool identity_follows_job =
        apply_print_job_state(status, stats, frame_has_exception, frame_has_message);
    apply_print_filename(stats, identity_follows_job);
    apply_pause_cause(stats, frame_has_exception);
    from_info = apply_info_layers(stats);
    apply_print_counters(stats, durations);
}

void PrinterPrintState::apply_print_durations(const PrintDurations& durations) {
    if (durations.print_s) {
        lv_subject_set_int(&print_duration_, static_cast<int>(*durations.print_s));
    }
    if (durations.total_s) {
        lv_subject_set_int(&print_elapsed_, static_cast<int>(*durations.total_s));
    }
}

bool PrinterPrintState::apply_print_message(const nlohmann::json& stats) {
    // print_stats.message — populated by Klipper to describe pause/error reason
    // (e.g. "Filament Sensor filament_sensor: Runout Detected"). Subscribed-field
    // updates may send null when the field is absent on the Klipper side.
    const auto msg_it = stats.find("message");
    if (msg_it == stats.end()) {
        return false;
    }
    // A print error can carry a firmware JSON envelope; every consumer
    // shows or substring-matches the message, so decode it once here.
    const std::string new_msg =
        msg_it->is_string() ? decode_firmware_error_text(msg_it->get_ref<const std::string&>())
                            : std::string();
    if (new_msg != lv_subject_get_string(&print_message_)) {
        lv_subject_copy_string(&print_message_, new_msg.c_str());
    }
    return true;
}

bool PrinterPrintState::apply_print_job_state(const nlohmann::json& status,
                                              const nlohmann::json& stats, bool frame_has_exception,
                                              bool frame_has_message) {
    // A printer-announced start learns its identity when the printer takes
    // the job, even when the file's name is the one print_stats already held.
    bool identity_follows_job = false;
    auto state_it = stats.find("state");
    if (state_it == stats.end() || !state_it->is_string()) {
        return identity_follows_job;
    }
    std::string state_str = state_it->get<std::string>();
    PrintJobState new_state = parse_print_job_state(state_str.c_str());
    auto current_state = static_cast<PrintJobState>(lv_subject_get_int(&print_state_enum_));
    auto current_outcome = static_cast<PrintOutcome>(lv_subject_get_int(&print_outcome_));

    // Update print_outcome based on state transitions:
    // - Set outcome when print reaches a terminal state (COMPLETE/CANCELLED/ERROR)
    // - Clear outcome when a NEW print starts (PRINTING from non-PAUSED)
    if (new_state != current_state) {
        // The exception describes one pause. Leaving the pause (a
        // resume, a cancel, a new print) ends it, so a later pause
        // whose frames carry no exception is not read as that one.
        // An exception in this same frame is parsed after this and latches
        // again.
        // RAW_PRINT_STATE_OK: the exception belongs to the printer's own
        // pause, so the wire state is what scopes it.
        if (new_state != PrintJobState::PAUSED) {
            clear_print_exception();
        } else if (!frame_has_exception && print_stats_frames_ - print_exception_frame_ > 1) {
            // An exception published earlier in the print describes
            // something else; this pause has none of its own yet.
            clear_print_exception();
        }
        // Entering a terminal state: record the outcome
        if (new_state == PrintJobState::COMPLETE) {
            spdlog::info("[PrinterPrintState] Print completed - setting outcome=COMPLETE");
            lv_subject_set_int(&print_outcome_, static_cast<int>(PrintOutcome::COMPLETE));
            freeze_progress_display(true);
        } else if (new_state == PrintJobState::CANCELLED) {
            spdlog::debug("[PrinterPrintState] Print cancelled - setting outcome=CANCELLED");
            lv_subject_set_int(&print_outcome_, static_cast<int>(PrintOutcome::CANCELLED));
            freeze_progress_display(false);
        } else if (new_state == PrintJobState::ERROR) {
            spdlog::info("[PrinterPrintState] Print error - setting outcome=ERROR");
            lv_subject_set_int(&print_outcome_, static_cast<int>(PrintOutcome::ERROR));
            freeze_progress_display(false);
        }
        // RAW_PRINT_STATE_OK: this runs INSIDE the wire-state handler,
        // one statement before publish_lifecycle_state() derives the
        // lifecycle from it. Reading the derived value here would read
        // the previous tick's answer.
        // Starting a NEW print: clear the previous outcome and slicer state
        // (only when transitioning TO PRINTING from a non-PAUSED state)
        else if (new_state == PrintJobState::PRINTING && current_state != PrintJobState::PAUSED) {
            if (current_outcome != PrintOutcome::NONE) {
                spdlog::info("[PrinterPrintState] New print starting - clearing outcome");
                lv_subject_set_int(&print_outcome_, static_cast<int>(PrintOutcome::NONE));
            }
            unfreeze_progress_display();
            // Reset slicer progress for the new print
            slicer_progress_ = 0.0;
            slicer_progress_active_ = false;
            // This frame's own message is already applied.
            if (!frame_has_message) {
                lv_subject_copy_string(&print_message_, "");
            }
        }

        // Clear M117 at print END, never at print start. Clearing on the
        // observed transition INTO printing destroys PRINT_START-era
        // messages permanently: Moonraker sends deltas, so Klipper never
        // re-sends an unchanged value. Correctness here depends on
        // print_stats being parsed before display_status within the same
        // status object (update_from_status' call order): if that ordering
        // ever reversed, a display_status delta belonging to this same
        // notification could be applied before this clear and get wiped
        // out immediately after. An END_PRINT macro's M117 arrives in a
        // later notification and displays normally.
        //
        // Must stay inside the `new_state != current_state` guard so this
        // is EDGE-triggered. Level-triggered would re-clear on every
        // notification while COMPLETE and would swallow the END_PRINT M117.
        //
        // Second clause covers an ABNORMAL exit: some paths (Klipper
        // restart mid-print, SDCARD_RESET_FILE, certain firmware cancels)
        // go printing/paused -> standby directly, without ever passing
        // through a terminal state, so the block above never fires and
        // the last mid-print M117 would otherwise persist into idle and
        // into the next print. This must NOT fire on the normal
        // complete -> standby leg of the end-of-print sequence — that
        // transition's `current_state` is COMPLETE, not
        // PRINTING/PAUSED, so it's excluded here and the END_PRINT
        // macro's farewell message (set after the COMPLETE clear above)
        // survives.
        // RAW_PRINT_STATE_OK: same handler, same reason as above. The
        // previous-state arm asks the wire question - printer_has_job() -
        // because a preparing job the printer never took set no M117.
        if (job_has_ended(new_state) ||
            (new_state == PrintJobState::STANDBY && printer_has_job(current_state))) {
            lv_subject_copy_string(&display_message_, "");
            update_display_message_visible();
        }
    }

    // Always update print_state_enum to reflect true Moonraker state
    // (print_outcome handles UI persistence for terminal states)
    if (new_state != current_state) {
        spdlog::debug("[PrinterPrintState] print_stats.state: '{}' -> enum {} (was {})", state_str,
                      static_cast<int>(new_state), static_cast<int>(current_state));
        lv_subject_set_int(&print_state_enum_, static_cast<int>(new_state));
        // A job the printer took with nothing of ours preparing (the
        // queue, another client, its own screen) is not one we started.
        if (printer_has_job(new_state) && !printer_has_job(current_state) &&
            preparing_job_.empty()) {
            confirmed_start_.reset();
        }
        // RAW_PRINT_STATE_OK: M300 waits in Klipper's G-code queue, so the
        // question is whether the printer's own job is printing, not the
        // app's lifecycle (Preparing does not run the print's moves).
        SoundManager::instance().set_printer_busy(new_state == PrintJobState::PRINTING);
        publish_lifecycle_state();
        reconcile_preparing();
    }

    // Update string subject AFTER enum so observers see consistent state
    // (PrintSelectPanel observes print_state_ string but reads print_state_enum_)
    if (strcmp(lv_subject_get_string(&print_state_), state_str.c_str()) != 0) {
        lv_subject_copy_string(&print_state_, state_str.c_str());
    }

    // Update print_active (1 when PRINTING/PAUSED, 0 otherwise)
    // This derived subject simplifies XML bindings for card visibility.
    // Uses the shared pure predicate so the subject's definition of
    // "active" matches the discovery-time idle gate exactly.
    bool is_active = status_indicates_active_print(status);
    int active_val = is_active ? 1 : 0;
    if (lv_subject_get_int(&print_active_) != active_val) {
        lv_subject_set_int(&print_active_, active_val);
        identity_follows_job = printer_side_pre_start_;

        // Safety: When print becomes inactive, ensure print_start_phase is IDLE
        // This prevents "Preparing Print" from showing when print is finished
        if (!is_active && !has_preparing_job()) {
            int phase = lv_subject_get_int(&print_start_phase_);
            if (phase != static_cast<int>(PrintStartPhase::IDLE)) {
                spdlog::warn("[PrinterPrintState] Safety reset: print inactive but phase={}, "
                             "resetting to IDLE",
                             phase);
                lv_subject_set_int(&print_start_phase_, static_cast<int>(PrintStartPhase::IDLE));
                lv_subject_set_int(&print_start_chamber_wait_, 0);
                publish_lifecycle_state();
                lv_subject_copy_string(&print_start_message_, "");
                lv_subject_set_int(&print_start_progress_, 0);
                update_display_message_visible();
            }
        }
    }

    // Update combined subject for home panel progress card visibility
    update_print_show_progress();
    lv_subject_set_int(&print_state_report_seq_, lv_subject_get_int(&print_state_report_seq_) + 1);
    return identity_follows_job;
}

void PrinterPrintState::apply_print_filename(const nlohmann::json& stats,
                                             bool identity_follows_job) {
    if (auto fn_it = stats.find("filename"); fn_it != stats.end() && fn_it->is_string()) {
        std::string filename = fn_it->get<std::string>();
        if (strcmp(lv_subject_get_string(&print_filename_), filename.c_str()) != 0) {
            // Order matters, exactly as it does in set_print_thumbnail():
            // the identity must be visible BEFORE any observer of
            // print_filename_ runs, or an immediate observer resolves the
            // new print through the previous one's name.
            recompute_effective_print_filename(filename);
            lv_subject_copy_string(&print_filename_, filename.c_str());
            identity_follows_job = false;
        }
        reconcile_preparing();
    }
    if (identity_follows_job) {
        recompute_effective_print_filename(lv_subject_get_string(&print_filename_));
    }
}

void PrinterPrintState::apply_pause_cause(const nlohmann::json& stats, bool frame_has_exception) {
    // Structured pause cause. pause_descriptor owns which firmware publishes
    // one and its schema; a frame silent about it leaves the latched cause,
    // and an empty descriptor (a user pause, or the cause cleared) resets it.
    if (auto cause = helix::pause_descriptor::read(stats)) {
        if (frame_has_exception) {
            print_exception_frame_ = print_stats_frames_;
        }
        print_exception_id_ = cause->id;
        print_exception_code_ = cause->code;
        print_exception_message_ = std::move(cause->message);
    }
    if (lv_subject_get_int(&print_exception_) != print_exception_code_) {
        lv_subject_set_int(&print_exception_, print_exception_code_);
    }
}

void PrinterPrintState::note_reported_layer(int layer, const char* source) {
    if (!has_real_layer_data_) {
        spdlog::info("[LayerTracker] Receiving real layer data from {}", source);
        has_real_layer_data_ = true;
    }
    printer_reports_layers_ = true; // sticky printer capability
    if (layer != lv_subject_get_int(&print_layer_current_)) {
        spdlog::debug("[LayerTracker] current_layer={} (from {})", layer, source);
    }
    apply_layer_current(layer, LayerSource::Reported, source);
}

void PrinterPrintState::note_reported_layer_total(int total, const char* source) {
    printer_reports_layers_ = true; // sticky printer capability (U1 sends this at start)
    if (total != lv_subject_get_int(&print_layer_total_)) {
        spdlog::debug("[LayerTracker] total_layer={} (from {})", total, source);
        lv_subject_set_int(&print_layer_total_, total);
    }
}

PrinterPrintState::InfoLayerFields
PrinterPrintState::apply_info_layers(const nlohmann::json& stats) {
    InfoLayerFields from_info;
    // Update layer info from print_stats.info (sent by Moonraker/mock client)
    // Note: Moonraker can send null values for layer fields when not available
    const auto info_it = stats.find("info");
    if (info_it == stats.end()) {
        return from_info;
    }
    if (!info_it->is_object()) {
        spdlog::debug("[LayerTracker] print_stats.info is null/missing - slicer may not emit "
                      "SET_PRINT_STATS_INFO");
        return from_info;
    }
    const auto& info = *info_it;
    spdlog::trace("[LayerTracker] print_stats.info received: {}",
                  helix::json_util::safe_dump(info));

    if (auto it = info.find("current_layer"); it != info.end() && it->is_number()) {
        const int current_layer = it->get<int>();
        note_reported_layer(current_layer, "print_stats.info");
        from_info.current = true;
        slicer_layers_reported_ = true;
        slicer_current_layer_ = current_layer;
    }

    if (auto it = info.find("total_layer"); it != info.end() && it->is_number()) {
        const int total_layer = it->get<int>();
        note_reported_layer_total(total_layer, "print_stats.info");
        from_info.total = true;
        slicer_layers_reported_ = true;
        slicer_total_layers_ = total_layer;
    }
    return from_info;
}

void PrinterPrintState::apply_print_counters(const nlohmann::json& stats,
                                             const PrintDurations& durations) {
    // Accept estimated_time from status updates (mock includes this; real Moonraker
    // sends it via file metadata API instead, handled by print status panel callback)
    if (auto it = stats.find("estimated_time"); it != stats.end() && it->is_number()) {
        int est = static_cast<int>(it->get<double>());
        if (est > 0 && estimated_print_time_ == 0) {
            estimated_print_time_ = est;
            spdlog::debug("[PrinterPrintState] Estimated time from status: {}s", est);
        }
    }

    // Track filament usage (Moonraker reports in mm)
    if (auto it = stats.find("filament_used"); it != stats.end() && it->is_number()) {
        lv_subject_set_int(&print_filament_used_, static_cast<int>(it->get<double>()));
    }

    // The durations again, after the state edge: what an observer of that
    // edge did to them, this frame's values replace.
    if (durations.print_s) {
        print_clock_s_ = *durations.print_s;
    }
    apply_print_durations(durations);

    if (durations.total_s) {
        // Estimate remaining from progress using print_duration (actual print time),
        // NOT total_duration (which includes prep/preheat and inflates the estimate)
        PrintEtaInputs eta_in;
        eta_in.progress_pct = lv_subject_get_int(&print_progress_);
        eta_in.print_duration_s = lv_subject_get_int(&print_duration_);
        eta_in.slicer_estimate_s = estimated_print_time_;
        eta_in.preprint_remaining_s = lv_subject_get_int(&preprint_remaining_);

        const int remaining = eta_estimator_.remaining_seconds(eta_in);
        if (remaining != PrintEtaEstimator::kNoEstimate &&
            lv_subject_get_int(&print_time_left_) != remaining) {
            lv_subject_set_int(&print_time_left_, remaining);
        }
    }
}

void PrinterPrintState::apply_display_status(const nlohmann::json& status, bool whole_objects) {
    // display_status: M73 progress + M117 message
    const auto display_it = status.find("display_status");
    if (display_it == status.end()) {
        return;
    }
    const auto& display = *display_it;
    // Not while paused. Klipper expires the M73 value behind this field and
    // substitutes virtual_sdcard's byte position, and a pause is exactly when
    // M73 stops being refreshed. commit_progress() declines to publish during
    // the pause, but recording the substituted value here would outlive it: a
    // later display_status carrying only an M117 message leaves this member
    // untouched and still reaches the publish below, handing it the byte
    // position as though it were the slicer's estimate.
    if (display.contains("progress") && display["progress"].is_number() &&
        get_print_lifecycle() != PrintState::Paused) {
        double raw = display["progress"].get<double>();
        slicer_progress_ = raw;
        if (raw > 0.0 && !slicer_progress_active_) {
            slicer_progress_active_ = true;
            spdlog::info("[PrinterPrintState] Slicer progress active (M73 detected)");
        }
    }
    // Klipper keeps the last M117 after a job ends, so a subscription response for
    // an ended job usually carries a PRINT_START message that the live path
    // cleared at the end edge. A whole-object payload cannot say whether its text
    // came before or after the end, and a replayed one may be older than the live
    // frames, so it leaves the shown message alone. print_stats is parsed first,
    // so this is the state the frame reports.
    const bool job_ended =
        job_has_ended(static_cast<PrintJobState>(lv_subject_get_int(&print_state_enum_)));
    if (display.contains("message") && whole_objects && job_ended) {
        spdlog::debug("[PrinterPrintState] Subscription response for an ended job: "
                      "display message left as shown");
    } else if (display.contains("message")) {
        const char* msg = display["message"].is_string()
                              ? display["message"].get_ref<const std::string&>().c_str()
                              : ""; // null or non-string clears the message
        if (strcmp(lv_subject_get_string(&display_message_), msg) != 0) {
            lv_subject_copy_string(&display_message_, msg);
        }
        update_display_message_visible();
    }

    // When slicer progress is active and display_status arrives without virtual_sdcard,
    // update print_progress_ directly from slicer value
    if (slicer_progress_active_ && !status.contains("virtual_sdcard")) {
        commit_progress(static_cast<int>(slicer_progress_ * 100.0 + 0.5));
    }
}

void PrinterPrintState::apply_extruder_filament_used(const nlohmann::json& status) {
    // Per-extruder filament_used (from Klipper's extruder/extruder1/... objects).
    // Keys live at the top level of the status payload, NOT under print_stats.
    // We scan a small fixed range (0..kMaxExtruderScan-1) — Klipper toolchanger
    // setups max out well below that. Missing keys are silently skipped.
    //
    // All map entries are pre-populated by init_subjects(). We do a direct
    // lookup and only mutate the int value inside the subject, so this runs
    // safely on the WebSocket background thread without racing the UI
    // accessor (no rehash, lv_subject_set_int is atomic on the int).
    for (int idx = 0; idx < kMaxExtruderScan; ++idx) {
        std::string key = (idx == 0) ? "extruder" : "extruder" + std::to_string(idx);
        auto json_it = status.find(key);
        if (json_it == status.end() || !json_it->is_object()) {
            continue;
        }
        if (!json_it->contains("filament_used") || !(*json_it)["filament_used"].is_number()) {
            continue;
        }
        int mm = static_cast<int>((*json_it)["filament_used"].get<double>());
        auto map_it = extruder_filament_used_.find(idx);
        if (map_it == extruder_filament_used_.end()) {
            // Cannot happen after init_subjects() pre-population; defensive log.
            spdlog::warn(
                "[PrinterPrintState] extruder{} filament_used entry missing (init skipped?)", idx);
            continue;
        }
        auto& info = map_it->second;
        if (info.subject && lv_subject_get_int(info.subject.get()) != mm) {
            lv_subject_set_int(info.subject.get(), mm);
        }
    }
}

void PrinterPrintState::cache_gcode_z(const nlohmann::json& status) {
    // Cache commanded Z (mm) from gcode_move for Z-height layer derivation.
    // gcode_position is [X, Y, Z, E] in raw mm (NOT centimm — only motion_state
    // converts). This lives at the top level of the status payload, so it may
    // arrive on an update that carries no virtual_sdcard.
    if (auto gm_it = status.find("gcode_move"); gm_it != status.end() && gm_it->is_object()) {
        if (auto pos_it = gm_it->find("gcode_position");
            pos_it != gm_it->end() && pos_it->is_array() && pos_it->size() >= 3 &&
            (*pos_it)[2].is_number()) {
            last_gcode_z_mm_ = (*pos_it)[2].get<double>();
            have_gcode_z_ = true;
        }
    }
}

void PrinterPrintState::apply_virtual_sdcard(const nlohmann::json& status,
                                             InfoLayerFields from_info) {
    const auto sdcard_it = status.find("virtual_sdcard");
    if (sdcard_it == status.end()) {
        return;
    }
    const auto& sdcard = *sdcard_it;

    // SD playback active flag — drives prepare_for_resume's dirty-bed
    // restart UX. Coexists independently with PrintJobState; do not
    // gate on other fields being present.
    if (sdcard.contains("is_active") && sdcard["is_active"].is_boolean()) {
        sdcard_active_ = sdcard["is_active"].get<bool>();
    }

    // Power-loss recovery: plr_backend owns which keys carry the
    // validated-snapshot flag and the recovery file, and their type rules
    // (-1 / nullptr mean this frame says nothing about them).
    if (const int snapshot_valid = helix::plr_parse_snapshot_valid(status); snapshot_valid >= 0) {
        lv_subject_set_int(&plr_snapshot_valid_, snapshot_valid);
    }
    if (const std::string* fp = helix::plr_parse_recovery_file(status);
        fp != nullptr && pl_recovery_file_ != *fp) {
        pl_recovery_file_ = *fp;
    }

    if (!sdcard.contains("progress") || !sdcard["progress"].is_number()) {
        return;
    }
    int file_progress_pct = helix::units::json_to_percent(sdcard, "progress");

    // Update print_progress_ subject (skipped when slicer is active
    // but only file progress arrived — slicer is authoritative)
    bool skip_progress_update = slicer_progress_active_ && !status.contains("display_status");

    if (!skip_progress_update) {
        int progress_pct = file_progress_pct;
        if (slicer_progress_active_) {
            // Slicer active and display_status present — use slicer value
            progress_pct = static_cast<int>(slicer_progress_ * 100.0 + 0.5);
        }
        commit_progress(progress_pct);
    }

    // virtual_sdcard.layer / layer_count are the FALLBACK source —
    // skip them when print_stats.info already provided the value in
    // this same update, since slicer-supplied values are
    // authoritative (cancel-stale-progress semantics during
    // pause/resume, etc.).
    if (!from_info.current && sdcard.contains("layer") && sdcard["layer"].is_number_integer()) {
        note_reported_layer(sdcard["layer"].get<int>(), "virtual_sdcard");
    }
    if (!from_info.total && sdcard.contains("layer_count") &&
        sdcard["layer_count"].is_number_integer()) {
        note_reported_layer_total(sdcard["layer_count"].get<int>(), "virtual_sdcard");
    }

    estimate_layer_from_progress(file_progress_pct);
}

void PrinterPrintState::estimate_layer_from_progress(int file_progress_pct) {
    // Layer estimation fallback: linear interpolation from file progress.
    //
    // Gate on the STICKY printer_reports_layers_ capability, NOT the
    // per-print has_real_layer_data_ flag. A printer that has ever
    // reported a real layer field (the U1 sends total_layer in
    // print_stats.info from print start) must NEVER fabricate a layer
    // from progress: reset_for_new_print() clears has_real_layer_data_
    // at the start of each print, and Moonraker DELTA updates omit the
    // unchanged info.current_layer=0 throughout pre-print, so that flag
    // stays false while file progress climbs. A per-print gate would
    // estimate current_layer=1 from ~2% progress mid-pre-print, which
    // trips the pre-print→printing hand-off
    // (MoonrakerManager::should_complete_preprint) ~24s in. With the
    // sticky gate, current_layer holds the authoritative info value (0
    // through pre-print) and only a genuine info.current_layer>=1
    // advances it. Estimation remains available for printers that never
    // report any real layer data (printer_reports_layers_ stays false).
    // Additionally require unknown slice geometry (layer_height_ <= 0):
    // when the layer height IS known, derive_layer_from_z() owns the
    // estimate (it tracks real geometry instead of the byte/time progress
    // fraction, which drifts high early in a print). This tier stays for
    // non-sliced jobs / prints with no height metadata.
    //
    // Gated on print_duration > 0: Klipper advances print_duration only
    // once real print/extrusion moves execute — never during PRINT_START
    // (bed mesh / purge line / Z-hop). File-position progress, however,
    // climbs while the START_PRINT macro streams through the file header,
    // so without this gate a fabricated layer appears (and self-corrects
    // only once printing begins). Deferring until print_duration > 0 shows
    // the pre-print default instead of a bogus number.
    if (!(!printer_reports_layers_ && layer_height_ <= 0.0 &&
          lv_subject_get_int(&print_duration_) > 0)) {
        return;
    }
    const auto current_state = static_cast<PrintJobState>(lv_subject_get_int(&print_state_enum_));
    if (job_has_ended(current_state) || file_progress_pct <= 0) {
        return;
    }
    const int total = lv_subject_get_int(&print_layer_total_);
    if (total <= 0) {
        return;
    }
    const int estimated = helix::layer_from_progress(file_progress_pct, total);
    const int current = lv_subject_get_int(&print_layer_current_);
    // Progress-fraction guess — the display marks this "~".
    layer_z_derived_ = false;
    if (estimated != current) {
        spdlog::debug("[LayerTracker] Estimated layer {}/{} from progress {}%", estimated, total,
                      file_progress_pct);
        apply_layer_current(estimated, LayerSource::Estimate, "progress estimate");
    }
}

void PrinterPrintState::apply_plr_interrupted(const nlohmann::json& status) {
    // Power-loss recovery: plr_backend owns which status key carries the
    // interrupted flag and the boolean-only rule (-1 means this frame says
    // nothing about it; see plr_parse_interrupted_flag). The offer controller
    // resets the subject on the disconnect edge, like plr_snapshot_valid.
    helix::plr_parse_saved_file(status, plr_saved_file_);
    const int plr_flag = helix::plr_parse_interrupted_flag(status);
    if (plr_flag >= 0 && lv_subject_get_int(&plr_interrupted_flag_) != plr_flag) {
        spdlog::debug("[PrinterPrintState] PLR interrupted flag = {} (power-loss recovery)",
                      plr_flag);
        lv_subject_set_int(&plr_interrupted_flag_, plr_flag);
    }
}

void PrinterPrintState::derive_layer_from_z() {
    // Z-height current-layer derivation (Mainsail/Fluidd parity) — tier 3.
    //
    // For printers whose slicer never reports a layer number, derive the layer
    // from commanded Z instead of the byte/time progress fraction. Progress is
    // unrelated to layer count and drifts high early (12% of 75 layers => ~9
    // while the true layer is 5); layer_from_z() states the formula. A Z-hop
    // lifts Z several layers for each travel move, so a new layer is shown
    // only once Z has held it for kHeightLayerDwellSeconds of print time
    // (decide_height_layer()); the first derived layer of a print shows at once.
    //
    // Gated on the STICKY printer_reports_layers_ (a printer that ever reports a
    // real layer field must never fabricate one), known geometry, and a cached
    // Z. has_real_layer_data_ / printer_reports_layers_ are deliberately NOT set
    // (this is not a slicer-reported field, so the pre-print completion gate
    // should_complete_preprint stays unaffected). It IS flagged layer_z_derived_,
    // which marks the value display-accurate (Mainsail parity) so the label drops
    // the "~" estimate prefix — the derivation tracks real commanded geometry.
    //
    // Gated on print_duration > 0 (see estimate_layer_from_progress()): during
    // PRINT_START the commanded Z is driven by bed probing and Z-hop, NOT model
    // layers, so the formula fabricates a bogus layer (e.g. Z=2mm with 0.2mm
    // layers => "layer 10"). Worse, this tier sets layer_z_derived_, so
    // layer_is_accurate() would present that garbage WITHOUT the "~" estimate
    // prefix. Klipper holds print_duration at 0 until real extrusion begins, so
    // deferring the derivation until then keeps the layer at the pre-print
    // default through bed mesh / purge / Z-hop.
    if (!(!printer_reports_layers_ && layer_height_ > 0.0 && have_gcode_z_ &&
          lv_subject_get_int(&print_duration_) > 0)) {
        return;
    }
    const auto current_state = static_cast<PrintJobState>(lv_subject_get_int(&print_state_enum_));
    const int total = lv_subject_get_int(&print_layer_total_);
    if (job_has_ended(current_state) || total <= 0) {
        return;
    }
    const int derived =
        helix::layer_from_z(last_gcode_z_mm_, first_layer_height_, layer_height_, total);
    const int shown = lv_subject_get_int(&print_layer_current_);
    const HeightLayerDecision decision = decide_height_layer(!layer_z_derived_, shown, derived,
                                                             height_layer_pending_, print_clock_s_);
    height_layer_pending_ = decision.pending;
    // Mark the source BEFORE writing the subject so the label observer
    // reads the correct accuracy state on the change it triggers.
    layer_z_derived_ = true;
    if (decision.show != shown) {
        spdlog::debug("[LayerTracker] Derived layer {}/{} from Z={:.3f}mm "
                      "(first={:.3f}, height={:.3f})",
                      decision.show, total, last_gcode_z_mm_, first_layer_height_, layer_height_);
        apply_layer_current(decision.show, LayerSource::Estimate, "z-height derivation");
    }
}

void PrinterPrintState::apply_layer_pause_object(const nlohmann::json& status) {
    // The printer's own record of a scheduled layer pause, when it keeps one.
    if (layer_pause_state_object_.empty()) {
        return;
    }
    if (const auto* object =
            helix::json_util::find_member(status, layer_pause_state_object_.c_str())) {
        layer_pause_.apply_status(*object);
    }
}

} // namespace helix
