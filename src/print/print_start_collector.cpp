// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_collector.h"

#include "ui_update_queue.h"

#include "config.h"
#include "format_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "memory_monitor.h"
#include "printer_detector.h"
#include "thermal_rate_model.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <ctime>

using namespace helix;

using json = nlohmann::json;

// Config path for pre-print prediction history
static constexpr const char* PREPRINT_HISTORY_PATH = "/print_start_history/entries";

// ============================================================================
// STATIC PATTERN DEFINITIONS
// ============================================================================

// Pattern to detect PRINT_START macro invocation
const std::regex
    PrintStartCollector::print_start_pattern_(R"(PRINT_START|START_PRINT|_PRINT_START)",
                                              std::regex::icase);

// Pattern to detect print start completion (first layer indicator)
// Includes HELIX:READY for our custom macro integration
const std::regex PrintStartCollector::completion_pattern_(
    R"(SET_PRINT_STATS_INFO\s+CURRENT_LAYER=|LAYER:?\s*1\b|;LAYER:1|First layer|HELIX:READY)",
    std::regex::icase);

// Pattern to detect RESPOND-based print start completion (authoritative signal)
// Matches messages containing "print" + "start"/"started"/"starting" adjacent in either word order
const std::regex PrintStartCollector::respond_completion_pattern_(
    R"(\bprint\b\W+\b(start|started|starting)\b|\b(start|started|starting)\b\W+\bprint\b)",
    std::regex::icase);

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

PrintStartCollector::PrintStartCollector(MoonrakerClient& client, PrinterState& state)
    : client_(client), state_(state) {
    spdlog::debug("[PrintStartCollector] Constructed");
}

PrintStartCollector::~PrintStartCollector() {
    // Don't call stop() here - it uses client_ and state_ references which may
    // already be destroyed during static destruction order. Callers should
    // explicitly call stop() before letting the shared_ptr go out of scope.
    // Just mark as inactive to prevent any pending callbacks from running.
    active_.store(false);
    registered_.store(false);

    // Safe to delete timer here (doesn't use client_ or state_)
    if (eta_timer_) {
        lv_timer_delete(eta_timer_);
        eta_timer_ = nullptr;
    }
}

// ============================================================================
// PUBLIC METHODS
// ============================================================================

void PrintStartCollector::start() {
    if (active_.load()) {
        spdlog::debug("[PrintStartCollector] Already active, ignoring start()");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // Record start time for timeout fallback
        printing_state_start_ = std::chrono::steady_clock::now();
        detected_phases_.clear();
        current_phase_ = PrintStartPhase::INITIALIZING;
        print_start_detected_ = false;
        max_sequential_progress_ = 0;
        phase_enter_times_.clear();
        // Reset mesh probe tracking
        mesh_probe_current_ = 0;
        mesh_probe_total_ = 0;
        mesh_probe_fallback_count_ = 0;
        mesh_first_probe_time_ = {};
        mesh_last_probe_time_ = {};
        mesh_seconds_per_probe_ = 0.0f;
        mesh_has_last_probe_pos_ = false;
        pre_mesh_probe_count_ = 0;
        pre_mesh_last_probe_time_ = {};
        // Snapshot stale subject values so fallbacks only trigger on real changes
        baseline_layer_ = lv_subject_get_int(state_.get_print_layer_current_subject());
        baseline_progress_ = lv_subject_get_int(state_.get_print_progress_subject());
    }
    fallbacks_enabled_.store(false); // Will be enabled after initial window
    spdlog::info("[PrintStartCollector] Baselines: layer={}, progress={}", baseline_layer_,
                 baseline_progress_);
    helix::MemoryMonitor::log_now("print_start_collector_start", spdlog::level::debug);

    // Capture start temperatures for heating fraction calculation
    start_ext_temp_ = lv_subject_get_int(state_.get_active_extruder_temp_subject()) / 10;
    start_bed_temp_ = lv_subject_get_int(state_.get_bed_temp_subject()) / 10;
    cached_ext_temp_.store(start_ext_temp_, std::memory_order_relaxed);
    cached_bed_temp_.store(start_bed_temp_, std::memory_order_relaxed);
    cached_ext_target_.store(lv_subject_get_int(state_.get_active_extruder_target_subject()) / 10,
                             std::memory_order_relaxed);
    cached_bed_target_.store(lv_subject_get_int(state_.get_bed_target_subject()) / 10,
                             std::memory_order_relaxed);
    last_remaining_ = 0;
    fallback_completion_ = false;

    // Reset thermal rate models with current temperatures
    {
        auto& mgr = ThermalRateManager::instance();
        mgr.get_model("extruder").reset(static_cast<float>(start_ext_temp_));
        mgr.get_model("heater_bed").reset(static_cast<float>(start_bed_temp_));
    }

    // Load prediction history from config
    load_prediction_history();

    // Compute duration-proportional weights from predictions + thermal model
    compute_predicted_weights();

    // Ensure we have a profile for pattern matching
    if (!profile_) {
        // Try printer-specific profile first (printer type may not have been known at init time)
        std::string printer_type = state_.get_printer_type();
        if (!printer_type.empty()) {
            std::string profile_name = PrinterDetector::get_print_start_profile(printer_type);
            if (!profile_name.empty()) {
                profile_ = PrintStartProfile::load(profile_name);
                spdlog::info("[PrintStartCollector] Loaded profile '{}' for '{}'", profile_name,
                             printer_type);
            }
        }
        if (!profile_) {
            profile_ = PrintStartProfile::load_default();
        }
    }

    auto self = shared_from_this();

    // Register gcode response handler if not already registered (stays registered
    // permanently — on_gcode_response() returns early when !active_)
    if (handler_name_.empty()) {
        static std::atomic<uint64_t> s_collector_id{0};
        handler_name_ = "print_start_collector_" + std::to_string(++s_collector_id);
        client_.register_method_callback("notify_gcode_response", handler_name_,
                                         [self](const json& msg) { self->on_gcode_response(msg); });
    }

    // Register for printer status updates (fallback for printers with KAMP/custom macros)
    // This watches for _START_PRINT.print_started, START_PRINT.preparation_done, etc.
    macro_subscription_id_ = client_.register_notify_update([self](const json& notification) {
        if (!self->active_.load())
            return;

        if (!notification.contains("params") || !notification["params"].is_array() ||
            notification["params"].empty()) {
            return;
        }

        const auto& status = notification["params"][0];

        // Check _START_PRINT.print_started (AD5M KAMP macro)
        if (status.contains("gcode_macro _START_PRINT")) {
            const auto& macro = status["gcode_macro _START_PRINT"];
            if (macro.contains("print_started") && macro["print_started"].is_boolean() &&
                macro["print_started"].get<bool>()) {
                spdlog::info("[PrintStartCollector] Macro signal: print_started=true");
                self->update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
                return;
            }
        }

        // Check START_PRINT.preparation_done
        if (status.contains("gcode_macro START_PRINT")) {
            const auto& macro = status["gcode_macro START_PRINT"];
            if (macro.contains("preparation_done") && macro["preparation_done"].is_boolean() &&
                macro["preparation_done"].get<bool>()) {
                spdlog::info("[PrintStartCollector] Macro signal: preparation_done=true");
                self->update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
                return;
            }
        }

        // Check _HELIX_STATE.print_started (our custom macro)
        if (status.contains("gcode_macro _HELIX_STATE")) {
            const auto& macro = status["gcode_macro _HELIX_STATE"];
            if (macro.contains("print_started") && macro["print_started"].is_boolean() &&
                macro["print_started"].get<bool>()) {
                spdlog::info("[PrintStartCollector] Helix macro signal: print_started=true");
                self->update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
                return;
            }
        }

        // Pass through display_status.message (M117) during preparation.
        // Many firmware/macros send M117 messages like "Homing...", "Probing...",
        // "Heating bed..." during startup that provide useful status info.
        if (status.contains("display_status")) {
            const auto& ds = status["display_status"];
            if (ds.contains("message") && ds["message"].is_string()) {
                const auto& msg = ds["message"].get_ref<const std::string&>();
                if (!msg.empty()) {
                    spdlog::debug("[PrintStartCollector] display_status.message: {}", msg);
                    // Update message without changing the current phase
                    self->update_message_only(msg);
                }
            }
        }
    });

    registered_.store(true);
    active_.store(true);

    // Set initial state
    state_.set_print_start_state(PrintStartPhase::INITIALIZING, lv_tr("Preparing Print..."), 0);

    // Create LVGL timer for periodic elapsed + ETA updates (runs on main thread)
    {
        eta_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* collector = static_cast<PrintStartCollector*>(lv_timer_get_user_data(timer));
                if (collector) {
                    collector->update_eta_display();
                    collector->check_fallback_completion();
                }
            },
            ETA_UPDATE_INTERVAL_MS, this);
        spdlog::debug("[PrintStartCollector] ETA timer created ({}ms interval)",
                      ETA_UPDATE_INTERVAL_MS);
    }
    // Run first update immediately
    update_eta_display();

    spdlog::debug("[PrintStartCollector] Started monitoring (handler: {})", handler_name_);
}

