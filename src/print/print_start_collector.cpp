// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_collector.h"

#include "ui_update_queue.h"

#include "config.h"
#include "format_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

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
        current_phase_ = PrintStartPhase::IDLE;
        print_start_detected_ = false;
        max_sequential_progress_ = 0;
        phase_enter_times_.clear();
    }
    fallbacks_enabled_.store(false); // Will be enabled after initial window

    // Load prediction history from config
    load_prediction_history();

    // Ensure we have a profile for pattern matching
    if (!profile_) {
        profile_ = PrintStartProfile::load_default();
    }

    // Generate unique handler name for G-code response callback
    static std::atomic<uint64_t> s_collector_id{0};
    handler_name_ = "print_start_collector_" + std::to_string(++s_collector_id);

    // Register for G-code responses (primary detection method)
    auto self = shared_from_this();
    client_.register_method_callback("notify_gcode_response", handler_name_,
                                     [self](const json& msg) { self->on_gcode_response(msg); });

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

    if (was_registered) {
        client_.unregister_method_callback("notify_gcode_response", handler_name_);
        spdlog::debug("[PrintStartCollector] Unregistered G-code callback");
    }

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
        current_phase_ = PrintStartPhase::IDLE;
        print_start_detected_ = false;
        max_sequential_progress_ = 0;
        printing_state_start_ = std::chrono::steady_clock::now();
        phase_enter_times_.clear();
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

