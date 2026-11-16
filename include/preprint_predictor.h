// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"
#include "print_start_plan.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
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
    /// Phases this start was told to skip (option toggles turned off);
    /// nullopt when nothing said, which reads as unknown, never as "none".
    std::optional<std::set<int>> skipped_phases;
    /// Nozzle+bed heating this start was priced for, in seconds; nullopt when
    /// the start bypassed pricing (legacy entries, a start we did not arm).
    std::optional<int> heating_seconds;
    /// The plan this start declared; nullopt for a start without one
    std::optional<PreprintDeclaredDurations> declared;
};

/**
 * @brief Heating time the thermal model prices for one start
 *
 * The predictor never records heating; `ThermalRateManager` estimates it from
 * the start temperatures and the targets.
 */
struct PreprintHeating {
    float nozzle_seconds = 0.0f;
    float bed_seconds = 0.0f;
    /// False while the nozzle's target is unknown (a bed-first macro issues
    /// its M109 late), so the estimate holds a placeholder for that climb.
    bool nozzle_target_known = false;

    /// Nozzle+bed seconds this start owes, or nullopt while its heating cannot
    /// be priced: 0 means targets known and already reached.
    [[nodiscard]] std::optional<float> priced_seconds() const {
        if (!nozzle_target_known) {
            return std::nullopt;
        }
        return nozzle_seconds + bed_seconds;
    }
};

/**
 * @brief One pre-print estimate: the phase shape and the absolute total
 */
struct PreprintEstimate {
    std::map<int, float> durations; ///< phase_enum -> seconds, heating included
    float total_seconds = 0.0f;
};

/**
 * @brief What this start does about a phase the predictor may expect
 *
 * phase_enum -> true when the start runs the phase (added if the expected
 * phases lack it), false when it skips it (removed if they have it). A phase
 * left out keeps whatever the history or defaults say.
 */
using PreprintPhaseOverrides = std::map<int, bool>;

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

    /// Bed temperature at or above which a start counts as warm
    static constexpr float kWarmBedC = 40.0f;

    /// The temp_bucket a start with the bed at @p bed_c belongs to: 2 (warm)
    /// at or above kWarmBedC, else 1 (cold)
    [[nodiscard]] static int temp_bucket_for(float bed_c);

    /// The phases @p overrides skips, as a run's history entry records them
    [[nodiscard]] static std::set<int> phases_skipped_by(const PreprintPhaseOverrides& overrides);

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

    /// Homing estimate floor. A macro that goes straight from G28 into its
    /// next narrated step records a homing phase of a few seconds or none.
    static constexpr int kMinHomingSeconds = 20;

    /**
     * @brief Predicted homing seconds, never below kMinHomingSeconds
     *
     * The one homing estimate the pre-print collector and the print details
     * estimate share.
     */
    [[nodiscard]] int predicted_homing_seconds() const;

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

    /// Generic per-phase durations, used when the printer database has no
    /// `print_start_default_phases` for the printer and for a phase a start
    /// adds that the expected phases lack.
    static std::map<int, int> generic_phase_durations();

    /// Nozzle-heating placeholder while the nozzle target is unknown.
    static constexpr float kUnknownNozzleHeatSeconds = 60.0f;

    /**
     * @brief The pre-print estimate the live countdown and the print details share
     *
     * Durations: the heating seconds, the predicted phases (history, else the
     * defaults) with homing at predicted_homing_seconds(), then @p overrides
     * applied. Total: with history, the recorded wall clock (which already
     * holds heating and every phase the start ran, matched or not) less each
     * skipped phase's share of it, plus the generic duration of a run phase
     * every entry recorded as skipped; any other run phase adds nothing. An
     * entry that recorded its priced heating replaces that wall clock with its
     * constant remainder plus the heating priced for this start, and a history
     * with none recorded is floored by this start's own heating. Without
     * history, the sum of the durations, a run phase the defaults lack added
     * at its generic duration.
     *
     * A declared duration beats the overrides, history and thermal price for its
     * phase, and a declared 0 removes the phase. With a plan the total comes only
     * from entries that also declared one: their mean residual plus this start's
     * heating and declared seconds, or the sum of the durations when none exist,
     * so a soak the wall clock already holds is never counted twice.
     */
    [[nodiscard]] PreprintEstimate estimate(const PreprintHeating& heating,
                                            const PreprintPhaseOverrides& overrides = {},
                                            const PreprintDeclaredDurations& declared = {}) const;

    /// @p heating with any declared bed or nozzle duration standing in for the
    /// thermal price; a declared nozzle counts as a known target
    [[nodiscard]] static PreprintHeating
    with_declared_heating(PreprintHeating heating, const PreprintDeclaredDurations& declared);

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
     * @brief Serialize entries to the /print_start_history/entries array shape
     *
     * The inverse of load_entries_from_config(), and kept beside it so both
     * directions of the stored format are read in one place.
     *
     * @return JSON array, one object per entry
     */
    [[nodiscard]] static json entries_to_json(const std::vector<PreprintEntry>& entries);

    /**
     * @brief Record one finished pre-print in Config's print_start_history
     *
     * Every stored entry is kept except the oldest of the new entry's own
     * (temp bucket, window) population beyond MAX_ENTRIES, and the oldest
     * overall beyond kMaxStoredEntries. Writes Config; does not save it.
     */
    static void append_to_config(const PreprintEntry& entry);

    /// Cap on the whole stored history, across buckets and windows
    static constexpr size_t kMaxStoredEntries = 15;

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

    /// Total for a start with a plan, from the entries that declared one;
    /// nullopt when none can give a residual
    [[nodiscard]] std::optional<float> planned_total(const PreprintDeclaredDurations& declared,
                                                     const std::map<int, float>& durations) const;

    /// Every entry records @p phase as skipped (false when any entry is silent)
    [[nodiscard]] bool every_entry_skipped(int phase) const;

    /// @p value per entry, averaged with compute_weights() over the entries it
    /// returns a value for (nullopt skips one); nullopt when it skips them all
    [[nodiscard]] std::optional<double> weighted_entry_mean(
        const std::function<std::optional<double>(const PreprintEntry&)>& value) const;

    std::vector<PreprintEntry> entries_;
};

} // namespace helix