void PrintStartCollector::stop() {
    // Mark inactive first to stop callbacks from processing
    bool was_active = active_.exchange(false);
    bool was_registered = registered_.exchange(false);

    // Keep gcode response handler registered permanently — it checks active_ and
    // returns early when not collecting. This avoids the race where early responses
    // from START_PRINT are lost because the handler isn't registered yet.

    // Unregister macro subscription using atomic exchange to prevent double-unsubscribe
    SubscriptionId sub_id = macro_subscription_id_.exchange(0);
    if (sub_id != 0) {
        client_.unsubscribe_notify_update(sub_id);
        spdlog::debug("[PrintStartCollector] Unsubscribed from status updates");
    }

    fallbacks_enabled_.store(false);

    // Delete ETA timer (must be on main thread, but stop() is always called from main thread)
    if (eta_timer_) {
        lv_timer_delete(eta_timer_);
        eta_timer_ = nullptr;
    }

    if (was_active) {
        state_.clear_print_start_time_left();
        state_.reset_print_start_state();
        spdlog::debug("[PrintStartCollector] Stopped monitoring");
    }
}

void PrintStartCollector::reset() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        detected_phases_.clear();
        current_phase_ = PrintStartPhase::INITIALIZING;
        print_start_detected_ = false;
        max_sequential_progress_ = 0;
        printing_state_start_ = std::chrono::steady_clock::now();
        phase_enter_times_.clear();
        mesh_probe_current_ = 0;
        mesh_probe_total_ = 0;
        mesh_probe_fallback_count_ = 0;
        mesh_first_probe_time_ = {};
        mesh_last_probe_time_ = {};
        mesh_seconds_per_probe_ = 0.0f;
        mesh_has_last_probe_pos_ = false;
        pre_mesh_probe_count_ = 0;
        pre_mesh_last_probe_time_ = {};
    }
    fallbacks_enabled_.store(false);

    if (active_.load()) {
        state_.set_print_start_state(PrintStartPhase::INITIALIZING, lv_tr("Preparing Print..."), 0);
    }

    spdlog::debug("[PrintStartCollector] Reset state");
}

void PrintStartCollector::enable_fallbacks() {
    if (!active_.load())
        return;

    fallbacks_enabled_.store(true);
    spdlog::debug("[PrintStartCollector] Fallback detection enabled");

    // Don't immediately check fallback conditions here.
    // set_print_start_state() defers reset_for_new_print() via async invoke,
    // so stale subject data (layer count, progress) from the previous print
    // hasn't been cleared yet. Let fallback checks be triggered naturally by
    // incoming data updates (observer callbacks on layer/progress subjects).
}

void PrintStartCollector::complete_from_external_signal(const char* source) {
    if (!active_.load()) {
        return;
    }
    spdlog::info("[PrintStartCollector] External completion signal: {}", source);
    update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
}

void PrintStartCollector::check_fallback_completion() {
    if (!active_.load() || !fallbacks_enabled_.load()) {
        return;
    }

    // Check current phase under lock
    std::chrono::steady_clock::time_point start_time;
    PrintStartPhase current;
    bool print_start_was_detected;
    float predicted_total;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // Already complete - nothing to do
        if (current_phase_ == PrintStartPhase::COMPLETE) {
            return;
        }
        current = current_phase_;
        print_start_was_detected = print_start_detected_;
        start_time = printing_state_start_;
        predicted_total = predicted_total_seconds_;
    }

    // Get temperature data for proactive and completion fallback checks
    int ext_temp = lv_subject_get_int(state_.get_active_extruder_temp_subject());
    int ext_target = lv_subject_get_int(state_.get_active_extruder_target_subject());
    int bed_temp = lv_subject_get_int(state_.get_bed_temp_subject());
    int bed_target = lv_subject_get_int(state_.get_bed_target_subject());

    // Cache for thread-safe access from calculate_progress_locked()
    cached_ext_temp_.store(ext_temp / 10, std::memory_order_relaxed);
    cached_ext_target_.store(ext_target / 10, std::memory_order_relaxed);
    cached_bed_temp_.store(bed_temp / 10, std::memory_order_relaxed);
    cached_bed_target_.store(bed_target / 10, std::memory_order_relaxed);

    // Recompute predicted weights when heater targets increase from 0.
    // This handles macros that heat bed first, then issue M109 later —
    // at start() time the nozzle target is 0, so HEATING_NOZZLE gets no weight.
    // Only recompute on 0→positive transitions to avoid churn from temporary
    // M104 S0 (nozzle cooldown for probe cleaning) that would remove the
    // heating phase and cause progress regression.
    {
        int new_ext = ext_target / 10;
        int new_bed = bed_target / 10;
        bool ext_newly_set = (weights_ext_target_ == 0 && new_ext > 0);
        bool bed_newly_set = (weights_bed_target_ == 0 && new_bed > 0);
        if (ext_newly_set || bed_newly_set) {
            spdlog::info("[PrintStartCollector] Heater targets changed "
                         "(ext: {}→{}°C, bed: {}→{}°C), recomputing weights",
                         weights_ext_target_, new_ext, weights_bed_target_, new_bed);
            compute_predicted_weights();
        }
    }

    // Temps are in decidegrees (value * 10), targets may be 0 if not set
    bool bed_heating = bed_target > 0 && bed_temp < bed_target - TEMP_TOLERANCE_DECIDEGREES;
    bool nozzle_heating = ext_target > 0 && ext_temp < ext_target - TEMP_TOLERANCE_DECIDEGREES;
    bool temps_ready = !bed_heating && !nozzle_heating;

    // =========================================================================
    // PROACTIVE DETECTION: Track preparation phases from temperature state
    // Continuously updates as heaters change, not just on first detection.
    // Only fires when no gcode_response patterns have been matched recently.
    // =========================================================================
    // Allow proactive temperature detection from any non-terminal phase.
    // On printers like the K1C where gcode responses are sparse, the proactive
    // detector is the primary way to advance through heating phases.
    bool is_temp_detected_phase =
        (current != PrintStartPhase::COMPLETE && current != PrintStartPhase::PURGING);
    if (is_temp_detected_phase) {
        if (bed_heating && (current != PrintStartPhase::HEATING_BED)) {
            // Bed still heating — show bed phase
            spdlog::info("[PrintStartCollector] Proactive: bed heating ({}/{})", bed_temp / 10,
                         bed_target / 10);
            update_phase(PrintStartPhase::HEATING_BED, lv_tr("Heating Bed..."));
            return;
        }
        if (nozzle_heating && !bed_heating && current != PrintStartPhase::HEATING_NOZZLE) {
            // Bed done (or no bed target), nozzle still heating
            spdlog::info("[PrintStartCollector] Proactive: nozzle heating ({}/{})", ext_temp / 10,
                         ext_target / 10);
            update_phase(PrintStartPhase::HEATING_NOZZLE, lv_tr("Heating Nozzle..."));
            return;
        }
        if (temps_ready && !bed_heating && !nozzle_heating &&
            (current == PrintStartPhase::HEATING_BED ||
             current == PrintStartPhase::HEATING_NOZZLE)) {
            // Both heaters at target — preparation continues (homing, mesh, purge, etc.)
            spdlog::info("[PrintStartCollector] Proactive: temps ready, waiting for print start");
            update_phase(PrintStartPhase::INITIALIZING, lv_tr("Preparing Print..."));
            return;
        }
        if ((bed_heating || nozzle_heating) && current == PrintStartPhase::IDLE) {
            // First detection — heaters just started ramping
            spdlog::info("[PrintStartCollector] Proactive: initializing (heaters ramping)");
            update_phase(PrintStartPhase::INITIALIZING, lv_tr("Preparing Print..."));
            return;
        }
    }

    // =========================================================================
    // COMPLETION DETECTION: Detect when PRINT_START is done
    // =========================================================================

    // Suppress timeout while mesh probing is actively progressing.
    // During mesh, the nozzle target may be 0 (macro cleared it for cooldown),
    // making temps_near unreliable. Active probing = we're making real progress.
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (current_phase_ == PrintStartPhase::BED_MESH &&
            (mesh_probe_current_ > 0 || mesh_probe_fallback_count_ > 0)) {
            auto since_last = std::chrono::steady_clock::now() - mesh_last_probe_time_;
            if (since_last < MESH_PROBE_GAP_RESET) {
                return; // Active probing — don't timeout
            }
        }
    }

    // Fallback 3: Adaptive timeout with temp awareness
    //
    // ext_target == 0 during macro execution means the nozzle heat command (M109)
    // hasn't been issued yet — NOT that no nozzle heating is needed. Bed-first
    // macros (common on AD5M with ABS) heat bed, then home + mesh, then heat
    // nozzle. Treating ext_target=0 as "nozzle satisfied" causes premature timeout.
    bool nozzle_target_set = ext_target > 0;
    bool nozzle_near = nozzle_target_set && ext_temp >= static_cast<int>(ext_target * 0.9);
    bool bed_near = bed_target <= 0 || bed_temp >= static_cast<int>(bed_target * 0.9);
    // temps_near requires nozzle target to be set AND near — unknown nozzle
    // target (ext_target=0) means we can't confirm temps are ready
    bool temps_near = nozzle_near && bed_near;

    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

    // Determine effective timeout: use prediction data when available,
    // FALLBACK_TIMEOUT only when we have no information at all
    if (predicted_total > 0) {
        // Adaptive timeout from prediction data (with margin for variance)
        auto adaptive_timeout =
            std::chrono::seconds(static_cast<int>(predicted_total * ADAPTIVE_TIMEOUT_MARGIN));

        if (elapsed > adaptive_timeout && temps_near) {
            spdlog::info("[PrintStartCollector] Fallback: adaptive timeout ({} sec, "
                         "predicted={:.0f}s)",
                         elapsed_sec, predicted_total);
            fallback_completion_ = true;
            update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
            return;
        }

        // Absolute ceiling based on predictions (something is seriously wrong)
        auto absolute_timeout =
            std::chrono::seconds(static_cast<int>(predicted_total * ABSOLUTE_TIMEOUT_MARGIN));
        absolute_timeout = std::max(absolute_timeout, ABSOLUTE_MAX_TIMEOUT);
        if (elapsed > absolute_timeout) {
            spdlog::warn("[PrintStartCollector] Fallback: absolute timeout ({} sec, "
                         "predicted={:.0f}s)",
                         elapsed_sec, predicted_total);
            fallback_completion_ = true;
            update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
            return;
        }
    } else {
        // No prediction data — FALLBACK_TIMEOUT is the last resort
        if (elapsed > FALLBACK_TIMEOUT && temps_near) {
            spdlog::info("[PrintStartCollector] Fallback: timeout ({} sec, no predictions)",
                         elapsed_sec);
            fallback_completion_ = true;
            update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
            return;
        }

        // Hard ceiling when we have no predictions AND nozzle target unknown
        if (elapsed > ABSOLUTE_MAX_TIMEOUT) {
            spdlog::warn("[PrintStartCollector] Fallback: absolute timeout ({} sec, "
                         "no predictions, nozzle_target_set={})",
                         elapsed_sec, nozzle_target_set);
            fallback_completion_ = true;
            update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
            return;
        }
    }
}

