// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "bed_mesh_probe_parser.h"
#include "moonraker_client.h"
#include "preprint_predictor.h"
#include "print_start_profile.h"
#include "printer_state.h"
#include "thermal_rate_model.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string>

/**
 * @file print_start_collector.h
 * @brief Monitors G-code responses to detect PRINT_START macro phases
 *
 * Subscribes to Moonraker's notify_gcode_response to parse G-code output
 * during print initialization. Detects common PRINT_START phases like
 * homing, heating, QGL, bed mesh, and purging through pattern matching.
 *
 * ## Usage
 * The collector is started when a print begins and stopped when the print
 * transitions to actual printing (or is cancelled). Progress is reported
 * through helix::PrinterState subjects which XML can bind to directly.
 *
 * ## Pattern Detection
 * Uses best-effort regex matching on G-code responses. Not all macros will
 * output all phases - the progress calculation handles missing phases gracefully.
 *
 * @see helix::PrintStartPhase enum in printer_state.h
 */
class PrintStartCollector : public std::enable_shared_from_this<PrintStartCollector> {
    friend class PrintStartCollectorTestAccess;

  public:
    /**
     * @brief Construct a PrintStartCollector
     * @param client helix::MoonrakerClient for registering callbacks
     * @param state helix::PrinterState to update with phase progress
     */
    PrintStartCollector(helix::MoonrakerClient& client, helix::PrinterState& state);

    ~PrintStartCollector();

    // Non-copyable
    PrintStartCollector(const PrintStartCollector&) = delete;
    PrintStartCollector& operator=(const PrintStartCollector&) = delete;

    /**
     * @brief Start monitoring for PRINT_START phases
     *
     * Registers for notify_gcode_response notifications and begins
     * parsing G-code output for phase detection patterns.
     */
    void start();

    /**
     * @brief Stop monitoring
     *
     * Unregisters callback and resets state. Called when print
     * initialization completes or print is cancelled.
     */
    void stop();

    /**
     * @brief Check if collector is currently active
     */
    [[nodiscard]] bool is_active() const {
        return active_.load();
    }

    /**
     * @brief Reset detected phases (for new print)
     */
    void reset();

    /**
     * @brief Check fallback completion conditions
     *
     * Called by observers when layer count or progress changes.
     * Checks multiple fallback signals for printers that don't emit
     * layer markers in G-code responses (e.g., FlashForge AD5M).
     */
    void check_fallback_completion();

    /**
     * @brief Enable fallback detection after initial G-code response window
     *
     * Called shortly after start() to enable fallback signals.
     * Gives G-code response detection priority for the first few seconds.
     */
    void enable_fallbacks();

    /**
     * @brief Complete the pre-print phase from an external authoritative signal
     *
     * Called when an external source (e.g., Moonraker state transition) definitively
     * indicates the print has started. Immediately transitions to COMPLETE.
     *
     * @param source Description of the signal source (for logging)
     */
    void complete_from_external_signal(const char* source);

    /**
     * @brief Set the print start profile for pattern/signal matching
     *
     * Must be called before start(). Ignored if the collector is active.
     *
     * @param profile Profile to use, or nullptr to disable profile-based matching
     */
    void set_profile(std::shared_ptr<PrintStartProfile> profile);

    /**
     * @brief Get the predictor for reading predictions
     *
     * Thread-safe: predictor is loaded on start() and entries added on COMPLETE,
     * both under state_mutex_. Callers (LVGL timer) should use remaining_seconds()
     * which is const and safe to call from main thread.
     */
    [[nodiscard]] const helix::PreprintPredictor& predictor() const {
        return predictor_;
    }

    /**
     * @brief Get detected phases as int set (for predictor remaining calculation)
     *
     * Must be called under state_mutex_ or from main thread when collector stopped.
     */
    [[nodiscard]] std::set<int> get_completed_phase_ints() const;

    /**
     * @brief Get current phase as int
     */
    [[nodiscard]] int get_current_phase_int() const;

    /**
     * @brief Get elapsed seconds in current phase
     */
    [[nodiscard]] int get_current_phase_elapsed_seconds() const;

  private:
    /**
     * @brief Handle incoming G-code response
     */
    void on_gcode_response(const nlohmann::json& msg);

    /**
     * @brief Check line against phase patterns
     */
    void check_phase_patterns(const std::string& line);

    /**
     * @brief Check for HELIX:PHASE:* signals from plugin/macros
     *
     * These are definitive signals that take priority over regex detection.
     * Format: "HELIX:PHASE:STARTING", "HELIX:PHASE:HOMING", "HELIX:PHASE:COMPLETE", etc.
     *
     * @return true if a HELIX:PHASE signal was detected and handled
     */
    bool check_helix_phase_signal(const std::string& line);

