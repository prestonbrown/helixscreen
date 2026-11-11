// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_print_state.cpp
 * @brief Print state management extracted from PrinterState
 *
 * Manages print subjects including progress, state, timing, layers, and
 * print start phases. Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_print_state.h"

#include "config.h"
#include "core/data_root_resolver.h"
#include "core/gcode_filename.h"
#include "core/timer_guard.h" // lv_timer_cancel_safe
#include "core/update_queue.h"
#include "format_utils.h"
#include "moonraker_error.h"
#include "print_lifecycle_state.h"
#include "printer_state.h" // For enum definitions
#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace helix {

const char* PrinterPrintState::no_thumbnail_placeholder() {
    static const std::string path =
        helix::asset_component_uri("assets/images/benchy_thumbnail_white.png");
    return path.c_str();
}

PrinterPrintState::PrinterPrintState() {
    // Initialize string buffers
    std::memset(print_filename_buf_, 0, sizeof(print_filename_buf_));
    std::memset(print_display_filename_buf_, 0, sizeof(print_display_filename_buf_));
    std::memset(print_thumbnail_path_buf_, 0, sizeof(print_thumbnail_path_buf_));
    std::memset(print_progress_text_buf_, 0, sizeof(print_progress_text_buf_));
    std::memset(print_state_buf_, 0, sizeof(print_state_buf_));
    std::memset(print_start_message_buf_, 0, sizeof(print_start_message_buf_));
    std::memset(print_start_time_left_buf_, 0, sizeof(print_start_time_left_buf_));
    std::memset(display_message_buf_, 0, sizeof(display_message_buf_));
    std::memset(print_message_buf_, 0, sizeof(print_message_buf_));

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
    INIT_SUBJECT_INT(print_progress_display, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(print_progress_text, "0%", subjects_, register_xml);
    INIT_SUBJECT_INT(pause_markers_version, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(layer_pause_availability,
                     static_cast<int>(helix::layer_pause::Availability::Hidden), subjects_,
                     register_xml);
    INIT_SUBJECT_INT(layer_pause_layer, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(print_filename, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(print_state, "standby", subjects_, register_xml);
    // RAW_PRINT_STATE_OK: declaring the wire subject itself.
    INIT_SUBJECT_INT(print_state_enum, static_cast<int>(PrintJobState::STANDBY), subjects_,
                     register_xml);
    INIT_SUBJECT_INT(print_exception, -1, subjects_, register_xml);
    INIT_SUBJECT_INT(print_outcome, static_cast<int>(PrintOutcome::NONE), subjects_, register_xml);
    INIT_SUBJECT_INT(print_active, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(print_show_progress, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(print_display_filename, "", subjects_, register_xml);
    // Seeded with the placeholder rather than "": consumers bind this subject
    // straight to lv_image_set_src and observers fire once at registration, so
    // an empty initial value would be delivered to every consumer before the
    // first print. See no_thumbnail_placeholder() for why "" is unsafe there.
    INIT_SUBJECT_STRING(print_thumbnail_path, no_thumbnail_placeholder(), subjects_, register_xml);
    INIT_SUBJECT_INT(print_has_thumbnail, 0, subjects_, register_xml);
#if defined(HELIX_PLATFORM_ESP32)
    INIT_SUBJECT_INT(print_psram_thumb_gen, 0, subjects_, register_xml);
#endif

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
    INIT_SUBJECT_INT(print_start_chamber_wait, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(print_lifecycle, static_cast<int>(PrintState::Idle), subjects_, register_xml);
    INIT_SUBJECT_INT(job_holds_machine, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(machine_motion_blocked, spool_latch_active_.load() ? 1 : 0, subjects_,
                     register_xml);
    INIT_SUBJECT_INT(spool_latch, spool_latch_active_.load() ? 1 : 0, subjects_, register_xml);
    INIT_SUBJECT_INT(preparing_epoch, 0, subjects_, false);
    INIT_SUBJECT_INT(preparing_start, static_cast<int>(PreparingStart::None), subjects_, false);
    INIT_SUBJECT_INT(print_identity_epoch, 0, subjects_, false);
    INIT_SUBJECT_INT(print_state_report_seq, 0, subjects_, false);
    INIT_SUBJECT_INT(print_lifecycle_prev, static_cast<int>(PrintState::Idle), subjects_, false);
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

    // Klipper print_stats.message — pause/error reason from firmware
    INIT_SUBJECT_STRING(print_message, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(print_stop_reason, "", subjects_, register_xml);
    INIT_SUBJECT_INT(print_stop_reason_visible, 0, subjects_, register_xml);

    // Power-loss recovery: the firmware's own validated-snapshot flag
    // (plr_parse_snapshot_valid). Default 0; stays 0 on firmware without one.
    INIT_SUBJECT_INT(plr_snapshot_valid, 0, subjects_, register_xml);

    // Power-loss recovery: the active probe's capability marker
    // (plr_parse_power_loss_signal). Default 0; latched to 1 by presence of the
    // key (see update_from_status).
    INIT_SUBJECT_INT(plr_power_loss_signal, 0, subjects_, register_xml);

    // Power-loss recovery, passive-backend half: the discovered resume macro
    // (capability, set from discovery by PrinterState::set_hardware) and the
    // interrupted flag (availability, parsed in update_from_status via
    // plr_backend). plr_backend owns which firmware carries them. Default 0.
    INIT_SUBJECT_INT(plr_resume_macro, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(plr_interrupted_flag, 0, subjects_, register_xml);

    // Pre-populate per-extruder filament_used map. Freezing the map structure
    // here eliminates the BG-thread emplace vs UI-thread read rehash race
    // (see header). update_from_status and the accessor both do direct
    // lookups and never mutate the map after this point.
    for (int idx = 0; idx < kMaxExtruderScan; ++idx) {
        create_extruder_filament_entry(idx);
    }

    subjects_initialized_ = true;
    spdlog::trace("[PrinterPrintState] Subjects initialized successfully");
}

void PrinterPrintState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[PrinterPrintState] Deinitializing subjects");

    // A printer switch deinits here: another printer's starts never answer for this one.
    pending_start_.reset();
    confirmed_start_.reset();

    // Expire any setter callbacks still queued on the UpdateQueue. They capture
    // `this` and write the subjects being torn down below; without this the next
    // drain notifies a freed observer list (#1165, #1146).
    async_lifetime_.invalidate();

    // Per-extruder filament_used subjects are dynamic: for each entry, signal
    // subject death BEFORE deinit so all ObserverGuards (including those still
    // holding shared_ptr copies) detect the subject as dead and skip
    // lv_observer_remove() on the about-to-be-freed observers (#816).
    for (auto& [idx, info] : extruder_filament_used_) {
        if (info.lifetime) {
            *info.lifetime = false;
        }
        info.lifetime.reset();
        if (info.subject) {
            lv_subject_deinit(info.subject.get());
        }
    }
    extruder_filament_used_.clear();

#if defined(HELIX_PLATFORM_ESP32)
    // Release the PSRAM thumbnail while LVGL is still initialized — the
    // destructor calls lv_image_cache_drop(), which is only valid before
    // lv_deinit(). StaticSubjectRegistry runs this on the main thread.
    print_psram_thumbnail_.reset();
#endif

    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterPrintState::create_extruder_filament_entry(int extruder_idx) {
    ExtruderFilamentInfo info;
    info.subject = std::make_unique<lv_subject_t>();
    lv_subject_init_int(info.subject.get(), 0);
    info.lifetime = std::make_shared<bool>(true);
    extruder_filament_used_.emplace(extruder_idx, std::move(info));
}

lv_subject_t* PrinterPrintState::get_extruder_filament_used_subject(int extruder_idx,
                                                                    SubjectLifetime& lifetime) {
    // Direct lookup only — the map is frozen after init_subjects().
    // No emplace on the UI thread, no races with the BG-thread status updater.
    auto it = extruder_filament_used_.find(extruder_idx);
    if (it == extruder_filament_used_.end()) {
        spdlog::warn("[PrinterPrintState] get_extruder_filament_used_subject({}) out of range or "
                     "subjects not initialized (MAX_EXTRUDER_SCAN={})",
                     extruder_idx, kMaxExtruderScan);
        return nullptr;
    }
    lifetime = it->second.lifetime;
    return it->second.subject.get();
}

void PrinterPrintState::publish_progress_display(int percent) {
    if (progress_frozen_) {
        return;
    }
    lv_subject_set_int(&print_progress_display_, percent);
    char buf[sizeof(print_progress_text_buf_)];
    helix::format::format_percent(percent, buf, sizeof(buf));
    if (strcmp(lv_subject_get_string(&print_progress_text_), buf) != 0) {
        lv_subject_copy_string(&print_progress_text_, buf);
    }
}

void PrinterPrintState::commit_progress(int percent) {
    if (percent > 100) {
        percent = 100;
    }
    if (percent < 0) {
        percent = 0;
    }

    // print_stats is parsed before this runs, and publish_lifecycle_state() with
    // it, so the lifecycle already describes THIS payload rather than the
    // previous tick's.
    const PrintState lifecycle = get_print_lifecycle();

    // A paused print is not advancing, so no payload arriving during the pause
    // carries a new position. Klipper expires the M73 value behind
    // display_status.progress and substitutes virtual_sdcard's byte position into
    // the same field, and a pause is precisely when M73 stops being refreshed. On
    // a multi-material print that byte position runs far ahead of the slicer's
    // time estimate - the wipe tower is dense and quick - so accepting it here
    // walks the bar to a number the print has not reached, then walks it back on
    // resume.
    if (lifecycle == PrintState::Paused) {
        return;
    }

    // A finished print never counts down: a late sample a fraction short of where
    // it stopped must not drag the number backwards.
    const bool is_terminal_state =
        (lifecycle == PrintState::Complete || lifecycle == PrintState::Cancelled ||
         lifecycle == PrintState::Error);

    const int current_progress = lv_subject_get_int(&print_progress_);
    if ((!is_terminal_state || percent >= current_progress) && current_progress != percent) {
        lv_subject_set_int(&print_progress_, percent);
    }
    publish_progress_display(percent);
}

void PrinterPrintState::freeze_progress_display(bool complete) {
    if (complete) {
        // A finished print is 100% even when the last virtual_sdcard sample
        // Moonraker sent was a fraction short.
        progress_frozen_ = false;
        publish_progress_display(100);
    }
    progress_frozen_ = true;
    spdlog::debug("[PrinterPrintState] Progress display frozen at {}%",
                  lv_subject_get_int(&print_progress_display_));
}

void PrinterPrintState::bump_pause_markers_version() {
    lv_subject_set_int(&pause_markers_version_, lv_subject_get_int(&pause_markers_version_) + 1);
}

void PrinterPrintState::set_pause_scan(helix::gcode::PauseScanPtr scan,
                                       const std::string& source_filename) {
    if (!scan) {
        return;
    }
    pause_scan_ = std::move(scan);
    pause_source_filename_ = source_filename;
    bump_pause_markers_version();
    spdlog::debug("[PrinterPrintState] Pause scan published: {} pauses, {} layer marks (axis={})",
                  pause_scan_->pauses.size(), pause_scan_->layer_marks.size(),
                  pause_scan_->axis == helix::gcode::ProgressAxis::SlicerTime ? "slicer" : "bytes");
}

std::optional<float> PrinterPrintState::layer_pause_marker_fraction() const {
    const int layer = lv_subject_get_int(const_cast<lv_subject_t*>(&layer_pause_layer_));
    if (layer <= 0 || !pause_scan_matches_current_file()) {
        return std::nullopt;
    }
    const auto* mark = helix::gcode::find_layer_mark(*pause_scan_, layer);
    if (!mark) {
        return std::nullopt;
    }
    return helix::gcode::display_fraction(*mark, pause_scan_->axis);
}

void PrinterPrintState::set_layer_pause_state_object(std::string object) {
    layer_pause_state_object_ = std::move(object);
    layer_pause_.reset();
    refresh_layer_pause();
}

bool PrinterPrintState::file_may_report_layer(int layer) const {
    if (!pause_scan_matches_current_file()) {
        return true;
    }
    return helix::gcode::find_layer_mark(*pause_scan_, layer) != nullptr;
}

void PrinterPrintState::note_layer_pause_next_layer(bool enable) {
    layer_pause_.note_next_layer(enable, slicer_current_layer_);
    refresh_layer_pause();
}

void PrinterPrintState::note_layer_pause_at_layer(int layer) {
    layer_pause_.note_at_layer(layer);
    refresh_layer_pause();
}

void PrinterPrintState::refresh_layer_pause() {
    if (!subjects_initialized_) {
        return;
    }
    // The macros compare against print_stats.info only, so every layer here is
    // that numbering, never print_layer_current_'s other sources.
    const int current = slicer_current_layer_;
    const int total = slicer_total_layers_;
    layer_pause_.on_layer(current);
    // RAW_PRINT_STATE_OK: the pause the macro caused is the printer's own.
    const bool paused = get_print_job_state() == PrintJobState::PAUSED;
    if (paused && !layer_pause_seen_paused_) {
        layer_pause_.on_paused(current);
    }
    layer_pause_seen_paused_ = paused;

    const auto availability = helix::layer_pause::classify(lv_subject_get_int(&print_active_) != 0,
                                                           slicer_layers_reported_, current, total);
    if (lv_subject_get_int(&layer_pause_availability_) != static_cast<int>(availability)) {
        lv_subject_set_int(&layer_pause_availability_, static_cast<int>(availability));
    }

    // A schedule only means something while a print runs that can fire it.
    const int pending = availability == helix::layer_pause::Availability::Hidden
                            ? 0
                            : helix::layer_pause::pending_layer(layer_pause_.effective(), current);
    if (lv_subject_get_int(&layer_pause_layer_) != pending) {
        spdlog::debug("[PrinterPrintState] Layer pause pending at layer {} (current {})", pending,
                      current);
        lv_subject_set_int(&layer_pause_layer_, pending);
        bump_pause_markers_version();
    }
}

void PrinterPrintState::unfreeze_progress_display() {
    progress_frozen_ = false;
    publish_progress_display(0);
}

void PrinterPrintState::reset_for_new_print() {
    // Clear stale print PROGRESS data when starting a new print.
    // The preparing overlay covers the UI, so stale data isn't visible.
    // IMPORTANT: Do NOT clear print_filename_ or print_display_filename_ here!
    // Clearing filename triggers ActivePrintMediaManager to wipe the thumbnail we just set.
    // Filename is Moonraker's source of truth - it updates when the print actually starts.
    lv_subject_set_int(&print_progress_, 0);
    unfreeze_progress_display();
    // The pause scan belongs to the previous job's file. A scan for the new
    // file may still be in flight; the publish-side filename guard keeps its
    // late result off the new print if the file differs.
    if (pause_scan_) {
        pause_scan_.reset();
        pause_source_filename_.clear();
        bump_pause_markers_version();
    }
    lv_subject_set_int(&print_layer_current_, 0);
    has_real_layer_data_ = false;
    slicer_layers_reported_ = false;
    slicer_current_layer_ = 0;
    slicer_total_layers_ = 0;
    layer_pause_.reset_for_new_print();
    layer_source_ = LayerSource::Estimate;
    // Commanded Z belongs to the print run, not the file — clear it. Do NOT
    // clear layer_height_/first_layer_height_: like print_layer_total_ and
    // estimated_print_time_ they belong to the file and must survive a same-file
    // reprint (whose metadata callback won't re-fire).
    have_gcode_z_ = false;
    last_gcode_z_mm_ = 0.0;
    print_clock_s_ = 0.0;
    height_layer_pending_ = {};
    layer_z_derived_ = false;
    slicer_progress_ = 0.0;
    slicer_progress_active_ = false;
    lv_subject_copy_string(&print_message_, "");
    set_print_stop_reason("");
    lv_subject_set_int(&print_duration_, 0);
    lv_subject_set_int(&print_elapsed_, 0);
    lv_subject_set_int(&print_filament_used_, 0);
    // Re-seed time_left from slicer estimate instead of clearing to 0.
    // For same-file reprints, the metadata callback won't re-fire since
    // the filename hasn't changed, so we preserve the previous estimate.
    // For different files, the metadata callback updates both values.
    lv_subject_set_int(&print_time_left_, estimated_print_time_);
    // DON'T clear estimated_print_time_ - it belongs to the file, not the session
    // Reset the estimator so it seeds from the first real measurement
    eta_estimator_.reset();
    refresh_layer_pause();
    spdlog::trace("[PrinterPrintState] Reset print progress for new print (slicer_est={}s)",
                  estimated_print_time_);
}

void PrinterPrintState::apply_layer_current(int layer, LayerSource kind, const char* source) {
    const int current = lv_subject_get_int(&print_layer_current_);
    // A status frame lags the gcode echo: while the subject holds a value the
    // echo just reported, a lower status number is the stale frame, not the
    // print going backward. Echo writes are never refused (sequential
    // one-object-at-a-time prints restart ;LAYER:N per object), and a decrease
    // against a status-sourced value is a genuine report from a status-only
    // printer. The guard judges the value IN the subject, so it compares
    // against the previous write's provenance, not this call's kind.
    if (layer < current && kind == LayerSource::Reported &&
        lv_subject_get_int(&print_active_) != 0 && layer_source_ == LayerSource::Echo) {
        spdlog::debug("[LayerTracker] Refusing layer decrease {} -> {} (from {})", current, layer,
                      source);
        return;
    }
    if (layer != current) {
        lv_subject_set_int(&print_layer_current_, layer);
    }
    // Provenance now traces to whichever source addressed the subject last,
    // including on an equal write.
    layer_source_ = kind;
}

bool PrinterPrintState::status_indicates_active_print(const nlohmann::json& status) {
    if (!status.contains("print_stats")) {
        return false;
    }
    const auto& ps = status["print_stats"];
    if (!ps.contains("state") || !ps["state"].is_string()) {
        return false;
    }
    // Defined in terms of the enum predicate so the subject's notion of "active"
    // and every enum-side caller can never drift apart. parse_print_job_state()
    // maps an unknown string to STANDBY, which lands on false as before.
    const std::string st = ps["state"].get<std::string>();
    return printer_has_job(parse_print_job_state(st.c_str()));
}

void PrinterPrintState::clear_print_exception() {
    print_exception_id_ = -1;
    print_exception_code_ = -1;
    print_exception_message_.clear();
    if (lv_subject_get_int(&print_exception_) != -1) {
        lv_subject_set_int(&print_exception_, -1);
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

void PrinterPrintState::set_print_stop_reason(const std::string& reason) {
    if (reason != lv_subject_get_string(&print_stop_reason_)) {
        lv_subject_copy_string(&print_stop_reason_, reason.c_str());
    }
    const int visible = reason.empty() ? 0 : 1;
    if (lv_subject_get_int(&print_stop_reason_visible_) != visible) {
        lv_subject_set_int(&print_stop_reason_visible_, visible);
    }
    update_display_message_visible();
}

void PrinterPrintState::update_display_message_visible() {
    // The row mirrors display_status.message and is shown whenever there is
    // text. Pre-print is deliberately included: PRINT_START macros are where
    // most M117 traffic originates. The collector's phase label lives in a
    // separate subject (print_start_message), so there is nothing to duplicate.
    // While a pause has a reason, that reason is the status: a macro's M117
    // ("Print starting...") left over from before the pause would read as if
    // the print were running.
    // RAW_PRINT_STATE_OK: the printer's own pause, which the reason explains.
    const bool paused_with_reason =
        static_cast<PrintJobState>(lv_subject_get_int(&print_state_enum_)) ==
            PrintJobState::PAUSED &&
        lv_subject_get_int(&print_stop_reason_visible_) != 0;
    int new_value =
        (!paused_with_reason && strcmp(lv_subject_get_string(&display_message_), "") != 0) ? 1 : 0;
    if (lv_subject_get_int(&display_message_visible_) != new_value) {
        lv_subject_set_int(&display_message_visible_, new_value);
    }
}

// ============================================================================
// Setters
// ============================================================================

void PrinterPrintState::set_print_outcome(PrintOutcome outcome) {
    int val = static_cast<int>(outcome);
    if (lv_subject_get_int(&print_outcome_) != val) {
        lv_subject_set_int(&print_outcome_, val);
        spdlog::debug("[PrinterPrintState] Print outcome set to: {}", val);
    }
}

void PrinterPrintState::recompute_effective_print_filename(const std::string& raw) {
    const std::string before = effective_print_filename_;

    // An override describes ONE print. Retire it once what the printer reports
    // stops being explainable by it, so print A's identity cannot bleed into
    // print B - the failure this whole authority exists to prevent. A preparing
    // job is exactly the case where the mismatch is legitimate: print_stats
    // still names the PREVIOUS job for the whole pre-start block, which is why
    // the identity is recorded at commit in the first place.
    if (!has_preparing_job() && !print_identity_override_.empty() && !raw.empty() &&
        !gcode::thumbnail_source_describes(raw, print_identity_override_)) {
        spdlog::debug("[PrinterPrintState] Identity override '{}' does not describe '{}' - "
                      "retiring it",
                      print_identity_override_, raw);
        print_identity_override_.clear();
    }

    if (printer_side_pre_start_ && lv_subject_get_int(&print_active_) == 0) {
        // Another app's start, before its file opens: print_stats still names the
        // previous job, and nothing names this one yet.
        effective_print_filename_.clear();
    } else if (!print_identity_override_.empty()) {
        effective_print_filename_ = print_identity_override_;
    } else {
        // No override: a rewritten path still has to resolve, because a print we
        // prepared can be reported by a process that never committed it - a
        // restart or reconnect mid-print, where nothing installed one at all.
        effective_print_filename_ = gcode::resolve_gcode_filename(raw);
    }

    publish_identity_if_changed(before);
}

void PrinterPrintState::publish_identity_if_changed(const std::string& before) {
    if (effective_print_filename_ == before) {
        return;
    }
    spdlog::debug("[PrinterPrintState] Effective print filename: '{}' -> '{}'", before,
                  effective_print_filename_);
    lv_subject_set_int(&print_identity_epoch_, ++print_identity_epoch_counter_);
}

void PrinterPrintState::set_printer_side_pre_start(bool live) {
    if (printer_side_pre_start_ == live) {
        return;
    }
    printer_side_pre_start_ = live;
    const std::string before = effective_print_filename_;
    recompute_effective_print_filename(lv_subject_get_string(&print_filename_));
    // After a Klipper restart the reported name is already empty, so the
    // identity string does not move, but what it means does: announce it.
    if (effective_print_filename_ == before) {
        lv_subject_set_int(&print_identity_epoch_, ++print_identity_epoch_counter_);
    }
}

void PrinterPrintState::set_print_identity_override(const std::string& name) {
    // Resolve on store: a caller may hand over the rewritten name directly
    // (Reprint replays whatever print_stats last reported), and storing that raw
    // would put `modified_1748..._orig` on screen. Identity for a clean name.
    print_identity_override_ = name.empty() ? name : gcode::resolve_gcode_filename(name);
    spdlog::debug("[PrinterPrintState] Identity override set to: {}",
                  print_identity_override_.empty() ? "(cleared)" : print_identity_override_);
    recompute_effective_print_filename(lv_subject_get_string(&print_filename_));
}

void PrinterPrintState::clear_print_identity_override() {
    if (print_identity_override_.empty()) {
        return;
    }
    print_identity_override_.clear();
    spdlog::debug("[PrinterPrintState] Identity override released");
    recompute_effective_print_filename(lv_subject_get_string(&print_filename_));
}

void PrinterPrintState::set_print_thumbnail(const std::string& for_file, const std::string& path) {
    // Callers marshal to the main thread (ui_queue_update / token.defer) before
    // reaching here, so the subject can be updated directly.
    if (path.empty()) {
        spdlog::debug("[PrinterPrintState] Clearing print thumbnail path for '{}'", for_file);
    } else {
        spdlog::debug("[PrinterPrintState] Setting print thumbnail path for '{}': {}", for_file,
                      path);
    }
    // Order matters: for_file must be visible BEFORE any observer of the path
    // subject runs, so observers can trust it describes the path they see.
    print_thumbnail_file_ = for_file;
    if (strcmp(lv_subject_get_string(&print_thumbnail_path_), path.c_str()) != 0) {
        lv_subject_copy_string(&print_thumbnail_path_, path.c_str());
    }
    update_has_thumbnail();
}

bool PrinterPrintState::has_own_thumbnail(const char* path, bool psram_thumbnail) {
    if (psram_thumbnail) {
        return true;
    }
    return path != nullptr && path[0] != '\0' && strcmp(path, no_thumbnail_placeholder()) != 0;
}

void PrinterPrintState::update_has_thumbnail() {
#if defined(HELIX_PLATFORM_ESP32)
    const bool psram = print_psram_thumbnail_ != nullptr;
#else
    const bool psram = false;
#endif
    const int has = has_own_thumbnail(lv_subject_get_string(&print_thumbnail_path_), psram) ? 1 : 0;
    if (lv_subject_get_int(&print_has_thumbnail_) != has) {
        lv_subject_set_int(&print_has_thumbnail_, has);
    }
}

#if defined(HELIX_PLATFORM_ESP32)
void PrinterPrintState::set_print_psram_thumbnail(
    std::shared_ptr<helix::ui::EspPsramThumbnail> thumb) {
    // Main thread only (see header). The assignment below may run the previous
    // thumbnail's destructor, which calls lv_image_cache_drop().
    print_psram_thumbnail_ = std::move(thumb);
    spdlog::debug("[PrinterPrintState] PSRAM thumbnail {}",
                  print_psram_thumbnail_ ? "installed" : "cleared");
    lv_subject_set_int(&print_psram_thumb_gen_, lv_subject_get_int(&print_psram_thumb_gen_) + 1);
    update_has_thumbnail();
}
#endif

void PrinterPrintState::set_print_display_filename(const std::string& name) {
    // Display filename is set from PrintStatusPanel's main-thread callback.
    spdlog::trace("[PrinterPrintState] Setting print display filename: {}", name);
    if (strcmp(lv_subject_get_string(&print_display_filename_), name.c_str()) != 0) {
        lv_subject_copy_string(&print_display_filename_, name.c_str());
    }
}

void PrinterPrintState::set_print_layer_total(int total) {
    async_lifetime_.defer("PrinterPrintState::set_print_layer_total",
                          [this, total]() { lv_subject_set_int(&print_layer_total_, total); });
}

void PrinterPrintState::set_print_layer_heights(double layer_height, double first_layer_height) {
    async_lifetime_.defer(
        "PrinterPrintState::set_print_layer_heights", [this, layer_height, first_layer_height]() {
            layer_height_ = layer_height;
            first_layer_height_ = (first_layer_height > 0.0) ? first_layer_height : layer_height;
        });
}

void PrinterPrintState::set_print_materials(std::vector<std::string> materials) {
    async_lifetime_.defer("PrinterPrintState::set_print_materials",
                          [this, materials = std::move(materials)]() mutable {
                              print_materials_ = std::move(materials);
                          });
}

void PrinterPrintState::set_print_layer_current(int layer) {
    spdlog::debug("[LayerTracker] set_print_layer_current({}) via gcode fallback", layer);
    async_lifetime_.defer("PrinterPrintState::set_print_layer_current", [this, layer]() {
        if (!has_real_layer_data_) {
            spdlog::info("[LayerTracker] Receiving real layer data from gcode response");
            has_real_layer_data_ = true;
        }
        printer_reports_layers_ = true; // sticky printer capability
        apply_layer_current(layer, LayerSource::Echo, "gcode response");
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
    async_lifetime_.defer("PrinterPrintState::set_print_start_state", [this, phase, msg,
                                                                       clamped_progress]() {
        // Guard: reject non-IDLE phase updates when print is no longer active,
        // UNLESS this is a new print starting (transitioning from IDLE phase).
        // This prevents a deferred COMPLETE callback from arriving after the print
        // has ended and the safety reset has already cleared the phase to IDLE,
        // while still allowing new prints to start from an inactive state.
        int current_phase = lv_subject_get_int(&print_start_phase_);
        bool is_new_print_start = (current_phase == static_cast<int>(PrintStartPhase::IDLE));
        // A live preparing job means WE started this and the printer has not
        // caught up yet - print_active is legitimately 0 for the whole
        // host-side pre-start block, and dropping these updates freezes the
        // overlay on its first phase. A start the printer announced itself is
        // in the same window without a job of ours.
        if (phase != PrintStartPhase::IDLE && lv_subject_get_int(&print_active_) == 0 &&
            !is_new_print_start && !has_preparing_job() && !printer_side_pre_start_) {
            spdlog::debug("[PrinterPrintState] Ignoring stale phase {} (print inactive)",
                          static_cast<int>(phase));
            return;
        }

        // Reset print progress when transitioning from IDLE to a preparing phase
        // (current_phase was already read above for the stale-update guard)
        if (is_new_print_start && phase != PrintStartPhase::IDLE) {
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
        if (phase != PrintStartPhase::HEATING_CHAMBER) {
            set_print_start_chamber_wait(false);
        }
        if (lv_subject_get_int(&print_start_phase_) != static_cast<int>(phase)) {
            lv_subject_set_int(&print_start_phase_, static_cast<int>(phase));
            publish_lifecycle_state();
            update_display_message_visible();
        }
        if (!msg.empty() &&
            strcmp(lv_subject_get_string(&print_start_message_), msg.c_str()) != 0) {
            lv_subject_copy_string(&print_start_message_, msg.c_str());
        }
        lv_subject_set_int(&print_start_progress_, clamped_progress);
        update_print_show_progress();
    });
}

void PrinterPrintState::reset_print_start_state() {
    // CRITICAL: Defer to main thread via ui_queue_update
    async_lifetime_.defer("PrinterPrintState::reset_print_start_state", [this]() {
        int phase = lv_subject_get_int(&print_start_phase_);
        if (phase != static_cast<int>(PrintStartPhase::IDLE)) {
            spdlog::debug("[PrinterPrintState] Resetting print start state to IDLE");
            lv_subject_set_int(&print_start_phase_, static_cast<int>(PrintStartPhase::IDLE));
            lv_subject_set_int(&print_start_chamber_wait_, 0);
            publish_lifecycle_state();
            lv_subject_copy_string(&print_start_message_, "");
            lv_subject_set_int(&print_start_progress_, 0);
            update_print_show_progress();
            update_display_message_visible();
        }
    });
}

void PrinterPrintState::set_print_start_chamber_wait(bool waiting) {
    const int value = waiting ? 1 : 0;
    if (lv_subject_get_int(&print_start_chamber_wait_) != value) {
        lv_subject_set_int(&print_start_chamber_wait_, value);
    }
}

void PrinterPrintState::set_print_in_progress(bool in_progress) {
    // Thread-safe wrapper: defer LVGL subject updates to main thread
    async_lifetime_.defer("PrinterPrintState::set_print_in_progress",
                          [this, in_progress]() { set_print_in_progress_internal(in_progress); });
}

void PrinterPrintState::set_print_start_time_left(const char* text) {
    const char* new_text = (text && text[0] != '\0') ? text : "";
    if (strcmp(lv_subject_get_string(&print_start_time_left_), new_text) != 0) {
        lv_subject_copy_string(&print_start_time_left_, new_text);
    }
}

void PrinterPrintState::clear_print_start_time_left() {
    if (strcmp(lv_subject_get_string(&print_start_time_left_), "") != 0) {
        lv_subject_copy_string(&print_start_time_left_, "");
    }
    lv_subject_set_int(&preprint_remaining_, 0);
    lv_subject_set_int(&preprint_elapsed_, 0);
}

void PrinterPrintState::set_preprint_remaining_seconds(int seconds) {
    int val = std::max(0, seconds);
    lv_subject_set_int(&preprint_remaining_, val);
}

void PrinterPrintState::set_preprint_elapsed_seconds(int seconds) {
    int val = std::max(0, seconds);
    lv_subject_set_int(&preprint_elapsed_, val);
}

void PrinterPrintState::set_estimated_print_time(int seconds) {
    estimated_print_time_ = std::max(0, seconds);
    spdlog::debug("[PrinterPrintState] Slicer estimated print time: {}s", estimated_print_time_);

    // Defer subject update to main thread: called from metadata callback (background thread).
    // lv_subject_set_int triggers observer chain which touches LVGL objects.
    int est = estimated_print_time_;
    async_lifetime_.defer("PrinterPrintState::set_estimated_print_time", [this, est]() {
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

// RAW_PRINT_STATE_OK: the wire accessor itself. get_print_lifecycle() below is
// the derived one, and each is paired with its own subject on purpose.
PrintJobState PrinterPrintState::get_print_job_state() const {
    // Note: lv_subject_get_int is thread-safe (atomic read)
    return static_cast<PrintJobState>(
        lv_subject_get_int(const_cast<lv_subject_t*>(&print_state_enum_)));
}

PrintState PrinterPrintState::get_print_lifecycle() const {
    return static_cast<PrintState>(
        lv_subject_get_int(const_cast<lv_subject_t*>(&print_lifecycle_)));
}

bool PrinterPrintState::can_start_new_print() const {
    // Check if a print workflow is already in progress (UI state)
    // This prevents double-tap issues during long G-code modification workflows
    if (is_print_in_progress()) {
        return false;
    }
    // The printer is running a start another app sent, before its file opens.
    if (printer_side_pre_start_) {
        return false;
    }

    // Check printer's physical state. RAW_PRINT_STATE_OK: the preparing window
    // is already covered by the is_print_in_progress() early-return above, so
    // this arm answers the narrower question of what the printer itself reports.
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

const char* preparing_exit_name(PreparingExit reason) {
    switch (reason) {
    case PreparingExit::Confirmed:
        return "confirmed";
    case PreparingExit::Superseded:
        return "superseded";
    case PreparingExit::Failed:
        return "failed";
    case PreparingExit::Cancelled:
        return "cancelled";
    case PreparingExit::TimedOut:
        return "timed out";
    case PreparingExit::EndedByPrinter:
        return "ended by the printer";
    }
    return "?";
}

void PrinterPrintState::reconcile_preparing() {
    if (preparing_job_.empty()) {
        return;
    }
    // Reconciliation asks the wire question deliberately: the claim is settled by
    // the PRINTER taking a job, which is exactly what printer_has_job() answers.
    // Widening to job_holds_machine() would count our own Preparing state and
    // settle the claim against itself.
    //
    // PAUSED settles it as surely as PRINTING. Klipper reaches paused only from
    // an accepted job, and a PRINT_START that pauses its own file - M25 while an
    // asynchronous soak runs from [delayed_gcode], firmware power-loss recovery
    // restoring a job, a runout during the purge line - is a job the printer is
    // holding. Requiring PRINTING left those claims armed until the half-hour
    // watchdog retired them as TimedOut, and that path cools both heaters: a
    // 65 minute ABS soak lost its bed 30 minutes in (#1365).
    const auto job_state = static_cast<PrintJobState>(lv_subject_get_int(&print_state_enum_));
    if (!printer_has_job(job_state)) {
        return; // Only a job the printer has taken settles this.
    }
    const char* reported = lv_subject_get_string(&print_filename_);
    if (!reported || reported[0] == '\0') {
        return; // Held, but not yet named - wait for the name.
    }

    // Compare on the bare name: the report may be path-qualified, and a
    // modification rewrite hands the printer a temp file standing in for the
    // file the user chose.
    auto bare = [](const std::string& p) {
        const std::string resolved = gcode::resolve_gcode_filename(p);
        return gcode::get_filename_basename(resolved);
    };

    const bool mine = bare(reported) == bare(preparing_job_.filename);
    retire_preparing(mine ? PreparingExit::Confirmed : PreparingExit::Superseded);
}

void PrinterPrintState::set_preparing_start(PreparingStart start) {
    if (lv_subject_get_int(&preparing_start_) != static_cast<int>(start)) {
        lv_subject_set_int(&preparing_start_, static_cast<int>(start));
    }
}

void PrinterPrintState::note_preparing_start_sent() {
    if (!preparing_job_.empty()) {
        set_preparing_start(PreparingStart::InFlight);
    }
}

void PrinterPrintState::note_start_plate(const std::string& printer_id, const std::string& filename,
                                         int plate) {
    if (preparing_job_.empty()) {
        return; // no job for the printer to confirm
    }
    pending_start_ = StartRecord{printer_id, filename, plate, preparing_epoch_counter_};
}

std::string PrinterPrintState::start_record_printer_id() {
    const Config* config = Config::get_instance();
    return config ? config->get_active_printer_id() : std::string();
}

std::optional<int> PrinterPrintState::confirmed_start_plate(const std::string& printer_id,
                                                            const std::string& filename) const {
    if (!confirmed_start_ || filename.empty() || confirmed_start_->printer_id != printer_id ||
        confirmed_start_->filename != filename) {
        return std::nullopt;
    }
    return confirmed_start_->plate;
}

void PrinterPrintState::note_preparing_start_answered(int epoch, bool accepted) {
    if (epoch == 0 || epoch != preparing_epoch_counter_ || preparing_job_.empty()) {
        return;
    }
    set_preparing_start(accepted ? PreparingStart::Accepted : PreparingStart::None);
    if (!accepted) {
        pending_start_.reset();
    }
}

bool PrinterPrintState::hold_unanswered_start(int epoch, const MoonrakerError& error) {
    if (epoch == 0 || epoch != preparing_epoch_counter_ || preparing_job_.empty()) {
        return false;
    }
    // A dropped connection may also be a start that never left the screen, so
    // only a Stop waiting to cancel it is reason to keep the job.
    const bool held =
        error.command_may_still_run() || (error.is_transport_loss() && preparing_cancel_requested_);
    if (!held) {
        return false;
    }
    spdlog::warn("[PrinterPrintState] Start of '{}' lost its answer ({}){} - keeping the job "
                 "until the printer reports it",
                 preparing_job_.filename, error.message,
                 preparing_cancel_requested_ ? " with a cancel requested" : "");
    set_preparing_start(PreparingStart::Unanswered);
    return true;
}

void PrinterPrintState::begin_preparing(const PrintJobRef& job) {
    if (job.empty()) {
        spdlog::warn("[PrinterPrintState] begin_preparing called with no job");
        return;
    }
    spdlog::info("[PrinterPrintState] Preparing '{}'", job.filename);
    preparing_job_ = job;
    preparing_cancel_requested_ = false;
    set_preparing_start(PreparingStart::None);

    // Adopt this job's identity NOW, not when the printer first reports a name.
    // print_stats still names the PREVIOUS job for the whole pre-start block, and
    // for a print we rewrote it will never name the original at all. Doing it
    // here rather than in each consumer's own epoch observer is the point: the
    // panel and the media manager each grew one, they diverged, and a print
    // started from the app resolved its media through the finished print
    // (prestonbrown/helixscreen#1339).
    set_print_identity_override(job.full_path());

    // Synchronous: see the header. The previous job's terminal state has to be
    // gone before any observer can paint a Preparing state beside it.
    reset_for_new_print();
    unfreeze_progress_display();
    if (static_cast<PrintOutcome>(lv_subject_get_int(&print_outcome_)) != PrintOutcome::NONE) {
        lv_subject_set_int(&print_outcome_, static_cast<int>(PrintOutcome::NONE));
    }
    lv_subject_set_int(&print_start_phase_, static_cast<int>(PrintStartPhase::INITIALIZING));
    lv_subject_set_int(&print_start_progress_, 0);
    publish_lifecycle_state();
    update_display_message_visible();

    // The preparing window IS what print_in_progress documents itself as - true
    // from the start request until the print actually starts or fails. It used
    // to be set by hand and cleared on eighteen separate exit paths in
    // PrintPreparationManager; a missed path latched it true and
    // can_start_new_print() then refused every later print for the session.
    set_print_in_progress_internal(true);
    arm_preparing_watchdog();

    // Published last: observers of the epoch read preparing_job() and the
    // cleared state, so everything they can see is already consistent.
    lv_subject_set_int(&preparing_epoch_, ++preparing_epoch_counter_);
}

PrinterPrintState::~PrinterPrintState() {
    cancel_preparing_watchdog();
}

void PrinterPrintState::arm_preparing_watchdog() {
    cancel_preparing_watchdog();
    preparing_watchdog_ = lv_timer_create(preparing_watchdog_cb, kPreparingWatchdogMs, this);
    if (preparing_watchdog_) {
        lv_timer_set_repeat_count(preparing_watchdog_, 1); // one-shot
    }
}

void PrinterPrintState::cancel_preparing_watchdog() {
    if (!preparing_watchdog_) {
        return;
    }
    // lv_timer_cancel_safe self-guards on lv_is_initialized() and neuters
    // instead of unlinking, so this is safe from a destructor and from inside
    // lv_timer_handler (#750/#751).
    helix::ui::lv_timer_cancel_safe(preparing_watchdog_);
    preparing_watchdog_ = nullptr;
}

void PrinterPrintState::preparing_watchdog_cb(lv_timer_t* timer) {
    auto* self = static_cast<PrinterPrintState*>(lv_timer_get_user_data(timer));
    if (!self) {
        return;
    }
    // One-shot: LVGL deletes the timer after this returns, so drop our handle
    // first rather than cancelling an object about to be freed.
    self->preparing_watchdog_ = nullptr;
    spdlog::warn("[PrinterPrintState] Preparing job '{}' never confirmed - timing out",
                 self->preparing_job_.filename);
    self->retire_preparing(PreparingExit::TimedOut);
}

void PrinterPrintState::retire_preparing(PreparingExit reason) {
    if (preparing_job_.empty()) {
        return;
    }
    spdlog::info("[PrinterPrintState] Retiring preparing job '{}': {}", preparing_job_.filename,
                 preparing_exit_name(reason));
    // The printer's last job is ours only when it confirms the one we sent;
    // a job it took instead replaces whatever ours was.
    if (reason == PreparingExit::Confirmed) {
        const bool sent = pending_start_ && pending_start_->epoch == preparing_epoch_counter_;
        confirmed_start_ = sent ? pending_start_ : std::nullopt;
    } else if (reason == PreparingExit::Superseded) {
        confirmed_start_.reset();
    }
    pending_start_.reset();
    preparing_job_ = {};
    preparing_cancel_requested_ = false;
    set_preparing_start(PreparingStart::None);
    last_preparing_exit_ = reason;
    cancel_preparing_watchdog();
    set_print_in_progress_internal(false);

    // Confirmed means the printer took OUR job, so the override still describes
    // what is printing - and for a rewritten copy it is the ONLY thing that
    // does, since print_stats reports the rewrite. Every other reason means the
    // job never ran, and leaving it set would resolve the next print through a
    // job that never happened.
    if (reason != PreparingExit::Confirmed) {
        clear_print_identity_override();
    }

    // Confirmed hands off to a real print, which owns the phase from here. Every
    // other reason means no print is coming, so the pre-print UI must come down.
    if (reason != PreparingExit::Confirmed) {
        lv_subject_set_int(&print_start_phase_, static_cast<int>(PrintStartPhase::IDLE));
        lv_subject_copy_string(&print_start_message_, "");
        lv_subject_set_int(&print_start_progress_, 0);
        update_display_message_visible();
    }
    publish_lifecycle_state();
    lv_subject_set_int(&preparing_epoch_, 0);
}

void PrinterPrintState::publish_lifecycle_state() {
    const auto job_state = static_cast<PrintJobState>(lv_subject_get_int(&print_state_enum_));
    const int phase = lv_subject_get_int(&print_start_phase_);
    const int derived = static_cast<int>(derive_print_state(job_state, phase));
    const int current = lv_subject_get_int(&print_lifecycle_);

    // Written BEFORE print_lifecycle, not after, and not from an observer on it.
    //
    // lv_subject_set_int() notifies synchronously, so any consumer reached by
    // print_lifecycle's observers runs before a later write here would land -
    // publishing the boolean second would let a widget built inside such an
    // observer see the stale value. Ordering it first closes that, and is safe
    // in the other direction because job_holds_machine has only XML consumers
    // (bind_state_if_eq), none of which reads print_lifecycle back.
    const int holds = job_holds_machine(static_cast<PrintState>(derived)) ? 1 : 0;
    lv_subject_set_int(&job_holds_machine_, holds);
    publish_machine_motion_blocked();

    if (current != derived) {
        // Previous first, so an observer of the current value already sees a
        // consistent pair when it fires.
        lv_subject_set_int(&print_lifecycle_prev_, current);
        lv_subject_set_int(&print_lifecycle_, derived);
    }
}

void PrinterPrintState::publish_machine_motion_blocked() {
    const int blocked =
        (lv_subject_get_int(&job_holds_machine_) != 0 || spool_latch_active_.load()) ? 1 : 0;
    lv_subject_set_int(&machine_motion_blocked_, blocked);
}

void PrinterPrintState::set_spool_latch(bool on, std::vector<std::string> extra_tokens) {
    {
        std::lock_guard<std::mutex> lock(spool_latch_mutex_);
        spool_latch_tokens_ = on ? std::move(extra_tokens) : std::vector<std::string>{};
    }
    spool_latch_active_.store(on);
    lv_subject_set_int(&spool_latch_, on ? 1 : 0);
    publish_machine_motion_blocked();
}

bool PrinterPrintState::is_in_print_start() const {
    int phase = lv_subject_get_int(const_cast<lv_subject_t*>(&print_start_phase_));
    return phase != static_cast<int>(PrintStartPhase::IDLE);
}

} // namespace helix