// ============================================================================
// PRIVATE METHODS
// ============================================================================

void PrintStartCollector::on_gcode_response(const json& msg) {
    if (!active_.load()) {
        return;
    }

    // Parse notify_gcode_response format: {"method": "...", "params": ["line"]}
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
        return;
    }

    const std::string& line = msg["params"][0].get_ref<const std::string&>();

    // Skip empty lines and common noise
    if (line.empty() || line == "ok") {
        return;
    }

    spdlog::trace("[PrintStartCollector] G-code: {}", line);

    // Check for HELIX:PHASE signals (highest priority - definitive signals from plugin/macros)
    if (check_helix_phase_signal(line)) {
        return; // Signal handled
    }

    // Check for K2/CFS-specific gcode tag stream (purge percent, box filament load).
    // These tags are emitted by Creality firmware and carry per-step progress that
    // universal heuristics can't infer. Falls through on stock Klipper printers.
    if (check_k2_cfs_signal(line)) {
        return; // Signal handled
    }

    // Check for RESPOND-based print start completion (authoritative signal)
    // Many users end PRINT_START macros with RESPOND MSG="Print started!" or similar.
    // This fires before Moonraker's state transition and is a definitive end-of-preprint signal.
    if (is_respond_completion(line)) {
        spdlog::info("[PrintStartCollector] RESPOND completion detected: {}", line);
        update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
        return;
    }

    // Check profile signal formats (priority 2, after HELIX:PHASE)
    if (profile_) {
        PrintStartProfile::MatchResult match;
        if (profile_->try_match_signal(line, match)) {
            if (profile_->progress_mode() == PrintStartProfile::ProgressMode::SEQUENTIAL) {
                update_phase(match.phase, match.message, match.progress);
            } else {
                update_phase(match.phase, match.message.c_str());
            }
            return;
        }
    }

    // Check for PRINT_START marker (once per session)
    bool should_set_initializing = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!print_start_detected_ && is_print_start_marker(line)) {
            print_start_detected_ = true;
            should_set_initializing = true;
        }
    }
    if (should_set_initializing) {
        update_phase(PrintStartPhase::INITIALIZING, lv_tr("Starting Print..."));
        spdlog::info("[PrintStartCollector] PRINT_START detected");
        return;
    }

    // Check for bed mesh probe progress (sub-phase tracking within BED_MESH).
    // Also detects BED_MESH phase entry from probe lines on printers that don't
    // emit a separate BED_MESH_CALIBRATE command line before probing starts.
    {
        bool in_mesh = false;
        bool in_leveling = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            in_mesh = (current_phase_ == PrintStartPhase::BED_MESH);
            // QGL and Z_TILT also fire "probe at X,Y is z=Z" lines via Klipper's
            // shared probe routine. Voron 2.4 QGL probes 4 corner pads with
            // `samples: 3` = 12 probe lines back-to-back, well over the
            // pre-mesh entry threshold. Without this guard the collector
            // would enter BED_MESH on the third QGL probe and count corner
            // pads against the bed_mesh probe total, throwing the displayed
            // "X / Y" count off by the number of leveling probes.
            in_leveling = (current_phase_ == PrintStartPhase::QGL ||
                           current_phase_ == PrintStartPhase::Z_TILT);
        }

        bool is_probe_line = !in_mesh && !in_leveling &&
                             (helix::parse_probe_progress(line) ||
                              helix::is_probe_result_line(line));

        // If we see a probe line but aren't in BED_MESH yet, buffer probes
        // before entering the phase. Some printers (e.g. AD5M Klipper mod)
        // emit 1-2 PROBE commands for nozzle wipe before mesh calibration.
        // Require MESH_PROBE_ENTRY_THRESHOLD consecutive probes to confirm
        // this is actual mesh probing, not an isolated operation.
        if (is_probe_line) {
            bool should_enter = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                auto now = std::chrono::steady_clock::now();

                // Gap reset for pre-mesh buffer
                if (pre_mesh_probe_count_ > 0 &&
                    (now - pre_mesh_last_probe_time_) > MESH_PROBE_GAP_RESET) {
                    spdlog::debug("[PrintStartCollector] Pre-mesh probe gap reset "
                                  "({}s since last probe)",
                                  std::chrono::duration_cast<std::chrono::seconds>(
                                      now - pre_mesh_last_probe_time_)
                                      .count());
                    pre_mesh_probe_count_ = 0;
                }

                pre_mesh_probe_count_++;
                pre_mesh_last_probe_time_ = now;

                if (pre_mesh_probe_count_ >= MESH_PROBE_ENTRY_THRESHOLD) {
                    should_enter = true;
                    // Seed mesh tracking with buffered count so probes aren't lost
                    mesh_probe_fallback_count_ = pre_mesh_probe_count_;
                    mesh_probe_current_ = pre_mesh_probe_count_;
                    mesh_first_probe_time_ = now;
                    mesh_last_probe_time_ = now;
                } else {
                    spdlog::debug("[PrintStartCollector] Pre-mesh probe {}/{} (buffering)",
                                  pre_mesh_probe_count_, MESH_PROBE_ENTRY_THRESHOLD);
                }
            }

            if (should_enter) {
                update_phase(PrintStartPhase::BED_MESH, lv_tr("Bed Mesh..."));
                in_mesh = true;
                // This probe line was already counted in the buffer transfer above,
                // so skip the per-probe counting below and return.
                char msg_buf[64];
                int count, total;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    count = mesh_probe_fallback_count_;
                    total = mesh_probe_total_;
                }
                if (total > 0) {
                    snprintf(msg_buf, sizeof(msg_buf), "%s (%d/%d)", lv_tr("Bed Mesh"), count,
                             total);
                } else {
                    snprintf(msg_buf, sizeof(msg_buf), "%s (%d)", lv_tr("Bed Mesh"), count);
                }
                state_.set_print_start_state(PrintStartPhase::BED_MESH, msg_buf,
                                             calculate_progress());
                return;
            }
            // Below threshold — don't enter BED_MESH yet, skip probe counting
            return;
        }

        if (in_mesh) {
            if (auto pp = helix::parse_probe_progress(line)) {
                // "Probing point X/Y" — firmware provides current and total
                int progress;
                char msg_buf[64];
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    auto now = std::chrono::steady_clock::now();

                    // Gap reset: if >30s since last probe, earlier probes were
                    // from a different operation (e.g. nozzle wipe)
                    if (mesh_probe_current_ > 0 &&
                        (now - mesh_last_probe_time_) > MESH_PROBE_GAP_RESET) {
                        spdlog::info("[PrintStartCollector] Mesh probe gap reset "
                                     "({}s since last probe)",
                                     std::chrono::duration_cast<std::chrono::seconds>(
                                         now - mesh_last_probe_time_)
                                         .count());
                        mesh_probe_current_ = 0;
                        mesh_seconds_per_probe_ = 0.0f;
                    }

                    if (mesh_probe_current_ == 0) {
                        mesh_first_probe_time_ = now;
                    } else if (pp->current > 1) {
                        float elapsed = static_cast<float>(
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                now - mesh_first_probe_time_)
                                                .count()) /
                                        1000.0f;
                        mesh_seconds_per_probe_ = elapsed / static_cast<float>(pp->current - 1);
                    }
                    mesh_last_probe_time_ = now;

                    mesh_probe_current_ = pp->current;
                    mesh_probe_total_ = pp->total;
                    progress = calculate_progress_locked();
                }
                spdlog::debug("[PrintStartCollector] Mesh probe {}/{} ({:.1f}s/probe)", pp->current,
                              pp->total, mesh_seconds_per_probe_);
                snprintf(msg_buf, sizeof(msg_buf), "%s (%d/%d)", lv_tr("Bed Mesh"), pp->current,
                         pp->total);
                state_.set_print_start_state(PrintStartPhase::BED_MESH, msg_buf, progress);
                return; // Handled — skip check_phase_patterns
            }

            if (helix::is_probe_result_line(line)) {
                // "probe at X,Y is z=Z" fallback — no total from firmware.
                // Dedupe by (x,y): Klipper's `samples: N` config emits N consecutive
                // probes at the same position; we count unique points, not samples.
                auto pos = helix::parse_probe_position(line);
                int count;
                int total;
                int progress;
                bool is_new_point = false;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    auto now = std::chrono::steady_clock::now();

                    // Gap reset: if >30s since last probe, earlier probes were
                    // from a different operation (e.g. nozzle wipe on AD5M)
                    if (mesh_probe_fallback_count_ > 0 &&
                        (now - mesh_last_probe_time_) > MESH_PROBE_GAP_RESET) {
                        spdlog::info("[PrintStartCollector] Mesh probe gap reset "
                                     "({}s since last probe)",
                                     std::chrono::duration_cast<std::chrono::seconds>(
                                         now - mesh_last_probe_time_)
                                         .count());
                        mesh_probe_fallback_count_ = 0;
                        mesh_probe_current_ = 0;
                        mesh_seconds_per_probe_ = 0.0f;
                        mesh_has_last_probe_pos_ = false;
                    }

                    // Position-based dedupe. When we can't parse a position, fall back
                    // to counting each line (preserves prior behavior for malformed input).
                    constexpr double POS_TOL = 0.05; // mm — samples at same point are exact
                    if (pos) {
                        if (!mesh_has_last_probe_pos_ ||
                            std::abs(pos->x - mesh_last_probe_x_) > POS_TOL ||
                            std::abs(pos->y - mesh_last_probe_y_) > POS_TOL) {
                            is_new_point = true;
                            mesh_last_probe_x_ = pos->x;
                            mesh_last_probe_y_ = pos->y;
                            mesh_has_last_probe_pos_ = true;
                        }
                    } else {
                        is_new_point = true;
                    }

                    if (is_new_point) {
                        mesh_probe_fallback_count_++;
                        if (mesh_probe_fallback_count_ == 1) {
                            mesh_first_probe_time_ = now;
                        } else {
                            float elapsed =
                                static_cast<float>(
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        now - mesh_first_probe_time_)
                                        .count()) /
                                1000.0f;
                            mesh_seconds_per_probe_ =
                                elapsed / static_cast<float>(mesh_probe_fallback_count_ - 1);
                        }
                    }
                    mesh_last_probe_time_ = now;

                    mesh_probe_current_ = mesh_probe_fallback_count_;
                    count = mesh_probe_fallback_count_;
                    total = mesh_probe_total_;
                    progress = calculate_progress_locked();
                }
                if (is_new_point) {
                    spdlog::debug("[PrintStartCollector] Mesh point #{}/{} ({:.1f}s/point)", count,
                                  total, mesh_seconds_per_probe_);
                    char msg_buf[64];
                    if (total > 0) {
                        snprintf(msg_buf, sizeof(msg_buf), "%s (%d/%d)", lv_tr("Bed Mesh"), count,
                                 total);
                    } else {
                        snprintf(msg_buf, sizeof(msg_buf), "%s (%d)", lv_tr("Bed Mesh"), count);
                    }
                    state_.set_print_start_state(PrintStartPhase::BED_MESH, msg_buf, progress);
                }
            }
        }
    }

    // Check phase patterns
    check_phase_patterns(line);
}