    /**
     * @brief Check for K2/CFS-specific gcode tag stream signals
     *
     * Creality K2 and CFS-equipped printers emit a richer gcode tag stream
     * than stock Klipper. These tags carry phase info that universal probe-
     * line / regex heuristics can't extract precisely. Currently:
     *
     * - `// num: N, velocity: V, percent F` (purge percent, fraction or int)
     * - `// [box] cut sensor detected` / `// [box] cut to return` /
     *   `BOX_LOAD_MATERIAL` (CFS filament-load events)
     *
     * Mapped onto the existing PrintStartPhase enum (PURGING, INITIALIZING)
     * so the legacy `preparing_overlay` UI binds without change.
     *
     * @return true if a K2/CFS signal was detected and handled
     */
    bool check_k2_cfs_signal(const std::string& line);

    /**
     * @brief Update phase and recalculate progress (weighted mode)
     */
    void update_phase(helix::PrintStartPhase phase, const char* message);

    /**
     * @brief Update phase with explicit progress value (sequential mode)
     */
    void update_phase(helix::PrintStartPhase phase, const std::string& message, int progress);

    /**
     * @brief Update only the status message without changing phase or progress
     *
     * Used to pass through display_status.message (M117) from Klipper during preparation.
     */
    void update_message_only(const std::string& message);

    /**
     * @brief Calculate overall progress based on detected phases
     */
    int calculate_progress() const;

    /**
     * @brief Calculate progress (must be called with state_mutex_ held)
     */
    int calculate_progress_locked() const;

    /**
     * @brief Get completed phases (must be called with state_mutex_ held)
     */
    [[nodiscard]] std::set<int> get_completed_phase_ints_locked() const;

    /**
     * @brief Check for PRINT_START start marker
     */
    bool is_print_start_marker(const std::string& line) const;

    /**
     * @brief Check for print start completion (layer 1, etc.)
     */
    bool is_completion_marker(const std::string& line) const;

    /** @brief Check if a G-code response is a RESPOND-based print start completion */
    [[nodiscard]] bool is_respond_completion(const std::string& line) const {
        return std::regex_search(line, respond_completion_pattern_);
    }

    // Dependencies
    helix::MoonrakerClient& client_;
    helix::PrinterState& state_;

    // Registration state
    std::string handler_name_;
    std::atomic<bool> active_{false};
    std::atomic<bool> registered_{false};

    // Thread safety: protects all non-atomic members below
    // WebSocket callbacks run on background thread, check_fallback_completion() runs on main thread
    mutable std::mutex state_mutex_;

    // Phase tracking (protected by state_mutex_)
    std::set<helix::PrintStartPhase> detected_phases_;
    helix::PrintStartPhase current_phase_ = helix::PrintStartPhase::IDLE;
    bool print_start_detected_ = false;
    int max_sequential_progress_ = 0; // Monotonic progress guard for sequential mode
    std::chrono::steady_clock::time_point printing_state_start_;

    // Profile for signal/pattern matching (set via set_profile() or loaded by start())
    std::shared_ptr<PrintStartProfile> profile_;

    // Universal patterns (not profile-specific)
    static const std::regex print_start_pattern_;
    static const std::regex completion_pattern_;
    static const std::regex respond_completion_pattern_;

    // Fallback detection constants
    static constexpr auto FALLBACK_TIMEOUT =
        std::chrono::seconds(300); ///< Last resort when no predictions
    static constexpr auto ABSOLUTE_MAX_TIMEOUT =
        std::chrono::seconds(900); ///< Hard ceiling (stuck detection)
    static constexpr float ADAPTIVE_TIMEOUT_MARGIN =
        1.5f; ///< Multiply predicted total for adaptive timeout
    static constexpr float ABSOLUTE_TIMEOUT_MARGIN =
        2.5f; ///< Multiply predicted total for absolute ceiling
    static constexpr int TEMP_TOLERANCE_DECIDEGREES = 50; // 5°C (temps stored as value * 10)

    // Fallback detection state (for printers without G-code layer markers)
    // Baseline values snapshot stale subject data at collector start so fallbacks
    // only trigger on actual changes, not leftover data from the previous print.
    int baseline_layer_{0};
    int baseline_progress_{0};
    std::atomic<bool> fallbacks_enabled_{false};
    std::atomic<helix::SubscriptionId> macro_subscription_id_{0};

    // Phase timing for duration prediction (protected by state_mutex_)
    std::map<int, std::chrono::steady_clock::time_point> phase_enter_times_;
    helix::PreprintPredictor predictor_;
    int loaded_temp_bucket_{0};

