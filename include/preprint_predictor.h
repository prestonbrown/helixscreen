// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace helix {

/// Whether the printer bed was cold or warm at print start
enum class StartCondition { COLD, WARM };

/**
 * @brief Which measurement window a timing entry describes
 *
 * The pre-print collector is armed either when the printer reports its own
 * print-start edge, or at user commit. Commit arming places any host-side
 * pre-start block - a forced bed mesh, a printer setup macro - inside the
 * measured window, which on some printers is minutes of extra work. The two
 * populations must not be averaged together: doing so produces an estimate
 * wrong for both, and feeds a too-small predicted total into the collector's
 * adaptive timeout, which can then complete the pre-print while it is still
 * running.
 *
 * As a *filter*, Unknown means "no window filter" - the same dual meaning
 * `temp_bucket == 0` already carries.
 */
enum class PreprintWindow : int {
    Unknown = 0,      ///< Legacy entry, recorded before commit arming existed
    PrinterEdge = 1,  ///< Measured from the printer's own print-start edge
    HostPreStart = 2, ///< Window includes a host-side pre-start block
};

/**
 * @brief A single recorded pre-print timing entry
 *
 * Captures per-phase durations from one print start sequence.
 * Phase keys are PrintStartPhase enum int values.
 */
struct PreprintEntry {
    int total_seconds;                  ///< Total pre-print duration
    int64_t timestamp;                  ///< Unix timestamp when entry was recorded
    std::map<int, int> phase_durations; ///< phase_enum -> seconds
    int temp_bucket{
        0}; ///< Bed temp at start: 1 = cold (<40°C), 2 = warm (≥40°C); 0 = unknown/legacy
    PreprintWindow window{PreprintWindow::Unknown}; ///< Which window this entry measured
};

/**
 * @brief Predicts pre-print duration from historical per-phase timing
 *
 * Tracks up to 10 entries and computes exponential time-decay weighted averages
 * to predict future pre-print remaining time. Weighting uses lambda=0.23 decay,
 * giving the oldest of 10 entries ~10% of the weight of the newest.
 *
 * Supports cold/warm start condition bucketing and applies MAD-based per-phase
 * anomaly rejection (3x MAD threshold) to filter outliers.
 *
 * Pure logic class with no LVGL or Config dependencies.
 */
class PreprintPredictor {
  public:
    /// Maximum entries to keep (FIFO)
    static constexpr int MAX_ENTRIES = 10;

    /**
     * @brief Load entries from storage, optionally filtering by start condition
     *
     * Trims to MAX_ENTRIES (FIFO, keeping newest).
     */
    void load_entries(const std::vector<PreprintEntry>& entries,
                      StartCondition condition = StartCondition::COLD);

    /**
     * @brief Load entries with legacy temp_bucket filtering (backward compat)
     *
     * bucket 0 = no filter, bucket 1 = cold, bucket 2 = warm,
     * other values use simplified mapping.
     */
    void load_entries(const std::vector<PreprintEntry>& entries, int temp_bucket,
                      PreprintWindow window = PreprintWindow::Unknown);

    /**
     * @brief Does this entry belong to the requested measurement window?
     *
     * Exposed for testing the legacy-entry asymmetry directly.
     */
    [[nodiscard]] static bool entry_matches_window(const PreprintEntry& e, PreprintWindow filter);

    /**
     * @brief Add a single entry, enforcing FIFO trim to MAX_ENTRIES
     */
    void add_entry(const PreprintEntry& entry);

    /**
     * @brief Get current entries (for persistence)
     */
    [[nodiscard]] std::vector<PreprintEntry> get_entries() const;

    /**
     * @brief Weighted-average wall-clock pre-print duration
     *
     * Uses the `total_seconds` field from history entries (exponential
     * time-decay weighted, same weights as predicted_phases()). This is the
     * honest end-to-end duration including heating and any unmapped macro
     * time — NOT the sum of predicted_phases(), which silently loses time
     * for phases the matcher failed to detect.
     *
     * With no history, falls back to the sum of default_phase_durations().
     */
    [[nodiscard]] int predicted_total() const;

    /**
     * @brief Per-phase predicted durations (phase_enum -> seconds)
     */
    [[nodiscard]] std::map<int, int> predicted_phases() const;

    /**
     * @brief Real-time remaining seconds estimate
     *
     * @param completed_phases Set of phase enum ints already completed
     * @param current_phase Current phase enum int (0=IDLE, no contribution)
     * @param elapsed_in_current_phase_seconds Seconds spent in current phase
     * @return Estimated remaining seconds, 0 if no predictions
     */
    [[nodiscard]] int remaining_seconds(const std::set<int>& completed_phases, int current_phase,
                                        int elapsed_in_current_phase_seconds) const;

    /**
     * @brief Whether any predictions can be made
     */
    [[nodiscard]] bool has_predictions() const;

    /**
     * @brief Default phase durations for when no history is available
     *
     * Returns reasonable defaults for non-heating phases based on
     * typical printer behavior.
     */
    static std::map<int, int> default_phase_durations();

    /**
     * @brief Load entries from Config's print_start_history
     *
     * Single source of truth for Config→PreprintEntry deserialization.
     * Used by both PrintStartCollector and predicted_total_from_config().
     *
     * @return Parsed entries (may be empty)
     */
    [[nodiscard]] static std::vector<PreprintEntry> load_entries_from_config();

    /**
     * @brief Load history from Config and return predicted total seconds
     *
     * Convenience method for UI code that needs the prediction without
     * access to the PrintStartCollector's predictor instance.
     *
     * @return Predicted pre-print seconds, or 0 if no history
     */
    [[nodiscard]] static int predicted_total_from_config();

  private:
    /// Exponential time-decay weights (oldest=low, newest=high), normalized to sum=1
    [[nodiscard]] std::vector<double> compute_weights() const;

    std::vector<PreprintEntry> entries_;
};

} // namespace helix