void PrintStartCollector::check_phase_patterns(const std::string& line) {
    if (!profile_) {
        return;
    }

    PrintStartProfile::MatchResult match;
    if (profile_->try_match_pattern(line, match)) {
        // Only update if this is a new phase
        bool is_new_phase = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (detected_phases_.find(match.phase) == detected_phases_.end()) {
                detected_phases_.insert(match.phase);
                is_new_phase = true;
            }
        }
        if (is_new_phase) {
            if (profile_->progress_mode() == PrintStartProfile::ProgressMode::SEQUENTIAL) {
                update_phase(match.phase, match.message, match.progress);
            } else {
                update_phase(match.phase, match.message.c_str());
            }
            spdlog::debug("[PrintStartCollector] Detected phase: {}",
                          static_cast<int>(match.phase));
        }
    }
}

bool PrintStartCollector::check_helix_phase_signal(const std::string& line) {
    // Check for HELIX:PHASE:* signals (definitive markers from plugin/macros)
    static const char* HELIX_PHASE_PREFIX = "HELIX:PHASE:";
    constexpr size_t PREFIX_LEN = 12; // strlen("HELIX:PHASE:")

    // Find the prefix in the line (may have quotes or other wrappers)
    size_t pos = line.find(HELIX_PHASE_PREFIX);
    if (pos == std::string::npos) {
        return false;
    }

    // Extract the phase name
    std::string phase_name = line.substr(pos + PREFIX_LEN);
    // Trim trailing whitespace, quotes, etc.
    size_t end = phase_name.find_first_of(" \t\n\r\"'");
    if (end != std::string::npos) {
        phase_name = phase_name.substr(0, end);
    }

    spdlog::info("[PrintStartCollector] HELIX:PHASE signal: {}", phase_name);

    // Map phase name to PrintStartPhase
    if (phase_name == "STARTING" || phase_name == "START") {
        // Mark print start detected and transition to INITIALIZING
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            print_start_detected_ = true;
        }
        update_phase(PrintStartPhase::INITIALIZING, lv_tr("Preparing Print..."));
        return true;
    }

    if (phase_name == "COMPLETE" || phase_name == "DONE") {
        update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
        spdlog::info("[PrintStartCollector] Print start complete via HELIX:PHASE signal");
        return true;
    }

    // Individual phases
    if (phase_name == "HOMING") {
        update_phase(PrintStartPhase::HOMING, lv_tr("Homing..."));
        return true;
    }
    if (phase_name == "HEATING_BED" || phase_name == "BED_HEATING") {
        update_phase(PrintStartPhase::HEATING_BED, lv_tr("Heating Bed..."));
        return true;
    }
    if (phase_name == "HEATING_NOZZLE" || phase_name == "NOZZLE_HEATING" ||
        phase_name == "HEATING_HOTEND") {
        update_phase(PrintStartPhase::HEATING_NOZZLE, lv_tr("Heating Nozzle..."));
        return true;
    }
    if (phase_name == "QGL" || phase_name == "QUAD_GANTRY_LEVEL") {
        update_phase(PrintStartPhase::QGL, lv_tr("Leveling Gantry..."));
        return true;
    }
    if (phase_name == "Z_TILT" || phase_name == "Z_TILT_ADJUST") {
        update_phase(PrintStartPhase::Z_TILT, lv_tr("Z Tilt Adjust..."));
        return true;
    }
    if (phase_name == "BED_MESH" || phase_name == "BED_LEVELING") {
        update_phase(PrintStartPhase::BED_MESH, lv_tr("Loading Bed Mesh..."));
        return true;
    }
    if (phase_name == "CLEANING" || phase_name == "NOZZLE_CLEAN") {
        update_phase(PrintStartPhase::CLEANING, lv_tr("Cleaning Nozzle..."));
        return true;
    }
    if (phase_name == "PURGING" || phase_name == "PURGE" || phase_name == "PRIMING") {
        update_phase(PrintStartPhase::PURGING, lv_tr("Purging..."));
        return true;
    }

    // Unknown phase - log but don't block
    spdlog::warn("[PrintStartCollector] Unknown HELIX:PHASE: {}", phase_name);
    return false;
}