void PrintStartCollector::check_fallback_completion() {
    if (!active_.load() || !fallbacks_enabled_.load()) {
        return;
    }

    // Check current phase under lock
    std::chrono::steady_clock::time_point start_time;
    PrintStartPhase current;
    bool print_start_was_detected;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // Already complete - nothing to do
        if (current_phase_ == PrintStartPhase::COMPLETE) {
            return;
        }
        current = current_phase_;
        print_start_was_detected = print_start_detected_;
        start_time = printing_state_start_;
    }

    // Get temperature data for proactive and completion fallback checks
    int ext_temp = lv_subject_get_int(state_.get_active_extruder_temp_subject());
    int ext_target = lv_subject_get_int(state_.get_active_extruder_target_subject());
    int bed_temp = lv_subject_get_int(state_.get_bed_temp_subject());
    int bed_target = lv_subject_get_int(state_.get_bed_target_subject());

    // Temps are in decidegrees (value * 10), targets may be 0 if not set
    bool bed_heating = bed_target > 0 && bed_temp < bed_target - TEMP_TOLERANCE_DECIDEGREES;
    bool nozzle_heating = ext_target > 0 && ext_temp < ext_target - TEMP_TOLERANCE_DECIDEGREES;
    bool temps_ready = !bed_heating && !nozzle_heating;

    // =========================================================================
    // PROACTIVE DETECTION: Detect PREPARING phase when heaters are ramping
    // This ensures "Preparing" shows even without HELIX:PHASE signals
    // =========================================================================
    if (current == PrintStartPhase::IDLE && !print_start_was_detected) {
        // We're in IDLE but collector is active - this means print just started
        // If heaters are ramping, we're definitely in PRINT_START preparation
        if (bed_heating || nozzle_heating) {
            // Determine which heating phase to show
            if (bed_heating && bed_temp < bed_target / 2) {
                // Bed is far from target - primary heating phase
                spdlog::info("[PrintStartCollector] Proactive: bed heating ({}/{})", bed_temp / 10,
                             bed_target / 10);
                update_phase(PrintStartPhase::HEATING_BED, lv_tr("Heating Bed..."));
            } else if (nozzle_heating) {
                // Nozzle heating (bed may be close or done)
                spdlog::info("[PrintStartCollector] Proactive: nozzle heating ({}/{})",
                             ext_temp / 10, ext_target / 10);
                update_phase(PrintStartPhase::HEATING_NOZZLE, lv_tr("Heating Nozzle..."));
            } else {
                // Generic initializing state
                spdlog::info("[PrintStartCollector] Proactive: initializing (heaters ramping)");
                update_phase(PrintStartPhase::INITIALIZING, lv_tr("Preparing Print..."));
            }
            return;
        }
    }

    // =========================================================================
    // COMPLETION DETECTION: Detect when PRINT_START is done
    // =========================================================================

    // Fallback 1: Layer count (slicer-dependent but reliable when present)
    int layer = lv_subject_get_int(state_.get_print_layer_current_subject());
    if (layer >= 1) {
        spdlog::info("[PrintStartCollector] Fallback: layer {} detected", layer);
        update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
        return;
    }

    // Fallback 2: Progress threshold with temps at target
    // 2% progress means file has advanced past typical preamble/macros
    int progress = lv_subject_get_int(state_.get_print_progress_subject());
    if (progress >= 2 && temps_ready) {
        spdlog::info("[PrintStartCollector] Fallback: progress {}% with temps ready", progress);
        update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
        return;
    }

    // Fallback 3: Timeout with temps near target (90% of target)
    bool temps_near = (ext_target <= 0 || ext_temp >= static_cast<int>(ext_target * 0.9)) &&
                      (bed_target <= 0 || bed_temp >= static_cast<int>(bed_target * 0.9));

    auto elapsed = std::chrono::steady_clock::now() - start_time;
    if (elapsed > FALLBACK_TIMEOUT && temps_near) {
        auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        spdlog::info("[PrintStartCollector] Fallback: timeout ({} sec)", elapsed_sec);
        update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
        return;
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

    // Check for completion (layer 1 indicator)
    if (is_completion_marker(line)) {
        update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
        spdlog::debug("[PrintStartCollector] Print start complete - layer 1 detected");
        // Note: The caller (main.cpp) should stop the collector when print state becomes PRINTING
        return;
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
            spdlog::debug("[PrintStartCollector] Detected phase: {} (progress: {}%)",
                          static_cast<int>(match.phase), calculate_progress());
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

void PrintStartCollector::update_phase(PrintStartPhase phase, const char* message) {
    int progress;
    bool should_save = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (phase == PrintStartPhase::COMPLETE && current_phase_ == PrintStartPhase::COMPLETE)
            return;
        current_phase_ = phase;

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
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (phase == PrintStartPhase::COMPLETE && current_phase_ == PrintStartPhase::COMPLETE)
            return;
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
    state_.set_print_start_state(phase, message.c_str(), effective_progress);

    if (should_save) {
        save_prediction_entry();
    }
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
    if (!profile_) {
        return 0;
    }

    int total_weight = 0;
    for (const auto& phase : detected_phases_) {
        total_weight += profile_->get_phase_weight(phase);
    }

    // Cap at 95% - final 5% is for completion transition
    return std::min(total_weight, 95);
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

    if (!predictor_.has_predictions()) {
        return;
    }

    auto completed = get_completed_phase_ints();
    int current = get_current_phase_int();
    int elapsed = get_current_phase_elapsed_seconds();

    int remaining = predictor_.remaining_seconds(completed, current, elapsed);

    // Always update the int subject for print time integration
    state_.set_preprint_remaining_seconds(remaining);

    if (remaining <= 0) {
        state_.set_print_start_time_left("Almost ready");
        return;
    }

    // Format as "~X min left" or "~X:XX left"
    std::string text = "~" + helix::format::duration_remaining(remaining);
    state_.set_print_start_time_left(text.c_str());

    spdlog::trace("[PrintStartCollector] ETA: {}s remaining (phase={}, elapsed={}s)", remaining,
                  current, elapsed);
}

// ============================================================================
// PREDICTION HISTORY PERSISTENCE
// ============================================================================

void PrintStartCollector::load_prediction_history() {
    auto entries = helix::PreprintPredictor::load_entries_from_config();

    std::lock_guard<std::mutex> lock(state_mutex_);
    predictor_.load_entries(entries);

    if (!entries.empty()) {
        spdlog::debug("[PrintStartCollector] Loaded {} prediction entries (predicted total: {}s)",
                      entries.size(), predictor_.predicted_total());
    }
}

void PrintStartCollector::save_prediction_entry() {
    // Compute per-phase durations from timestamps
    std::map<int, int> phase_durations;
    auto now = std::chrono::steady_clock::now();
    int total_seconds = 0;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);

        // Build sorted list of phase enter times
        std::vector<std::pair<int, std::chrono::steady_clock::time_point>> sorted_phases(
            phase_enter_times_.begin(), phase_enter_times_.end());
        std::sort(sorted_phases.begin(), sorted_phases.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });

        for (size_t i = 0; i < sorted_phases.size(); ++i) {
            auto end_time = (i + 1 < sorted_phases.size()) ? sorted_phases[i + 1].second : now;
            int duration = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(end_time - sorted_phases[i].second)
                    .count());
            phase_durations[sorted_phases[i].first] = duration;
            total_seconds += duration;
        }
    }

    if (phase_durations.empty()) {
        spdlog::debug("[PrintStartCollector] No phase timings to save");
        return;
    }

    helix::PreprintEntry entry;
    entry.total_seconds = total_seconds;
    entry.timestamp = static_cast<int64_t>(std::time(nullptr));
    entry.phase_durations = std::move(phase_durations);

    std::vector<helix::PreprintEntry> entries;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        predictor_.add_entry(entry);
        entries = predictor_.get_entries();
    }

    // Persist to config (must be on main thread)
    helix::ui::queue_update([entries]() {
        auto* cfg = Config::get_instance();
        if (!cfg) {
            return;
        }

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
            entries_json.push_back(entry_json);
        }

        cfg->set<json>(PREPRINT_HISTORY_PATH, entries_json);
        cfg->save();

        spdlog::debug("[PrintStartCollector] Saved prediction history ({} entries)",
                      entries.size());
    });
}
