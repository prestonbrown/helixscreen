// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "bed_mesh_probe_parser.h"
#include "i_moonraker_client.h"
#include "preprint_predictor.h"
#include "print_start_position_classifier.h"
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
     * @param client helix::IMoonrakerClient for registering callbacks
     * @param state helix::PrinterState to update with phase progress
     */
    PrintStartCollector(helix::IMoonrakerClient& client, helix::PrinterState& state);

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
     * @brief Declare that a host-side pre-start block is part of this window
     *
     * Called when HelixScreen dispatches blocking pre-start gcode in front of
     * the job (a forced bed mesh, a printer setup macro). That work sits inside
     * the measured window, which on some printers is several minutes, so its
     * timings must not be averaged with printer-edge measurements.
     *
     * Safe to call after start(): it re-filters the loaded history in place.
     * Idempotent.
     */
    void note_host_side_pre_start();

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
     * @brief Record an observed current_layer value for this print
     *
     * Arms the layer-1 completion edge: the first time current_layer is seen at
     * 0 (or below 1) since this print started, the collector latches
     * layer_zero_seen_. The pre-print → printing hand-off requires this latch so
     * a stale positive layer carried over from the previous print (before
     * reset_for_new_print() has zeroed the subject) can't complete the new
     * print's pre-print phase. See MoonrakerManager::should_complete_preprint().
     *
     * @param current_layer The layer value just observed
     */
    void note_current_layer(int current_layer) {
        if (current_layer < 1) {
            layer_zero_seen_.store(true, std::memory_order_relaxed);
            return;
        }

        // A layer counter that ADVANCES while we are collecting is live data for
        // this print, which is the same proof a 0 -> >=1 edge gives. Tracking it
        // separately matters because the 0 sample is not guaranteed to be
        // observed: notify_status_update is coalesced, so a fast first layer (or
        // a reconnect part-way in) can deliver a first sample that is already
        // >= 1 and the zero edge never arrives. Without this the pre-print
        // overlay stays up for the entire print.
        int prev = first_layer_observed_.load(std::memory_order_relaxed);
        if (prev < 0) {
            first_layer_observed_.store(current_layer, std::memory_order_relaxed);
        } else if (current_layer > prev) {
            layer_advanced_.store(true, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Whether current_layer==0 has been observed since this print started
     */
    [[nodiscard]] bool has_seen_layer_zero() const {
        return layer_zero_seen_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Whether current_layer has increased since collection began.
     *
     * A stale positive carried over from the previous print is static, so it
     * can never satisfy this — which is what the layer-zero latch was guarding
     * against.
     */
    [[nodiscard]] bool has_seen_layer_advance() const {
        return layer_advanced_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Mark the prime/purge line as in progress (phase UPDATE, not completion)
     *
     * Called when print_stats.print_duration first goes positive while the real
     * first layer has NOT yet been reached (current_layer < 1). On firmwares
     * whose prime/purge line emits no observable gcode_response (Snapmaker U1:
     * the initial "G1 X110 E15" prime extrudes silently; the PRINT_PREEXTRUDING
     * action code only fires for a SECOND tool mid-print), print_duration going
     * 0->positive is the one real, observable signal that priming has begun.
     *
     * This advances the displayed phase to PURGING ("Priming...") but does NOT
     * complete the pre-print phase — completion stays gated on the genuine
     * current_layer 0->1 edge (MoonrakerManager::should_complete_preprint). A
     * no-op once already at COMPLETE or PURGING.
     */
    void note_priming();

    /**
     * @brief Record the printer's live bed-mesh presence
     *
     * Fed from the bed_mesh status stream. A mesh that disappears while the
     * collector is in CLEANING marks the start of leveling work (accurate Z
     * probing, bed-mesh corner validation) that Creality-class firmwares do
     * not echo to gcode_response — the display moves to BED_MESH ("Bed
     * Leveling...") instead of sitting on "Cleaning Nozzle..." through the
     * whole silent window. Mesh clears arriving before CLEANING are the
     * rough G28's own clear and carry no phase information.
     *
     * Thread-safe: called from the WebSocket background thread.
     */
    void note_bed_mesh_presence(bool present);

    /**
     * @brief Record the bed-mesh probe-area bounds (gcode mm)
     *
     * Fed from the same bed_mesh status updates as note_bed_mesh_presence().
     * The bounds anchor the position classifier's geometric zones: Z-probing
     * happens at the mesh centre, corner validation at the mesh corners, and
     * the K1-class wipe strip sits beyond mesh_max at the bed rear. Until
     * bounds arrive the classifier withholds all verdicts.
     *
     * Thread-safe: called from the WebSocket background thread.
     */
    void note_mesh_bounds(float x_min, float x_max, float y_min, float y_max);

    /**
     * @brief Feed one toolhead position sample (gcode mm)
     *
     * Fed from the toolhead.position subjects (subscribed already). When the
     * profile declares position_signals and the console has gone silent, the
     * inferred activity refines the status line: centre probes →
     * "Probing Z...", corner tour → "Checking Bed Mesh...", sweep march →
     * BED_MESH entry. Real gcode_response signals always win — this only
     * fills silence.
     *
     * Thread-safe: observers fire on the main thread (queued subject sets).
     */
    void note_position_sample(float x_mm, float y_mm, float z_mm);

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

    /// Record that the printer said something about its pre-print. Feeds the
    /// quiet gate on every timeout branch. Takes state_mutex_ itself, so do not
    /// call it while already holding the lock.
    void note_activity();

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
     * Only consulted when the active profile declares cfs_signals — the
     * vocabulary is vendor-specific and must not fire on another printer's
     * coincidental "percent" plus "num:" output.
     *
     * @return true if a K2/CFS signal was detected and handled
     */
    bool check_k2_cfs_signal(const std::string& line);

    /**
     * @brief Reset mesh probe counters on BED_MESH entry / sub-phase change
     *
     * Some firmwares (Snapmaker U1) route multiple distinct probe operations
     * through a single BED_MESH phase enum, varying only the status message
     * between them (Pre-scanning Bed, Levelling Bed, Detecting Plate,
     * Inspecting Bed). The probe counters need to reset between sub-phases
     * so the displayed "(N/M)" doesn't roll past M. Caller must hold
     * state_mutex_.
     */
    void maybe_reset_for_mesh_subphase_locked(helix::PrintStartPhase next_phase,
                                              const std::string& next_message);

    /**
     * @brief Update phase and recalculate progress (weighted mode)
     */
    void update_phase(helix::PrintStartPhase phase, const char* message);

    /**
     * @brief Update phase with explicit progress value (sequential mode)
     */
    void update_phase(helix::PrintStartPhase phase, const std::string& message, int progress);

    /**
     * @brief Relabel between the two heating phases from live temps (bed-first).
     *
     * Compare-and-swap: applies the relabel only if current_phase_ is STILL a
     * heating phase (HEATING_BED/HEATING_NOZZLE) at write time. A background
     * gcode signal may have advanced current_phase_ past heating between the
     * caller's temperature snapshot and this call; the CAS refuses in that case
     * so a newer non-heating phase is never regressed back to heating. The
     * label is derived from `resolved`.
     */
    void relabel_heating_phase(helix::PrintStartPhase resolved);

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
    helix::IMoonrakerClient& client_;
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

    /// When the printer last said anything about its pre-print: a profile
    /// pattern matched, a probe line arrived, or the phase advanced.
    ///
    /// The timeouts key off THIS, not off elapsed-since-start. A pre-print that
    /// is still narrating itself is not stuck however long it runs, and keying
    /// off elapsed time made the collector give up mid-sequence on any printer
    /// that meshes after heating — which then skipped the prediction save and
    /// froze the estimate that set the deadline in the first place.
    std::chrono::steady_clock::time_point last_activity_time_;

    // Profile for signal/pattern matching (set via set_profile() or loaded by start())
    std::shared_ptr<PrintStartProfile> profile_;

    // Universal patterns (not profile-specific)
    static const std::regex print_start_pattern_;
    static const std::regex completion_pattern_;
    static const std::regex respond_completion_pattern_;

    // Fallback detection constants
    static constexpr auto FALLBACK_TIMEOUT =
        std::chrono::seconds(300); ///< Last resort when no predictions
    /// Ungated final backstop. Every other timeout also requires the printer to
    /// have gone quiet; this one fires regardless, so a firmware that chatters
    /// forever still leaves Preparing. Must therefore sit above the longest
    /// legitimate pre-print: the K2 Plus runs ~1140s (heat, ~390s mesh, purge),
    /// and a cold-start ASA soak pushes that further.
    static constexpr auto ABSOLUTE_MAX_TIMEOUT =
        std::chrono::seconds(1800); ///< Hard ceiling (stuck detection)
    /// How long the printer must say nothing before a timeout may complete the
    /// pre-print. Longer than the gap between mesh probe points on a slow bed
    /// (the K2 spends ~5s per point, ~3s on a manual sweep) with margin for a
    /// heat-soak step that emits nothing at all.
    static constexpr auto PREPRINT_QUIET_TIMEOUT = std::chrono::seconds(90);
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
    /// Which window this run is measuring. Defaults to PrinterEdge: an
    /// externally started print never announces a host-side block, and neither
    /// does a screen-started print on a printer that runs none.
    helix::PreprintWindow window_{helix::PreprintWindow::PrinterEdge};

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
    std::chrono::steady_clock::time_point mesh_first_probe_time_;
    std::chrono::steady_clock::time_point mesh_last_probe_time_;
    float mesh_seconds_per_probe_ = 0.0f; ///< Running average from observed probe intervals

    /// Unique probe POINTS (not sample lines) counted from the "probe at X,Y"
    /// fallback, for firmware that emits no "Probing point N/M". Reset on
    /// gap-detection, on mesh sub-phase change, and in reset().
    helix::ProbePointCounter mesh_points_;

    // Sub-phase tracking within BED_MESH. Some firmwares (Snapmaker U1) route
    // multiple distinct probe operations through one phase enum but vary the
    // status message — e.g. Pre-scanning Bed → Levelling Bed → Detecting
    // Plate → Inspecting Bed, each emitting its own probe-line burst. We
    // reset the probe counters when this message changes while the phase
    // stays BED_MESH so the displayed (N/M) doesn't roll past M. Also used
    // as the human label when rendering "<sub-phase> (N/M)" so the user sees
    // which sub-phase they're in. Empty when not in BED_MESH.
    std::string current_mesh_message_;

    /// Last bed-mesh presence reported via note_bed_mesh_presence(). The
    /// leveling trigger is the present→absent edge, so an unknown initial
    /// state (no report yet) never fires it.
    bool bed_mesh_present_{false};

    /// Position-stream inference for silent pre-print windows. Fed by
    /// note_position_sample()/note_mesh_bounds(); guarded by state_mutex_.
    helix::PrintStartPositionClassifier position_classifier_;
    helix::PositionActivity last_position_activity_ = helix::PositionActivity::NONE;
    /// Anchor for the classifier's millisecond sample clock (set in start()).
    std::chrono::steady_clock::time_point position_clock_start_{};

    /// Max gap between consecutive probe lines before resetting counters.
    /// Handles printers that emit "probe at" for non-mesh operations (e.g.
    /// nozzle wipe) before the actual mesh calibration begins.
    static constexpr auto MESH_PROBE_GAP_RESET = std::chrono::seconds(30);

    // Pre-mesh probe buffering: don't auto-enter BED_MESH from probe lines
    // until we've seen enough distinct probe POINTS to distinguish mesh
    // calibration from isolated PROBE commands (e.g. nozzle wipe on AD5M
    // Klipper mod). Counting points rather than lines matters because Klipper
    // emits two lines per touch on firmware that reports z_compensation
    // separately, which halved the effective threshold.
    helix::ProbePointCounter pre_mesh_points_;
    std::chrono::steady_clock::time_point pre_mesh_last_probe_time_;

    /// Distinct pre-mesh probe points required before auto-entering BED_MESH.
    /// Must clear the largest non-mesh probe burst any firmware emits: K2 Plus
    /// BOX_NOZZLE_CLEAN touches 3 points on the wipe strip, Voron 2.4 QGL
    /// touches 4 corner pads, AD5M nozzle wipe touches 1-2. Every real mesh is
    /// far larger, so 5 costs nothing on the firmwares that need this path.
    static constexpr int MESH_PROBE_ENTRY_THRESHOLD = 5;

    // Targets used in last compute_predicted_weights() call — used to detect
    // when heater targets change (e.g. macro issues M109 after bed-first heating)
    // and weights need recomputing to include the new heating phase.
    int weights_ext_target_ = 0;
    int weights_bed_target_ = 0;

    // Silent-phase progression (firmwares with silent cleaning/purge macros).
    // temps_ready_time_ is set the first time temps become ready (and remains
    // set across subsequent ticks); silent_progression_idx_ tracks how many
    // SilentPhaseEntry items have already fired. See
    // PrintStartProfile::SilentPhaseEntry for semantics.
    std::chrono::steady_clock::time_point temps_ready_time_; // {} = not yet ready
    size_t silent_progression_idx_ = 0;

    // Set true the moment any real firmware signal is observed for this print
    // (HELIX:PHASE, K2/CFS tag, profile signal/pattern match, PRINT_START
    // marker, or RESPOND completion). Gates the proactive temperature
    // heuristic: once the firmware is actively narrating its PRINT_START
    // sequence it is authoritative, so the "temps ready → INITIALIZING"
    // fallback must not bounce the displayed phase back to the generic
    // "Preparing Print...". Atomic — written from the WebSocket background
    // thread (on_gcode_response) and read from the main thread
    // (check_fallback_completion).
    std::atomic<bool> real_signal_seen_{false};

    // Latched true the first time current_layer is observed < 1 since this
    // print started. Arms the layer-1 completion edge so a stale positive layer
    // from the previous print (before reset_for_new_print() zeroes the subject)
    // can't trigger the pre-print → printing hand-off. Reset in start()/reset().
    // Written/read on the main thread (LVGL observer callbacks); atomic for
    // consistency with the other cross-method flags above.
    std::atomic<bool> layer_zero_seen_{false};

    // Second, equally authoritative route out of Preparing for the case where
    // the 0 sample is never delivered: the first layer value seen while active
    // (-1 = none yet) and whether a later sample exceeded it. Same threading
    // note as layer_zero_seen_. Reset in start()/reset().
    std::atomic<int> first_layer_observed_{-1};
    std::atomic<bool> layer_advanced_{false};

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
     * @brief Compute the heating fraction for an arbitrary phase
     *
     * Concurrent-heat firmwares (K1/K2 class) run heating in parallel with
     * homing, wiping and meshing, so a heating phase that is no longer
     * current may still be physically running. This is the phase-aware
     * generalization compute_heating_fraction() delegates to.
     */
    float compute_heating_fraction_for_locked(helix::PrintStartPhase phase) const;

    /**
     * @brief True once the phase's heater has reached its target (within 2C)
     *
     * The completion criterion for heating phases on concurrent-heat
     * firmware: the marker passing means the chain moved on, not that the
     * heater finished.
     */
    bool heating_target_reached_locked(helix::PrintStartPhase phase) const;

    /**
     * @brief Enter BED_MESH and credit any buffered pre-mesh probes
     *
     * update_phase() clears the mesh counters, so the probes buffered while
     * approaching the mesh (console pre-mesh points, or samples seen before
     * the position classifier's sweep-march verdict) have to be re-applied
     * after it returns — they are the sweep's first points. Every BED_MESH
     * promotion path (console threshold, bed-mesh flap, sweep march) goes
     * through here so none of them drops the count.
     */
    void enter_bed_mesh_with_buffer(const char* message);

    /**
     * @brief Save current print's phase timings to prediction history
     *
     * Called on COMPLETE. Computes per-phase durations from timestamps,
     * adds entry to predictor, and persists to Config.
     */
    void save_prediction_entry();
};