bool PrintStartCollector::check_k2_cfs_signal(const std::string& line) {
    // ---- K2 purge percent --------------------------------------------------
    // Real K2 firmware emits: "// num: 0, velocity: 575.000000, percent 1.000000"
    //   - "percent" with no colon, value is a 0..1 fraction (not 0..100)
    // Earlier/integer form may appear as "percent: 50" with 0..100. Tolerate both.
    if (auto p = line.find("percent"); p != std::string::npos &&
                                       (line.find("num:") != std::string::npos ||
                                        line.find("velocity:") != std::string::npos)) {
        const char* cursor = line.c_str() + p + 7; // past "percent"
        while (*cursor == ':' || *cursor == ' ') cursor++;

        float frac = -1.0f;
        if (std::sscanf(cursor, "%f", &frac) == 1 && frac >= 0.0f) {
            // Accept both 0..1 (K2 fraction) and 0..100 (legacy/integer).
            int percent = (frac <= 1.5f) ? static_cast<int>(frac * 100.0f + 0.5f)
                                         : static_cast<int>(frac + 0.5f);
            percent = std::max(0, std::min(100, percent));

            char msg[32];
            std::snprintf(msg, sizeof(msg), "%s — %d%%", lv_tr("Purging"), percent);
            update_phase(PrintStartPhase::PURGING, std::string(msg), percent);
            return true;
        }
    }

    // ---- CFS filament-load events ------------------------------------------
    // The box (CFS unit) emits these distinct lines during a load sequence.
    // Stock Klipper / AFC / ERCF use different vocabulary (LOAD_FILAMENT,
    // RESPOND MSG="Loading...", etc.) — those are handled elsewhere.
    if (line.find("[box] cut sensor detected") != std::string::npos ||
        line.find("[box] cut to return") != std::string::npos ||
        line.find("BOX_LOAD_MATERIAL") != std::string::npos) {
        // No dedicated PrintStartPhase value — INITIALIZING is the closest
        // non-terminal neighbor; the message carries the human-facing label.
        update_phase(PrintStartPhase::INITIALIZING, lv_tr("Loading Filament..."));
        return true;
    }

    return false;
}

void PrintStartCollector::update_phase(PrintStartPhase phase, const char* message) {
    int progress;
    bool should_save = false;
    bool has_predictions = false;
    bool entering_mesh = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (phase == PrintStartPhase::COMPLETE && current_phase_ == PrintStartPhase::COMPLETE)
            return;
        entering_mesh =
            (phase == PrintStartPhase::BED_MESH && current_phase_ != PrintStartPhase::BED_MESH);
        // Entering a leveling phase clears the pre-mesh probe buffer — any
        // stray probe lines that arrived before the QGL/Z_TILT regex hit
        // (rare, but possible if firmware emits a probe before the macro
        // echo) shouldn't carry into the eventual BED_MESH count.
        if ((phase == PrintStartPhase::QGL || phase == PrintStartPhase::Z_TILT) &&
            current_phase_ != phase) {
            pre_mesh_probe_count_ = 0;
        }
        current_phase_ = phase;
        detected_phases_.insert(phase); // Track for progress calculation

        // Record phase enter timestamp (skip IDLE and INITIALIZING)
        int phase_int = static_cast<int>(phase);
        if (phase != PrintStartPhase::IDLE && phase != PrintStartPhase::INITIALIZING &&
            phase != PrintStartPhase::COMPLETE) {
            if (phase_enter_times_.find(phase_int) == phase_enter_times_.end()) {
                phase_enter_times_[phase_int] = std::chrono::steady_clock::now();
            }
        }

        if (phase == PrintStartPhase::COMPLETE) {
            should_save = true;
        }

        progress = calculate_progress_locked();
        has_predictions = predictor_.has_predictions();
    }

    // When predictor has data, time-based progress in update_eta_display() is the
    // sole progress source. Don't override it with phase-weight progress here.
    if (has_predictions && phase != PrintStartPhase::COMPLETE) {
        auto* subj = state_.get_print_start_progress_subject();
        if (subj) {
            progress = lv_subject_get_int(subj);
        }
    }

    // Query probe count when entering BED_MESH for the first time
    if (entering_mesh) {
        query_mesh_probe_count();
    }

    // Call PrinterState outside the lock to avoid potential deadlocks
    state_.set_print_start_state(phase, message, progress);

    if (should_save) {
        save_prediction_entry();
    }
}

void PrintStartCollector::update_phase(PrintStartPhase phase, const std::string& message,
                                       int progress) {
    int effective_progress;
    bool should_save = false;
    bool entering_mesh = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (phase == PrintStartPhase::COMPLETE && current_phase_ == PrintStartPhase::COMPLETE)
            return;
        entering_mesh =
            (phase == PrintStartPhase::BED_MESH && current_phase_ != PrintStartPhase::BED_MESH);
        if ((phase == PrintStartPhase::QGL || phase == PrintStartPhase::Z_TILT) &&
            current_phase_ != phase) {
            pre_mesh_probe_count_ = 0;
        }
        current_phase_ = phase;
        detected_phases_.insert(phase);

        // Record phase enter timestamp (skip IDLE and INITIALIZING)
        int phase_int = static_cast<int>(phase);
        if (phase != PrintStartPhase::IDLE && phase != PrintStartPhase::INITIALIZING &&
            phase != PrintStartPhase::COMPLETE) {
            if (phase_enter_times_.find(phase_int) == phase_enter_times_.end()) {
                phase_enter_times_[phase_int] = std::chrono::steady_clock::now();
            }
        }

        // Monotonic progress guard: never allow progress to decrease in sequential mode
        // (except COMPLETE which always goes to 100%)
        if (phase == PrintStartPhase::COMPLETE) {
            effective_progress = 100;
            should_save = true;
        } else {
            effective_progress = std::max(progress, max_sequential_progress_);
            effective_progress = std::min(effective_progress, 95);
        }
        max_sequential_progress_ = effective_progress;
    }

    if (entering_mesh) {
        query_mesh_probe_count();
    }

    state_.set_print_start_state(phase, message.c_str(), effective_progress);

    if (should_save) {
        save_prediction_entry();
    }
}

