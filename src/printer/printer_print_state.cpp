// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_print_state.cpp
 * @brief Print state management extracted from PrinterState
 *
 * Manages print subjects including progress, state, timing, layers, and
 * print start phases. Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_print_state.h"

#include "ui_update_queue.h"

#include "printer_state.h" // For enum definitions
#include "state/subject_macros.h"
#include "unit_conversions.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace helix {

PrinterPrintState::PrinterPrintState() {
    // Initialize string buffers
    std::memset(print_filename_buf_, 0, sizeof(print_filename_buf_));
    std::memset(print_display_filename_buf_, 0, sizeof(print_display_filename_buf_));
    std::memset(print_thumbnail_path_buf_, 0, sizeof(print_thumbnail_path_buf_));
    std::memset(print_state_buf_, 0, sizeof(print_state_buf_));
    std::memset(print_start_message_buf_, 0, sizeof(print_start_message_buf_));
    std::memset(print_start_time_left_buf_, 0, sizeof(print_start_time_left_buf_));
    std::memset(display_message_buf_, 0, sizeof(display_message_buf_));

    // Set default values
    std::strcpy(print_state_buf_, "standby");
}

void PrinterPrintState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterPrintState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterPrintState] Initializing subjects (register_xml={})", register_xml);

    // Print progress subjects
    INIT_SUBJECT_INT(print_progress, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(print_filename, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(print_state, "standby", subjects_, register_xml);
    INIT_SUBJECT_INT(print_state_enum, static_cast<int>(PrintJobState::STANDBY), subjects_,
                     register_xml);
    INIT_SUBJECT_INT(print_outcome, static_cast<int>(PrintOutcome::NONE), subjects_, register_xml);
    INIT_SUBJECT_INT(print_active, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(print_show_progress, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(print_display_filename, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(print_thumbnail_path, "", subjects_, register_xml);

    // Layer tracking subjects
    INIT_SUBJECT_INT(print_layer_current, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(print_layer_total, 0, subjects_, register_xml);

    // Print time tracking subjects (NOT XML-registered: formatted STRING subjects
    // in PrintStatusPanel own the XML bindings for print_elapsed/print_remaining)
    INIT_SUBJECT_INT(print_duration, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(print_elapsed, 0, subjects_, false);
    INIT_SUBJECT_INT(print_time_left, 0, subjects_, false);
    INIT_SUBJECT_INT(print_filament_used, 0, subjects_, register_xml);

    // Print start progress subjects
    INIT_SUBJECT_INT(print_start_phase, static_cast<int>(PrintStartPhase::IDLE), subjects_,
                     register_xml);
    INIT_SUBJECT_STRING(print_start_message, "", subjects_, register_xml);
    INIT_SUBJECT_INT(print_start_progress, 0, subjects_, register_xml);

    // Print workflow in-progress subject
    INIT_SUBJECT_INT(print_in_progress, 0, subjects_, register_xml);

    // Pre-print duration prediction subjects
    INIT_SUBJECT_STRING(print_start_time_left, "", subjects_, register_xml);
    INIT_SUBJECT_INT(preprint_remaining, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(preprint_elapsed, 0, subjects_, register_xml);

    // Klipper display message (M117 / display_status.message)
    INIT_SUBJECT_STRING(display_message, "", subjects_, register_xml);
    INIT_SUBJECT_INT(display_message_visible, 0, subjects_, register_xml);

    subjects_initialized_ = true;
    spdlog::trace("[PrinterPrintState] Subjects initialized successfully");
}

void PrinterPrintState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[PrinterPrintState] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterPrintState::reset_for_new_print() {
    // Clear stale print PROGRESS data when starting a new print.
    // The preparing overlay covers the UI, so stale data isn't visible.
    // IMPORTANT: Do NOT clear print_filename_ or print_display_filename_ here!
    // Clearing filename triggers ActivePrintMediaManager to wipe the thumbnail we just set.
    // Filename is Moonraker's source of truth - it updates when the print actually starts.
    lv_subject_set_int(&print_progress_, 0);
    lv_subject_set_int(&print_layer_current_, 0);
    has_real_layer_data_ = false;
    slicer_progress_ = 0.0;
    slicer_progress_active_ = false;
    lv_subject_copy_string(&display_message_, "");
    lv_subject_set_int(&display_message_visible_, 0);
    lv_subject_set_int(&print_duration_, 0);
    lv_subject_set_int(&print_elapsed_, 0);
    lv_subject_set_int(&print_filament_used_, 0);
    // Re-seed time_left from slicer estimate instead of clearing to 0.
    // For same-file reprints, the metadata callback won't re-fire since
    // the filename hasn't changed, so we preserve the previous estimate.
    // For different files, the metadata callback updates both values.
    lv_subject_set_int(&print_time_left_, estimated_print_time_);
    // DON'T clear estimated_print_time_ - it belongs to the file, not the session
    spdlog::trace("[PrinterPrintState] Reset print progress for new print (slicer_est={}s)",
                  estimated_print_time_);
}

void PrinterPrintState::update_from_status(const nlohmann::json& status) {
    // IMPORTANT: Process print_stats BEFORE virtual_sdcard.
    // The print_state_enum_ observer fires synchronously and reads print_progress_
    // for mid-print detection (should_start_print_collector). If virtual_sdcard is
    // processed first, progress is already non-zero when the observer fires, causing
    // false mid-print detection and preventing the print start collector from activating.
    if (status.contains("print_stats")) {
        const auto& stats = status["print_stats"];

        if (stats.contains("state")) {
            std::string state_str = stats["state"].get<std::string>();
            PrintJobState new_state = parse_print_job_state(state_str.c_str());
            auto current_state = static_cast<PrintJobState>(lv_subject_get_int(&print_state_enum_));
            auto current_outcome = static_cast<PrintOutcome>(lv_subject_get_int(&print_outcome_));

            // Update print_outcome based on state transitions:
            // - Set outcome when print reaches a terminal state (COMPLETE/CANCELLED/ERROR)
            // - Clear outcome when a NEW print starts (PRINTING from non-PAUSED)
            if (new_state != current_state) {
                // Entering a terminal state: record the outcome
                if (new_state == PrintJobState::COMPLETE) {
                    spdlog::info("[PrinterPrintState] Print completed - setting outcome=COMPLETE");
                    lv_subject_set_int(&print_outcome_, static_cast<int>(PrintOutcome::COMPLETE));
                } else if (new_state == PrintJobState::CANCELLED) {
                    spdlog::debug(
                        "[PrinterPrintState] Print cancelled - setting outcome=CANCELLED");
                    lv_subject_set_int(&print_outcome_, static_cast<int>(PrintOutcome::CANCELLED));
                } else if (new_state == PrintJobState::ERROR) {
                    spdlog::info("[PrinterPrintState] Print error - setting outcome=ERROR");
                    lv_subject_set_int(&print_outcome_, static_cast<int>(PrintOutcome::ERROR));
                }
                // Starting a NEW print: clear the previous outcome and slicer state
                // (only when transitioning TO PRINTING from a non-PAUSED state)
                else if (new_state == PrintJobState::PRINTING &&
                         current_state != PrintJobState::PAUSED) {
                    if (current_outcome != PrintOutcome::NONE) {
                        spdlog::info("[PrinterPrintState] New print starting - clearing outcome");
                        lv_subject_set_int(&print_outcome_, static_cast<int>(PrintOutcome::NONE));
                    }
                    // Reset slicer progress and display message for the new print
                    slicer_progress_ = 0.0;
                    slicer_progress_active_ = false;
                    lv_subject_copy_string(&display_message_, "");
                    lv_subject_set_int(&display_message_visible_, 0);
                }
            }

            // Always update print_state_enum to reflect true Moonraker state
            // (print_outcome handles UI persistence for terminal states)
            if (new_state != current_state) {
                spdlog::debug("[PrinterPrintState] print_stats.state: '{}' -> enum {} (was {})",
                              state_str, static_cast<int>(new_state),
                              static_cast<int>(current_state));
                lv_subject_set_int(&print_state_enum_, static_cast<int>(new_state));
            }

            // Update string subject AFTER enum so observers see consistent state
            // (PrintSelectPanel observes print_state_ string but reads print_state_enum_)
            lv_subject_copy_string(&print_state_, state_str.c_str());

            // Update print_active (1 when PRINTING/PAUSED, 0 otherwise)
            // This derived subject simplifies XML bindings for card visibility
            bool is_active =
                (new_state == PrintJobState::PRINTING || new_state == PrintJobState::PAUSED);
            int active_val = is_active ? 1 : 0;
            if (lv_subject_get_int(&print_active_) != active_val) {
                lv_subject_set_int(&print_active_, active_val);

                // Safety: When print becomes inactive, ensure print_start_phase is IDLE
                // This prevents "Preparing Print" from showing when print is finished
                if (!is_active) {
                    int phase = lv_subject_get_int(&print_start_phase_);
                    if (phase != static_cast<int>(PrintStartPhase::IDLE)) {
                        spdlog::warn(
                            "[PrinterPrintState] Safety reset: print inactive but phase={}, "
                            "resetting to IDLE",
                            phase);
                        lv_subject_set_int(&print_start_phase_,
                                           static_cast<int>(PrintStartPhase::IDLE));
                        lv_subject_copy_string(&print_start_message_, "");
                        lv_subject_set_int(&print_start_progress_, 0);
                    }
                }
            }

            // Update combined subject for home panel progress card visibility
            update_print_show_progress();
        }

        if (stats.contains("filename")) {
            std::string filename = stats["filename"].get<std::string>();
            lv_subject_copy_string(&print_filename_, filename.c_str());
        }

        // Update layer info from print_stats.info (sent by Moonraker/mock client)
        // Note: Moonraker can send null values for layer fields when not available
        if (stats.contains("info") && stats["info"].is_object()) {
            const auto& info = stats["info"];
            spdlog::trace("[LayerTracker] print_stats.info received: {}", info.dump());

            if (info.contains("current_layer") && info["current_layer"].is_number()) {
                int current_layer = info["current_layer"].get<int>();
                if (!has_real_layer_data_) {
                    spdlog::info("[LayerTracker] Receiving real layer data from print_stats.info");
                    has_real_layer_data_ = true;
                }
                if (current_layer != lv_subject_get_int(&print_layer_current_)) {
                    spdlog::debug("[LayerTracker] current_layer={} (from print_stats.info)",
                                  current_layer);
                }
                lv_subject_set_int(&print_layer_current_, current_layer);
            }

            if (info.contains("total_layer") && info["total_layer"].is_number()) {
                int total_layer = info["total_layer"].get<int>();
                if (total_layer != lv_subject_get_int(&print_layer_total_)) {
                    spdlog::debug("[LayerTracker] total_layer={} (from print_stats.info)",
                                  total_layer);
                }
                lv_subject_set_int(&print_layer_total_, total_layer);
            }
        } else if (stats.contains("info")) {
            spdlog::debug("[LayerTracker] print_stats.info is null/missing - slicer may not emit "
                          "SET_PRINT_STATS_INFO");
        }

        // Accept estimated_time from status updates (mock includes this; real Moonraker
        // sends it via file metadata API instead, handled by print status panel callback)
        if (stats.contains("estimated_time") && stats["estimated_time"].is_number()) {
            int est = static_cast<int>(stats["estimated_time"].get<double>());
            if (est > 0 && estimated_print_time_ == 0) {
                estimated_print_time_ = est;
                spdlog::debug("[PrinterPrintState] Estimated time from status: {}s", est);
            }
        }

        // Track filament usage (Moonraker reports in mm)
        if (stats.contains("filament_used") && stats["filament_used"].is_number()) {
            int filament_mm = static_cast<int>(stats["filament_used"].get<double>());
            if (filament_mm != lv_subject_get_int(&print_filament_used_)) {
                lv_subject_set_int(&print_filament_used_, filament_mm);
            }
        }

        // Update print time tracking (elapsed and remaining)
        if (stats.contains("print_duration") && stats["print_duration"].is_number()) {
            int print_seconds = static_cast<int>(stats["print_duration"].get<double>());
            lv_subject_set_int(&print_duration_, print_seconds);
        }

        // total_duration = wall-clock elapsed since job started (includes prep, pauses)
        if (stats.contains("total_duration") && stats["total_duration"].is_number()) {
            int total_elapsed = static_cast<int>(stats["total_duration"].get<double>());
            lv_subject_set_int(&print_elapsed_, total_elapsed);

            // Estimate remaining from progress using print_duration (actual print time),
            // NOT total_duration (which includes prep/preheat and inflates the estimate)
            int print_time = lv_subject_get_int(&print_duration_);
            int progress = lv_subject_get_int(&print_progress_);
            if (progress >= 1 && progress < 100 && print_time > 0) {
                int remaining =
                    static_cast<int>(static_cast<double>(print_time) * (100 - progress) / progress);

                // At very low progress (<5%), blend with slicer estimate to avoid
                // wild extrapolation from tiny samples (e.g. 30s at 1% → 50 min)
                if (progress < 5 && estimated_print_time_ > 0) {
                    // Linear blend: at 1% use 80% slicer, at 4% use 20% slicer
                    double slicer_weight = (5.0 - progress) / 5.0;
                    int slicer_remaining = estimated_print_time_ * (100 - progress) / 100;
                    remaining = static_cast<int>(slicer_weight * slicer_remaining +
                                                 (1.0 - slicer_weight) * remaining);
                }

                lv_subject_set_int(&print_time_left_, remaining);
            } else if (progress >= 1 && progress < 100 && print_time == 0 &&
                       estimated_print_time_ > 0) {
                // Fallback: use slicer estimate when print_duration hasn't started yet
                int remaining = estimated_print_time_ * (100 - progress) / 100;
                lv_subject_set_int(&print_time_left_, remaining);
            } else if (progress >= 100) {
                lv_subject_set_int(&print_time_left_, 0);
            }
        }
    }

    // Parse display_status (M73 progress + M117 message)
    if (status.contains("display_status")) {
        const auto& display = status["display_status"];
        if (display.contains("progress") && display["progress"].is_number()) {
            double raw = display["progress"].get<double>();
            slicer_progress_ = raw;
            if (raw > 0.0 && !slicer_progress_active_) {
                slicer_progress_active_ = true;
                spdlog::info("[PrinterPrintState] Slicer progress active (M73 detected)");
            }
        }
        if (display.contains("message")) {
            bool has_message = false;
            if (display["message"].is_string()) {
                const auto& msg = display["message"].get_ref<const std::string&>();
                lv_subject_copy_string(&display_message_, msg.c_str());
                has_message = !msg.empty();
            } else {
                // null or non-string — clear the message
                lv_subject_copy_string(&display_message_, "");
            }
            int visible = has_message ? 1 : 0;
            if (lv_subject_get_int(&display_message_visible_) != visible) {
                lv_subject_set_int(&display_message_visible_, visible);
            }
        }
    }

    // When slicer progress is active and display_status arrives without virtual_sdcard,
    // update print_progress_ directly from slicer value
    if (slicer_progress_active_ && status.contains("display_status") &&
        !status.contains("virtual_sdcard")) {
        int progress_pct = static_cast<int>(slicer_progress_ * 100.0 + 0.5);
        if (progress_pct > 100)
            progress_pct = 100;
        if (progress_pct < 0)
            progress_pct = 0;

        auto current_state = static_cast<PrintJobState>(lv_subject_get_int(&print_state_enum_));
        bool is_terminal_state =
            (current_state == PrintJobState::COMPLETE ||
             current_state == PrintJobState::CANCELLED || current_state == PrintJobState::ERROR);
        int current_progress = lv_subject_get_int(&print_progress_);
        if (!is_terminal_state || progress_pct >= current_progress) {
            lv_subject_set_int(&print_progress_, progress_pct);
        }
    }

    // Update print progress (virtual_sdcard) - processed AFTER print_stats
    if (status.contains("virtual_sdcard")) {
        const auto& sdcard = status["virtual_sdcard"];

        if (sdcard.contains("progress") && sdcard["progress"].is_number()) {
            int file_progress_pct = helix::units::json_to_percent(sdcard, "progress");

            // Update print_progress_ subject (skipped when slicer is active
            // but only file progress arrived — slicer is authoritative)
            bool skip_progress_update =
                slicer_progress_active_ && !status.contains("display_status");

            if (!skip_progress_update) {
                int progress_pct = file_progress_pct;
                if (slicer_progress_active_) {
                    // Slicer active and display_status present — use slicer value
                    progress_pct = static_cast<int>(slicer_progress_ * 100.0 + 0.5);
                    if (progress_pct > 100)
                        progress_pct = 100;
                    if (progress_pct < 0)
                        progress_pct = 0;
                }

                // Guard: Don't reset progress to 0 in terminal print states
                auto current_state =
                    static_cast<PrintJobState>(lv_subject_get_int(&print_state_enum_));
                bool is_terminal_state = (current_state == PrintJobState::COMPLETE ||
                                          current_state == PrintJobState::CANCELLED ||
                                          current_state == PrintJobState::ERROR);

                int current_progress = lv_subject_get_int(&print_progress_);
                if (!is_terminal_state || progress_pct >= current_progress) {
                    lv_subject_set_int(&print_progress_, progress_pct);
                }
            }

            // Layer estimation: always uses file_progress_pct (file position
            // correlates better with layer number than time-weighted progress)
            if (!has_real_layer_data_) {
                auto current_state =
                    static_cast<PrintJobState>(lv_subject_get_int(&print_state_enum_));
                bool is_terminal_state = (current_state == PrintJobState::COMPLETE ||
                                          current_state == PrintJobState::CANCELLED ||
                                          current_state == PrintJobState::ERROR);
                if (!is_terminal_state && file_progress_pct > 0) {
                    int total = lv_subject_get_int(&print_layer_total_);
                    if (total > 0) {
                        int estimated = (file_progress_pct * total + 50) / 100;
                        if (estimated < 1)
                            estimated = 1;
                        if (estimated > total)
                            estimated = total;
                        int current = lv_subject_get_int(&print_layer_current_);
                        if (estimated != current) {
                            spdlog::debug("[LayerTracker] Estimated layer {}/{} from progress {}%",
                                          estimated, total, file_progress_pct);
                            lv_subject_set_int(&print_layer_current_, estimated);
                        }
                    }
                }
            }
        }
    }
}

void PrinterPrintState::update_print_show_progress() {
    // Combined subject for home panel progress card visibility
    // Show progress card only when: print is active AND not in print start phase
    bool is_active = lv_subject_get_int(&print_active_) != 0;
    bool is_starting =
        lv_subject_get_int(&print_start_phase_) != static_cast<int>(PrintStartPhase::IDLE);
    int new_value = (is_active && !is_starting) ? 1 : 0;

    if (lv_subject_get_int(&print_show_progress_) != new_value) {
        lv_subject_set_int(&print_show_progress_, new_value);
        spdlog::trace(
            "[PrinterPrintState] print_show_progress updated: {} (active={}, starting={})",
            new_value, is_active, is_starting);
    }
}

// ============================================================================
// Setters
// ============================================================================

void PrinterPrintState::set_print_outcome(PrintOutcome outcome) {
    lv_subject_set_int(&print_outcome_, static_cast<int>(outcome));
    spdlog::debug("[PrinterPrintState] Print outcome set to: {}", static_cast<int>(outcome));
}

void PrinterPrintState::set_print_thumbnail_path(const std::string& path) {
    // Thumbnail path is set from PrintStatusPanel via ui_queue_update(),
    // so this runs on the main thread and can update the subject directly.
    if (path.empty()) {
        spdlog::debug("[PrinterPrintState] Clearing print thumbnail path");
    } else {
        spdlog::debug("[PrinterPrintState] Setting print thumbnail path: {}", path);
    }
    lv_subject_copy_string(&print_thumbnail_path_, path.c_str());
}

void PrinterPrintState::set_print_display_filename(const std::string& name) {
    // Display filename is set from PrintStatusPanel's main-thread callback.
    spdlog::trace("[PrinterPrintState] Setting print display filename: {}", name);
    lv_subject_copy_string(&print_display_filename_, name.c_str());
}

void PrinterPrintState::set_print_layer_total(int total) {
    lv_subject_set_int(&print_layer_total_, total);
}

void PrinterPrintState::set_print_layer_current(int layer) {
    spdlog::debug("[LayerTracker] set_print_layer_current({}) via gcode fallback", layer);
    helix::ui::queue_update([this, layer]() {
        if (!has_real_layer_data_) {
            spdlog::info("[LayerTracker] Receiving real layer data from gcode response");
            has_real_layer_data_ = true;
        }
        lv_subject_set_int(&print_layer_current_, layer);
    });
}

void PrinterPrintState::set_print_start_state(PrintStartPhase phase, const char* message,
                                              int progress) {
    spdlog::trace("[PrinterPrintState] Print start: phase={}, message='{}', progress={}%",
                  static_cast<int>(phase), message ? message : "", progress);

    // CRITICAL: Defer to main thread via ui_queue_update to avoid LVGL assertion
    // when subject updates trigger lv_obj_invalidate() during rendering.
    // This is called from WebSocket callbacks (background thread).
    std::string msg = message ? message : "";
    int clamped_progress = std::clamp(progress, 0, 100);
    helix::ui::queue_update([this, phase, msg, clamped_progress]() {
        // Reset print progress when transitioning from IDLE to a preparing phase
        // IMPORTANT: Read old_phase inside lambda for thread safety - avoids race
        // condition where another callback could modify print_start_phase_ between
        // the read and this lambda's execution.
        int old_phase = lv_subject_get_int(&print_start_phase_);
        if (old_phase == static_cast<int>(PrintStartPhase::IDLE) &&
            phase != PrintStartPhase::IDLE) {
            reset_for_new_print();
            // Clear terminal outcome immediately so cancel button shows during pre-print.
            // Without this, print_outcome stays COMPLETE/CANCELLED/ERROR until Moonraker
            // reports state=PRINTING (after PRINT_START finishes), leaving the reprint
            // button visible for the entire pre-print duration.
            auto current_outcome = static_cast<PrintOutcome>(lv_subject_get_int(&print_outcome_));
            if (current_outcome != PrintOutcome::NONE) {
                spdlog::info("[PrinterPrintState] Preparing new print - clearing outcome");
                lv_subject_set_int(&print_outcome_, static_cast<int>(PrintOutcome::NONE));
            }
        }
        lv_subject_set_int(&print_start_phase_, static_cast<int>(phase));
        if (!msg.empty()) {
            lv_subject_copy_string(&print_start_message_, msg.c_str());
        }
        lv_subject_set_int(&print_start_progress_, clamped_progress);
        update_print_show_progress();
    });
}

void PrinterPrintState::reset_print_start_state() {
    // CRITICAL: Defer to main thread via ui_queue_update
    helix::ui::queue_update([this]() {
        int phase = lv_subject_get_int(&print_start_phase_);
        if (phase != static_cast<int>(PrintStartPhase::IDLE)) {
            spdlog::debug("[PrinterPrintState] Resetting print start state to IDLE");
            lv_subject_set_int(&print_start_phase_, static_cast<int>(PrintStartPhase::IDLE));
            lv_subject_copy_string(&print_start_message_, "");
            lv_subject_set_int(&print_start_progress_, 0);
            update_print_show_progress();
        }
    });
}

void PrinterPrintState::set_print_in_progress(bool in_progress) {
    // Thread-safe wrapper: defer LVGL subject updates to main thread
    helix::ui::queue_update([this, in_progress]() { set_print_in_progress_internal(in_progress); });
}

void PrinterPrintState::set_print_start_time_left(const char* text) {
    if (text && text[0] != '\0') {
        lv_subject_copy_string(&print_start_time_left_, text);
    } else {
        lv_subject_copy_string(&print_start_time_left_, "");
    }
}

void PrinterPrintState::clear_print_start_time_left() {
    lv_subject_copy_string(&print_start_time_left_, "");
    lv_subject_set_int(&preprint_remaining_, 0);
    lv_subject_set_int(&preprint_elapsed_, 0);
}

void PrinterPrintState::set_preprint_remaining_seconds(int seconds) {
    lv_subject_set_int(&preprint_remaining_, std::max(0, seconds));
}

void PrinterPrintState::set_preprint_elapsed_seconds(int seconds) {
    lv_subject_set_int(&preprint_elapsed_, std::max(0, seconds));
}

void PrinterPrintState::set_estimated_print_time(int seconds) {
    estimated_print_time_ = std::max(0, seconds);
    spdlog::debug("[PrinterPrintState] Slicer estimated print time: {}s", estimated_print_time_);

    // Defer subject update to main thread: called from metadata callback (background thread).
    // lv_subject_set_int triggers observer chain which touches LVGL objects.
    int est = estimated_print_time_;
    helix::ui::queue_update([this, est]() {
        // Seed/update time_left with slicer estimate when progress is still 0%.
        // Once progress-based calculation kicks in (>=1%), it takes over.
        if (est > 0 && lv_subject_get_int(&print_progress_) == 0) {
            lv_subject_set_int(&print_time_left_, est);
            spdlog::debug("[PrinterPrintState] Seeded time_left with slicer estimate: {}s", est);
        }
    });
}

int PrinterPrintState::get_estimated_print_time() const {
    return estimated_print_time_;
}

void PrinterPrintState::set_print_in_progress_internal(bool in_progress) {
    int new_value = in_progress ? 1 : 0;
    if (lv_subject_get_int(&print_in_progress_) != new_value) {
        spdlog::trace("[PrinterPrintState] Print in progress: {}", in_progress);
        lv_subject_set_int(&print_in_progress_, new_value);
    }
}

// ============================================================================
// State queries
// ============================================================================

PrintJobState PrinterPrintState::get_print_job_state() const {
    // Note: lv_subject_get_int is thread-safe (atomic read)
    return static_cast<PrintJobState>(
        lv_subject_get_int(const_cast<lv_subject_t*>(&print_state_enum_)));
}

bool PrinterPrintState::can_start_new_print() const {
    // Check if a print workflow is already in progress (UI state)
    // This prevents double-tap issues during long G-code modification workflows
    if (is_print_in_progress()) {
        return false;
    }

    // Check printer's physical state
    PrintJobState state = get_print_job_state();
    // A new print can be started when printer is idle or previous print finished
    switch (state) {
    case PrintJobState::STANDBY:
    case PrintJobState::COMPLETE:
    case PrintJobState::CANCELLED:
    case PrintJobState::ERROR:
        return true;
    case PrintJobState::PRINTING:
    case PrintJobState::PAUSED:
        return false;
    default:
        // Unknown state - be conservative
        return false;
    }
}

bool PrinterPrintState::is_print_in_progress() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&print_in_progress_)) != 0;
}

bool PrinterPrintState::is_in_print_start() const {
    int phase = lv_subject_get_int(const_cast<lv_subject_t*>(&print_start_phase_));
    return phase != static_cast<int>(PrintStartPhase::IDLE);
}

} // namespace helix
