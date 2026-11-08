// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

/**
 * @brief Reusable EMA-based thermal heating rate model
 *
 * Tracks temperature samples over time and computes a smoothed heating rate
 * (seconds per degree) using exponential moving average. PID calibration
 * (PidProgressTracker) and pre-print timing share this one algorithm.
 *
 * Units: rate is always in seconds per degree Celsius (s/°C).
 * Higher rate = slower heating.
 */
class ThermalRateModel {
  public:
    // EMA constants for live measurement smoothing
    static constexpr float kMinDeltaFromLast = 2.0f; // °C between samples to compute rate
    static constexpr float kMinTotalMovement = 5.0f; // °C from start before seeding EMA
    static constexpr float kEmaNewWeight = 0.3f;     // Weight for new instantaneous rate
    static constexpr float kEmaOldWeight = 0.7f;     // Weight for existing EMA rate

    /// A heater that has not beaten its highest reading by kRiseEpsilonC for
    /// this long is holding, not climbing. Until it rises again every reading
    /// restarts the step, so a hold between two climbs (a probing temperature,
    /// then the print temperature) is counted in neither.
    static constexpr uint32_t kStallReanchorMs = 30000;
    static constexpr float kRiseEpsilonC = 1.0f;

    // Blending constants for persistence (saving to history)
    static constexpr float kSaveNewWeight = 0.7f; // Weight for current measurement when saving
    static constexpr float kSaveOldWeight = 0.3f; // Weight for historical rate when saving

    // Fallback when no history or measurement exists.
    // Most extruders heat at ~0.2-0.3 s/°C, beds at ~1.0-2.0 s/°C.
    static constexpr float kFallbackDefaultRate = 0.25f; // s/°C (typical extruder)

    /// Feed a temperature sample. tick_ms should be monotonically increasing.
    void record_sample(float temp_c, uint32_t tick_ms);

    /// Report the heater's target before a sample. A raised target starts a
    /// new step at this reading: the time the heater idled under its old
    /// target is not heating, and a step spanning it reads seconds per degree
    /// several times too slow. Once a target has been reported, a sample
    /// counts toward the climb only while that target is positive and above
    /// the reading.
    void note_target(float target_c, float temp_c, uint32_t tick_ms);

    /// Estimate seconds for a climb from `current` to `target` at
    /// climb_rate_for(target - current). Returns 0 if current >= target.
    float estimate_seconds(float current, float target) const;

    /// Current measured rate, or nullopt if not yet established. Weighted
    /// toward the latest steps, so it tracks how the heater is heating now.
    std::optional<float> measured_rate() const;

    /// Best available rate for the degrees just ahead: measured > history >
    /// default. Near a target it is the slow tail rate, so it suits the time
    /// left in a climb under way, not a whole climb.
    float best_rate() const;

    /// Best available rate for a whole climb: seconds per degree across every
    /// step measured so far > history > default. History and the database
    /// rates are whole-climb rates too, so the three agree in kind; the tail
    /// rate applied to a whole climb overstates it several times over.
    float climb_rate() const;

    /// climb_rate() for a climb of `span_c` degrees. A heater's first degrees
    /// are its fastest, so until the measured climb covers the span the
    /// measured average is blended with a known rate (history, or a database
    /// rate given as the default), weighted by the share of the span
    /// measured. With no known rate the measured average stands alone: a
    /// size guess is no better than the fast start.
    float climb_rate_for(float span_c) const;

    /// Load a historical rate from previous sessions.
    void load_history(float rate_s_per_deg);

    /// Rate to persist: seconds per degree across every step of the climb
    /// measured so far, blended with history. A heater's last degrees are its
    /// slowest, so the latest steps alone would overstate a whole climb.
    /// Returns 0 if no measurement has been taken.
    float blended_rate_for_save() const;

    /// Set a custom default rate (overrides kFallbackDefaultRate).
    /// @param measured The rate was measured on this printer (a database
    ///        entry's thermal_rates), not guessed from its size.
    void set_default_rate(float rate_s_per_deg, bool measured = false);

    /// Reset all measurement state for a new tracking session.
    void reset(float start_temp);

  private:
    float measured_heat_rate_ = 0.0f;
    bool has_measured_heat_rate_ = false;

    float hist_heat_rate_ = 0.0f;
    bool has_history_ = false;

    float default_rate_ = kFallbackDefaultRate;
    bool default_measured_ = false;

    float last_temp_ = 0.0f; ///< where the current step started
    uint32_t last_tick_ = 0;
    uint32_t start_tick_ = 0;

    /// Highest reading since the last rise; while holding it follows the
    /// reading down to within kRiseEpsilonC, so a heater that cooled can
    /// climb again, and a swing smaller than that never counts as a rise.
    float high_temp_ = 0.0f;
    uint32_t last_rise_tick_ = 0;
    bool holding_ = false;

    float target_c_ = 0.0f; ///< last target note_target() reported
    /// Whether a caller reports targets. Without them (the PID tracker) every
    /// rise counts; with them only a rise under a positive target does.
    bool target_known_ = false;
    float climb_seconds_ = 0.0f; ///< time across counted steps
    float climb_degrees_ = 0.0f; ///< degrees across counted steps
};

namespace helix {
class Config;
} // namespace helix

/**
 * @brief Manages per-heater ThermalRateModel instances with persistence
 *
 * Singleton that provides printer-archetype-aware defaults and persists
 * learned heating rates to Config between sessions.
 *
 * @thread_safety Main thread only. No internal synchronization.
 * apply_archetype_defaults() must be called via ui_queue_update() from background threads.
 */
class ThermalRateManager {
  public:
    /// Heaters whose rates load_from_config() reads back between sessions; a
    /// rate kept under any other name is never used again.
    static constexpr std::array<const char*, 2> kPersistedHeaters = {"extruder", "heater_bed"};

    static ThermalRateManager& instance();
    ThermalRateModel& get_model(const std::string& heater_name);
    float estimate_heating_seconds(const std::string& heater_name, float current_temp,
                                   float target_temp) const;
    void load_from_config(helix::Config& config);
    void save_to_config(helix::Config& config);
    /// Default heating rates for a printer with no learned history: the
    /// database entry's measured `thermal_rates` where it has them, otherwise
    /// a guess from the bed's X extent.
    void apply_archetype_defaults(float bed_x_max, const std::string& printer_type);
    void reset(); // For testing

    ThermalRateManager() = default;

  private:
    std::map<std::string, ThermalRateModel> models_;
};