void PrintStartCollector::update_message_only(const std::string& message) {
    PrintStartPhase phase;
    int progress;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        phase = current_phase_;
        if (phase == PrintStartPhase::IDLE || phase == PrintStartPhase::COMPLETE) {
            return; // Don't update message when not preparing
        }
        progress = calculate_progress_locked();
    }
    state_.set_print_start_state(phase, message.c_str(), progress);
}

void PrintStartCollector::set_profile(std::shared_ptr<PrintStartProfile> profile) {
    if (active_.load()) {
        spdlog::warn("[PrintStartCollector] set_profile() called while active, ignoring");
        return;
    }
    profile_ = std::move(profile);
    if (profile_) {
        spdlog::debug("[PrintStartCollector] Using profile: {}", profile_->name());
    } else {
        spdlog::info("[PrintStartCollector] No profile set, signal/pattern matching disabled");
    }
}

int PrintStartCollector::calculate_progress() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return calculate_progress_locked();
}

int PrintStartCollector::calculate_progress_locked() const {
    if (predicted_phase_weights_.empty()) {
        // Fallback to profile weights if no predictions
        if (!profile_)
            return 0;
        int total_weight = 0;
        for (const auto& phase : detected_phases_) {
            total_weight += profile_->get_phase_weight(phase);
        }
        return std::min(total_weight, 95);
    }

    float progress = 0.0f;

    // Completed phases (not the current one)
    for (const auto& phase : detected_phases_) {
        if (phase == current_phase_)
            continue;
        auto it = predicted_phase_weights_.find(static_cast<int>(phase));
        if (it != predicted_phase_weights_.end()) {
            progress += it->second;
        }
    }

    // Partial progress for current phase
    auto cur_it = predicted_phase_weights_.find(static_cast<int>(current_phase_));
    if (cur_it != predicted_phase_weights_.end()) {
        float phase_frac = 0.0f;

        if (current_phase_ == PrintStartPhase::HEATING_BED ||
            current_phase_ == PrintStartPhase::HEATING_NOZZLE) {
            phase_frac = compute_heating_fraction();
        } else if (current_phase_ == PrintStartPhase::BED_MESH && mesh_probe_total_ > 0) {
            // Use exact probe count for deterministic progress
            phase_frac = std::min(static_cast<float>(mesh_probe_current_) /
                                      static_cast<float>(mesh_probe_total_),
                                  0.95f);
        } else {
            // Time-based for non-heating phases
            auto enter_it = phase_enter_times_.find(static_cast<int>(current_phase_));
            if (enter_it != phase_enter_times_.end()) {
                float elapsed =
                    static_cast<float>(std::chrono::duration_cast<std::chrono::seconds>(
                                           std::chrono::steady_clock::now() - enter_it->second)
                                           .count());
                auto pred = predictor_.predicted_phases();
                auto pred_it = pred.find(static_cast<int>(current_phase_));
                if (pred_it != pred.end() && pred_it->second > 0) {
                    phase_frac = std::min(elapsed / static_cast<float>(pred_it->second), 0.95f);
                }
            }
        }
        progress += cur_it->second * phase_frac;
    }

    return std::min(static_cast<int>(progress * 95.0f), 95);
}

bool PrintStartCollector::is_print_start_marker(const std::string& line) const {
    return std::regex_search(line, print_start_pattern_);
}

bool PrintStartCollector::is_completion_marker(const std::string& line) const {
    return std::regex_search(line, completion_pattern_);
}

// ============================================================================
// PUBLIC ACCESSORS FOR PREDICTION
// ============================================================================

std::set<int> PrintStartCollector::get_completed_phase_ints() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return get_completed_phase_ints_locked();
}

std::set<int> PrintStartCollector::get_completed_phase_ints_locked() const {
    std::set<int> result;
    for (const auto& phase : detected_phases_) {
        int p = static_cast<int>(phase);
        // Exclude current phase from "completed" set
        if (phase != current_phase_ && phase != PrintStartPhase::IDLE &&
            phase != PrintStartPhase::INITIALIZING) {
            result.insert(p);
        }
    }
    return result;
}

int PrintStartCollector::get_current_phase_int() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return static_cast<int>(current_phase_);
}

int PrintStartCollector::get_current_phase_elapsed_seconds() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    int phase_int = static_cast<int>(current_phase_);
    auto it = phase_enter_times_.find(phase_int);
    if (it == phase_enter_times_.end()) {
        return 0;
    }
    auto elapsed = std::chrono::steady_clock::now() - it->second;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
}

// ============================================================================
// ETA DISPLAY UPDATE
// ============================================================================

void PrintStartCollector::update_eta_display() {
    if (!active_.load()) {
        return;
    }

    // Always update elapsed time since preparation started
    int total_elapsed;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto now = std::chrono::steady_clock::now();
        total_elapsed = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(now - printing_state_start_).count());
    }
    state_.set_preprint_elapsed_seconds(total_elapsed);
    feed_thermal_sample();

    // Compute remaining from composite weights (thermal model + predictor).
    // This handles heating phases correctly (predictor alone doesn't track them).
    int remaining;
    int predicted_total;
    int current_snap = 0;
    int phase_elapsed_snap = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (predicted_phase_weights_.empty()) {
            return;
        }

        auto completed = get_completed_phase_ints_locked();
        int current = static_cast<int>(current_phase_);
        int phase_elapsed = 0;
        auto it = phase_enter_times_.find(current);
        if (it != phase_enter_times_.end()) {
            phase_elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                                                 std::chrono::steady_clock::now() - it->second)
                                                 .count());
        }

        // Compute remaining from composite per-phase durations (weights * total)
        float remaining_f = 0.0f;
        for (const auto& [phase, weight] : predicted_phase_weights_) {
            float phase_dur = weight * predicted_total_seconds_;
            if (completed.count(phase) && phase != current) {
                continue; // completed phase
            } else if (phase == current) {
                // Current phase: use heating fraction for heating, probe
                // extrapolation for bed mesh, elapsed for others
                if (current == static_cast<int>(PrintStartPhase::HEATING_BED) ||
                    current == static_cast<int>(PrintStartPhase::HEATING_NOZZLE)) {
                    float frac = compute_heating_fraction();
                    remaining_f += phase_dur * (1.0f - frac);
                } else if (current == static_cast<int>(PrintStartPhase::BED_MESH) &&
                           mesh_probe_total_ > 0 && mesh_seconds_per_probe_ > 0.0f) {
                    // Live per-probe time extrapolation overrides historical prediction
                    int probes_left = mesh_probe_total_ - mesh_probe_current_;
                    remaining_f += mesh_seconds_per_probe_ * static_cast<float>(probes_left);
                } else {
                    remaining_f += std::max(0.0f, phase_dur - static_cast<float>(phase_elapsed));
                }
            } else {
                remaining_f += phase_dur; // future phase
            }
        }
        remaining = static_cast<int>(remaining_f);

        predicted_total = static_cast<int>(predicted_total_seconds_);
        current_snap = current;
        phase_elapsed_snap = phase_elapsed;
    }

    // --- Diagnostic: monotonic bias data collection ---
    {
        auto pred_phases = predictor_.predicted_phases();
        int cur_phase_predicted = 0;
        auto pp_it = pred_phases.find(current_snap);
        if (pp_it != pred_phases.end())
            cur_phase_predicted = pp_it->second;

        spdlog::debug("[PrintStartCollector] ETA update: phase={} elapsed={}s, remaining={}s, "
                      "predicted_total={:.0f}s, phase_predicted={}s",
                      current_snap, phase_elapsed_snap, remaining, predicted_total_seconds_,
                      cur_phase_predicted);
    }

    // Always update the int subject for print time integration
    // Monotonic bias: once under 2 minutes, only decrease unless overrun > 20%
    if (remaining < 120 && last_remaining_ > 0 && last_remaining_ < 120) {
        if (remaining > last_remaining_) {
            float overrun_pct = static_cast<float>(remaining - last_remaining_) /
                                std::max(1.0f, predicted_total_seconds_) * 100.0f;
            if (overrun_pct < 20.0f) {
                auto pred_phases = predictor_.predicted_phases();
                int cur_phase_predicted = 0;
                auto pp_it = pred_phases.find(current_snap);
                if (pp_it != pred_phases.end())
                    cur_phase_predicted = pp_it->second;
                float per_phase_overrun_pct =
                    static_cast<float>(remaining - last_remaining_) /
                    std::max(1.0f, static_cast<float>(cur_phase_predicted)) * 100.0f;
                int remaining_before_bias = remaining;
                remaining = last_remaining_;
                spdlog::debug("[PrintStartCollector] Monotonic bias: suppressed {}s→{}s, "
                              "overrun={:.1f}% (total denom={:.0f}s), "
                              "per-phase would be {:.1f}% (phase denom={}s)",
                              last_remaining_, remaining_before_bias, overrun_pct,
                              predicted_total_seconds_, per_phase_overrun_pct, cur_phase_predicted);
            }
        }
    }
    // Strict monotonic guard: remaining time should never increase.
    // Weight recomputation (e.g. new heater target discovered) can cause
    // a large upward spike that looks jarring on screen. The underlying
    // estimate will naturally catch up as heating progresses.
    if (last_remaining_ > 0 && remaining > last_remaining_) {
        spdlog::debug("[PrintStartCollector] Clamping remaining {}s→{}s (monotonic)", remaining,
                      last_remaining_);
        remaining = last_remaining_;
    }
    last_remaining_ = remaining;

    state_.set_preprint_remaining_seconds(remaining);
    if (predicted_total > 0) {
        int effective_progress = calculate_progress();
        // Monotonic progress: never go backwards. When weights recompute
        // (e.g. new heater target discovered), the progress may recalculate
        // lower against a larger predicted total. Users should never see
        // the progress bar regress.
        auto* subj = state_.get_print_start_progress_subject();
        if (subj) {
            int current_progress = lv_subject_get_int(subj);
            effective_progress = std::max(effective_progress, current_progress);
            if (effective_progress != current_progress) {
                lv_subject_set_int(subj, effective_progress);
            }
        }
    }

    if (remaining <= 0) {
        state_.set_print_start_time_left("Almost ready");
        return;
    }

    // Round for stable display, then format as "~X min left" or "~X:XX left"
    int display_remaining = helix::format::round_eta_seconds(remaining);
    std::string text = "~" + helix::format::duration_remaining(display_remaining);
    state_.set_print_start_time_left(text.c_str());

    spdlog::trace("[PrintStartCollector] ETA: {}s remaining, predicted_total={}s", remaining,
                  predicted_total);
}