    // Duration-proportional progress weights (protected by state_mutex_)
    std::map<int, float> predicted_phase_weights_; ///< Phase -> fraction of total (0.0-1.0)
    float predicted_total_seconds_ = 0.0f;         ///< Total predicted pre-print duration
    int start_ext_temp_ = 0; ///< Extruder temp at collector start (decideg/10)
    int start_bed_temp_ = 0; ///< Bed temp at collector start (decideg/10)
    // Cached temperature readings for thread-safe access from calculate_progress_locked()
    // Updated from main thread in check_fallback_completion() and start()
    std::atomic<int> cached_ext_temp_{0};   ///< Current extruder temp (decideg/10)
    std::atomic<int> cached_ext_target_{0}; ///< Current extruder target (decideg/10)
    std::atomic<int> cached_bed_temp_{0};   ///< Current bed temp (decideg/10)
    std::atomic<int> cached_bed_target_{0}; ///< Current bed target (decideg/10)
    int last_remaining_ = 0;                ///< For monotonic bias
    bool fallback_completion_ = false;      ///< True if COMPLETE was triggered by timeout fallback

    // Bed mesh probe tracking (protected by state_mutex_)
    // Parsed from G-code responses during BED_MESH phase to provide sub-phase
    // progress and per-probe time extrapolation for ETA.
    int mesh_probe_current_ = 0;
    int mesh_probe_total_ = 0;
    int mesh_probe_fallback_count_ = 0; ///< Unique probe POINTS (not samples) counted from fallback
    std::chrono::steady_clock::time_point mesh_first_probe_time_;
    std::chrono::steady_clock::time_point mesh_last_probe_time_;
    float mesh_seconds_per_probe_ = 0.0f; ///< Running average from observed probe intervals

    // Dedupe state: Klipper's `samples: N` config emits N consecutive "probe at X,Y"
    // lines at the same position. We count unique (x,y) positions as "points", which
    // matches what the user expects (# points that will be probed, not raw sample
    // count). Reset on gap-detection and in reset().
    double mesh_last_probe_x_ = 0.0;
    double mesh_last_probe_y_ = 0.0;
    bool mesh_has_last_probe_pos_ = false;

    /// Max gap between consecutive probe lines before resetting counters.
    /// Handles printers that emit "probe at" for non-mesh operations (e.g.
    /// nozzle wipe) before the actual mesh calibration begins.
    static constexpr auto MESH_PROBE_GAP_RESET = std::chrono::seconds(30);

    // Pre-mesh probe buffering: don't auto-enter BED_MESH from probe lines
    // until we've seen enough consecutive probes to distinguish mesh calibration
    // from isolated PROBE commands (e.g. nozzle wipe on AD5M Klipper mod).
    int pre_mesh_probe_count_ = 0;
    std::chrono::steady_clock::time_point pre_mesh_last_probe_time_;
    static constexpr int MESH_PROBE_ENTRY_THRESHOLD = 3;

    // Targets used in last compute_predicted_weights() call — used to detect
    // when heater targets change (e.g. macro issues M109 after bed-first heating)
    // and weights need recomputing to include the new heating phase.
    int weights_ext_target_ = 0;
    int weights_bed_target_ = 0;

    // LVGL timer for periodic ETA updates (main thread only)
    lv_timer_t* eta_timer_ = nullptr;
    static constexpr uint32_t ETA_UPDATE_INTERVAL_MS = 5000;

    /**
     * @brief Update ETA display from timer callback (main thread)
     */
    void update_eta_display();

    /**
     * @brief Query configfile for bed_mesh probe_count to get expected total
     *
     * Fires an async query when entering BED_MESH phase. The response sets
     * mesh_probe_total_ for deterministic progress and ETA extrapolation.
     */
    void query_mesh_probe_count();

    /**
     * @brief Feed current temperature readings to ThermalRateModel during heating phases
     */
    void feed_thermal_sample();

    /**
     * @brief Load prediction entries from helix::Config on start()
     */
    void load_prediction_history();

    /**
     * @brief Compute duration-proportional weights from predicted durations
     *
     * Combines thermal model estimates for heating phases with predictor
     * estimates for non-heating phases to assign each phase a weight
     * proportional to its fraction of total predicted time.
     */
    void compute_predicted_weights();

    /**
     * @brief Compute heating fraction for current phase based on temperature progress
     *
     * Returns 0.0-1.0 representing how far the current heater has progressed
     * from its starting temperature toward the target.
     */
    float compute_heating_fraction() const;

    /**
     * @brief Save current print's phase timings to prediction history
     *
     * Called on COMPLETE. Computes per-phase durations from timestamps,
     * adds entry to predictor, and persists to Config.
     */
    void save_prediction_entry();
};