// ============================================================================
// PREDICTION HISTORY PERSISTENCE
// ============================================================================

void PrintStartCollector::load_prediction_history() {
    auto entries = helix::PreprintPredictor::load_entries_from_config();

    // Cold (1) vs Warm (2) based on current bed temp
    int bed_temp = lv_subject_get_int(state_.get_bed_temp_subject()) / 10;
    int temp_bucket = (bed_temp >= 40) ? 2 : 1;

    std::lock_guard<std::mutex> lock(state_mutex_);
    predictor_.load_entries(entries, temp_bucket);
    loaded_temp_bucket_ = temp_bucket;

    if (!entries.empty()) {
        spdlog::debug("[PrintStartCollector] Loaded {} prediction entries for {} bucket "
                      "(predicted total: {}s)",
                      predictor_.get_entries().size(), temp_bucket == 2 ? "warm" : "cold",
                      predictor_.predicted_total());
    }
}

void PrintStartCollector::feed_thermal_sample() {
    auto& mgr = ThermalRateManager::instance();
    auto now_ms =
        static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - printing_state_start_)
                                  .count());

    PrintStartPhase phase;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        phase = current_phase_;
    }

    if (phase == PrintStartPhase::HEATING_BED || phase == PrintStartPhase::HEATING_NOZZLE) {
        // Temps are in decidegrees (value * 10)
        int ext_temp = lv_subject_get_int(state_.get_active_extruder_temp_subject());
        int bed_temp = lv_subject_get_int(state_.get_bed_temp_subject());
        mgr.get_model("extruder").record_sample(ext_temp / 10.0f, now_ms);
        mgr.get_model("heater_bed").record_sample(bed_temp / 10.0f, now_ms);
    }
}

void PrintStartCollector::compute_predicted_weights() {
    auto& mgr = ThermalRateManager::instance();

    int ext_target = lv_subject_get_int(state_.get_active_extruder_target_subject()) / 10;
    int bed_target = lv_subject_get_int(state_.get_bed_target_subject()) / 10;

    std::map<int, float> durations;

    // Heating phases from thermal model
    float ext_heat = mgr.estimate_heating_seconds("extruder", static_cast<float>(start_ext_temp_),
                                                  static_cast<float>(ext_target));
    float bed_heat = mgr.estimate_heating_seconds("heater_bed", static_cast<float>(start_bed_temp_),
                                                  static_cast<float>(bed_target));
    if (ext_heat > 0)
        durations[static_cast<int>(PrintStartPhase::HEATING_NOZZLE)] = ext_heat;
    if (bed_heat > 0)
        durations[static_cast<int>(PrintStartPhase::HEATING_BED)] = bed_heat;

    // Non-heating phases from predictor + write predicted weights under lock
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto pred_phases = predictor_.predicted_phases();

        for (const auto& [phase, secs] : pred_phases) {
            if (phase != static_cast<int>(PrintStartPhase::HEATING_BED) &&
                phase != static_cast<int>(PrintStartPhase::HEATING_NOZZLE)) {
                durations[phase] = static_cast<float>(secs);
            }
        }

        // Ensure homing has at least a default. History often records
        // HOMING=0 on printers whose PRINT_START macros don't emit a
        // distinct "Homing..." message before the next phase starts, so
        // check both "missing" and "zero" cases.
        int homing = static_cast<int>(PrintStartPhase::HOMING);
        auto homing_it = durations.find(homing);
        if (homing_it == durations.end() || homing_it->second < 20.0f) {
            durations[homing] = 20.0f;
        }

        // When nozzle target is unknown (common with bed-first macros that
        // issue M109 after homing/mesh), include a placeholder so progress
        // doesn't over-inflate by assigning all weight to known phases.
        int nozzle_phase = static_cast<int>(PrintStartPhase::HEATING_NOZZLE);
        if (ext_target == 0 && durations.find(nozzle_phase) == durations.end()) {
            durations[nozzle_phase] = 60.0f; // Conservative placeholder
        }

        float durations_sum = 0.0f;
        for (const auto& [_, dur] : durations) {
            durations_sum += dur;
        }

        // Phase weights are fractions of durations_sum so they always sum
        // to 1.0, regardless of the total used for display.
        predicted_phase_weights_.clear();
        if (durations_sum > 0.0f) {
            for (const auto& [phase, dur] : durations) {
                predicted_phase_weights_[phase] = dur / durations_sum;
            }
        }

        // Use the max of our built-up sum and the predictor's wall-clock
        // weighted average. On printers where the phase-matching regexes
        // miss PURGING/CLEANING/etc., durations_sum systematically under-
        // estimates total pre-print time while history's total_seconds
        // captured the real wall-clock duration. Taking the max folds that
        // unmapped time back into the display estimate without double-
        // counting the current thermal estimate.
        int wall_clock_estimate = predictor_.predicted_total();
        predicted_total_seconds_ = std::max(durations_sum, static_cast<float>(wall_clock_estimate));
    }

    // Track targets used so we can detect changes and recompute
    weights_ext_target_ = ext_target;
    weights_bed_target_ = bed_target;

    spdlog::debug("[PrintStartCollector] Predicted weights: total={:.0f}s, {} phases "
                  "(ext_target={}°C, bed_target={}°C)",
                  predicted_total_seconds_, predicted_phase_weights_.size(), ext_target,
                  bed_target);
}

float PrintStartCollector::compute_heating_fraction() const {
    // Use cached temps (thread-safe atomics) instead of LVGL subjects
    int ext_temp = cached_ext_temp_.load(std::memory_order_relaxed);
    int ext_target = cached_ext_target_.load(std::memory_order_relaxed);
    int bed_temp = cached_bed_temp_.load(std::memory_order_relaxed);
    int bed_target = cached_bed_target_.load(std::memory_order_relaxed);

    if (current_phase_ == PrintStartPhase::HEATING_NOZZLE && ext_target > 0) {
        float start = start_ext_temp_ > 0 ? static_cast<float>(start_ext_temp_) : 25.0f;
        float range = static_cast<float>(ext_target) - start;
        if (range > 0.0f) {
            return std::clamp((static_cast<float>(ext_temp) - start) / range, 0.0f, 1.0f);
        }
    }
    if (current_phase_ == PrintStartPhase::HEATING_BED && bed_target > 0) {
        float start = start_bed_temp_ > 0 ? static_cast<float>(start_bed_temp_) : 25.0f;
        float range = static_cast<float>(bed_target) - start;
        if (range > 0.0f) {
            return std::clamp((static_cast<float>(bed_temp) - start) / range, 0.0f, 1.0f);
        }
    }
    return 0.0f;
}

void PrintStartCollector::query_mesh_probe_count() {
    if (!active_.load())
        return;

    auto self = shared_from_this();
    // Query both bed_mesh (for probed_matrix from last run — authoritative point count,
    // handles adaptive meshing where probe_count config overstates actual probes)
    // and configfile settings (fallback when no prior mesh exists).
    json params = {
        {"objects",
         json::object({{"bed_mesh", nullptr}, {"configfile", json::array({"settings"})}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [self](json response) {
            if (!self->active_.load())
                return;

            int total = 0;
            const char* source = "";

            // Prefer probed_matrix dimensions from last completed mesh. For printers
            // with adaptive/reduced probing (Snapmaker U1, KAMP), this reflects what
            // will actually be probed; probe_count config overstates by 5x on U1.
            try {
                const auto& status = response["result"]["status"];
                if (status.contains("bed_mesh") && status["bed_mesh"].contains("probed_matrix")) {
                    const auto& pm = status["bed_mesh"]["probed_matrix"];
                    if (pm.is_array() && !pm.empty() && pm[0].is_array()) {
                        int rows = static_cast<int>(pm.size());
                        int cols = static_cast<int>(pm[0].size());
                        if (rows > 0 && cols > 0) {
                            total = rows * cols;
                            source = "probed_matrix";
                        }
                    }
                }
            } catch (...) {
                // fall through to config
            }

            if (total == 0) {
                try {
                    const auto& settings = response["result"]["status"]["configfile"]["settings"];
                    if (settings.contains("bed_mesh") &&
                        settings["bed_mesh"].contains("probe_count")) {
                        const auto& pc = settings["bed_mesh"]["probe_count"];
                        if (pc.is_array() && pc.size() >= 2) {
                            total = pc[0].template get<int>() * pc[1].template get<int>();
                        } else if (pc.is_number_integer()) {
                            int n = pc.template get<int>();
                            total = n * n; // square grid
                        }
                        source = "probe_count";
                    }
                } catch (...) {
                    // Non-fatal — fallback mode continues without total
                }
            }

            if (total > 0) {
                std::lock_guard<std::mutex> lock(self->state_mutex_);
                self->mesh_probe_total_ = total;
                spdlog::info("[PrintStartCollector] Mesh point total from {}: {}", source, total);
            }
        },
        [](const MoonrakerError& /*err*/) {
            spdlog::debug("[PrintStartCollector] Failed to query bed_mesh point count");
        });
}

void PrintStartCollector::save_prediction_entry() {
    // Don't save timing data from fallback timeout completions — phases may be
    // interrupted or incomplete, producing misleading predictions
    if (fallback_completion_) {
        spdlog::debug(
            "[PrintStartCollector] Skipping prediction save (fallback timeout completion)");
        return;
    }

    // Compute per-phase durations from timestamps
    std::map<int, int> phase_durations;
    auto now = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point start_time;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        start_time = printing_state_start_;

        // Build sorted list of phase enter times
        std::vector<std::pair<int, std::chrono::steady_clock::time_point>> sorted_phases(
            phase_enter_times_.begin(), phase_enter_times_.end());
        std::sort(sorted_phases.begin(), sorted_phases.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });

        for (size_t i = 0; i < sorted_phases.size(); ++i) {
            int phase_int = sorted_phases[i].first;
            // Heating phases tracked by ThermalRateModel, not predictor
            if (phase_int == static_cast<int>(PrintStartPhase::HEATING_BED) ||
                phase_int == static_cast<int>(PrintStartPhase::HEATING_NOZZLE)) {
                continue;
            }
            auto end_time = (i + 1 < sorted_phases.size()) ? sorted_phases[i + 1].second : now;
            int duration = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(end_time - sorted_phases[i].second)
                    .count());
            // Skip zero-second phases — these happen when two phases enter
            // within the same second (e.g. G28 followed immediately by
            // proactive HEATING_BED detection on AD5M). Recording HOMING=0
            // repeatedly would poison the MAD filter and suppress the
            // default-floor fallback in compute_predicted_weights().
            if (duration <= 0) {
                continue;
            }
            phase_durations[phase_int] = duration;
        }

        // --- Diagnostic: predicted vs actual per-phase ---
        auto predicted = predictor_.predicted_phases();
        spdlog::debug("[PrintStartCollector] Print complete — predicted vs actual:");
        for (const auto& [phase, actual] : phase_durations) {
            auto p_it = predicted.find(phase);
            int pred = (p_it != predicted.end()) ? p_it->second : 0;
            float error_pct =
                pred > 0 ? ((static_cast<float>(actual) - pred) / pred * 100.0f) : 0.0f;
            spdlog::debug(
                "[PrintStartCollector]   phase {}: predicted={}s actual={}s error={:.1f}%", phase,
                pred, actual, error_pct);
        }
    }

    if (phase_durations.empty()) {
        spdlog::debug("[PrintStartCollector] No phase timings to save");
        return;
    }

    // Use wall-clock elapsed time as total — includes heating phases omitted from phase_durations
    int wall_clock_total = static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count());

    // Cold (1) vs Warm (2) based on bed temp at start
    helix::PreprintEntry entry;
    entry.total_seconds = wall_clock_total;
    entry.timestamp = static_cast<int64_t>(std::time(nullptr));
    entry.phase_durations = std::move(phase_durations);
    entry.temp_bucket = (start_bed_temp_ >= 40) ? 2 : 1;

    std::vector<helix::PreprintEntry> bucket_entries;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        predictor_.add_entry(entry);
        bucket_entries = predictor_.get_entries();
    }

    // Persist to config: merge bucket entries with entries from other buckets
    int temp_bucket = entry.temp_bucket;
    helix::ui::queue_update([bucket_entries, temp_bucket]() {
        // Load all existing entries, keep those from OTHER buckets
        auto all_entries = helix::PreprintPredictor::load_entries_from_config();
        std::vector<helix::PreprintEntry> merged;
        for (const auto& e : all_entries) {
            if (e.temp_bucket != temp_bucket) {
                merged.push_back(e);
            }
        }
        // Add current bucket's entries
        for (const auto& e : bucket_entries) {
            merged.push_back(e);
        }
        // Cap total stored entries to prevent unbounded growth
        constexpr size_t MAX_TOTAL_ENTRIES = 15;
        while (merged.size() > MAX_TOTAL_ENTRIES) {
            merged.erase(merged.begin());
        }
        auto entries = std::move(merged);
        auto* cfg = Config::get_instance();
        if (!cfg) {
            return;
        }

        if (entries.size() > 1000) {
            spdlog::warn("[PrintStartCollector] Suspiciously large entry count ({}), skipping save",
                         entries.size());
            return;
        }

        try {
            json entries_json = json::array();
            for (const auto& e : entries) {
                json entry_json;
                entry_json["total"] = e.total_seconds;
                entry_json["timestamp"] = e.timestamp;

                json phases_json = json::object();
                for (const auto& [phase, duration] : e.phase_durations) {
                    phases_json[std::to_string(phase)] = duration;
                }
                entry_json["phases"] = phases_json;
                if (e.temp_bucket > 0) {
                    entry_json["temp_bucket"] = e.temp_bucket;
                }
                entries_json.push_back(entry_json);
            }

            cfg->set<json>(PREPRINT_HISTORY_PATH, entries_json);
            ThermalRateManager::instance().save_to_config(*cfg);
            cfg->save();

            spdlog::debug("[PrintStartCollector] Saved prediction history ({} entries)",
                          entries.size());
        } catch (const std::exception& ex) {
            spdlog::error("[PrintStartCollector] Failed to save prediction history: {}", ex.what());
        }
    });
}
