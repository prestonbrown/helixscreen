// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pid_progress_tracker.h"

#include <algorithm>

void PidProgressTracker::start(Heater heater, int target_temp, float current_temp) {
    phase_ = Phase::HEATING;
    heater_ = heater;
    target_temp_ = target_temp;
    start_temp_ = current_temp;
    last_temp_ = current_temp;
    start_tick_ = 0;
    last_tick_ = 0;
    thermal_model_.reset(current_temp);
    thermal_model_.set_default_rate(heater == Heater::EXTRUDER ? kDefaultExtruderHeatRate
                                                               : kDefaultBedHeatRate);
    was_above_target_ = false;
    oscillation_count_ = 0;
    first_oscillation_tick_ = 0;
    last_oscillation_tick_ = 0;
    measured_cycle_period_ = 0;
    has_measured_cycle_ = false;
    has_sample_progress_ = false;
    sample_number_ = 0;
    sample_total_ = 0;
}

void PidProgressTracker::on_temperature(float temp, uint32_t tick_ms) {
    if (phase_ == Phase::IDLE || phase_ == Phase::COMPLETE)
        return;

    // Initialize start tick on first reading
    if (start_tick_ == 0)
        start_tick_ = tick_ms;

    bool is_above = temp >= static_cast<float>(target_temp_);

    if (phase_ == Phase::HEATING) {
        // Delegate heating rate measurement to ThermalRateModel
        thermal_model_.record_sample(temp, tick_ms);

        // Detect phase transition: must go above target first, then come back down
        if (is_above) {
            was_above_target_ = true;
        } else if (was_above_target_ && !is_above) {
            // First downward crossing — oscillations begin
            phase_ = Phase::OSCILLATING;
            oscillation_count_ = 1;
            first_oscillation_tick_ = tick_ms;
            last_oscillation_tick_ = tick_ms;
        }
    } else if (phase_ == Phase::OSCILLATING) {
        // Count downward zero-crossings
        bool was_above = last_temp_ >= static_cast<float>(target_temp_);
        if (was_above && !is_above) {
            oscillation_count_++;
            // Measure cycle period from first oscillation
            if (oscillation_count_ > 1) {
                float total_osc_s = static_cast<float>(tick_ms - first_oscillation_tick_) / 1000.0f;
                measured_cycle_period_ = total_osc_s / static_cast<float>(oscillation_count_ - 1);
                has_measured_cycle_ = true;
            }
            last_oscillation_tick_ = tick_ms;
        }
    }

    last_temp_ = temp;
    last_tick_ = tick_ms;
}

void PidProgressTracker::mark_complete() {
    phase_ = Phase::COMPLETE;
}

float PidProgressTracker::heating_fraction() const {
    float heat_est = thermal_model_.estimate_seconds(start_temp_, static_cast<float>(target_temp_));
    float osc_est = best_oscillation_duration();
    float total = heat_est + osc_est;
    if (total <= 0)
        return 0.5f;
    return std::clamp(heat_est / total, 0.05f, 0.80f);
}

int PidProgressTracker::progress_percent() const {
    if (phase_ == Phase::IDLE)
        return 0;
    if (phase_ == Phase::COMPLETE)
        return 100;

    float heat_frac = heating_fraction();

    if (phase_ == Phase::HEATING) {
        if (target_temp_ <= static_cast<int>(start_temp_))
            return 0;
        // Progress proportional to temperature, scaled to heating's share of total time
        float temp_progress =
            (last_temp_ - start_temp_) / (static_cast<float>(target_temp_) - start_temp_);
        temp_progress = std::clamp(temp_progress, 0.0f, 1.0f);
        return static_cast<int>(temp_progress * heat_frac * 95.0f);
    }

    // Oscillating: heat_frac*95% to 95%
    float osc_bar = (1.0f - heat_frac) * 95.0f;
    float base = heat_frac * 95.0f;

    if (has_sample_progress_ && sample_total_ > 0) {
        float sample_progress =
            static_cast<float>(sample_number_) / static_cast<float>(sample_total_);
        int pct = static_cast<int>(base + sample_progress * osc_bar);
        return std::min(pct, 95);
    }

    // Cycle-based progress
    float cycle_progress =
        static_cast<float>(oscillation_count_) / static_cast<float>(kExpectedCycles);
    cycle_progress = std::clamp(cycle_progress, 0.0f, 1.0f);
    int pct = static_cast<int>(base + cycle_progress * osc_bar);
    return std::min(pct, 95);
}

std::optional<int> PidProgressTracker::eta_seconds() const {
    if (phase_ == Phase::IDLE || phase_ == Phase::COMPLETE)
        return std::nullopt;

    if (phase_ == Phase::HEATING) {
        float rate = best_heat_rate();
        if (rate <= 0)
            return std::nullopt;

        float remaining_degrees = static_cast<float>(target_temp_) - last_temp_;
        if (remaining_degrees <= 0)
            remaining_degrees = 0;
        float heating_remaining = remaining_degrees * rate;
        float osc_est = best_oscillation_duration();
        return static_cast<int>(heating_remaining + osc_est);
    }

    // Oscillating
    if (has_measured_cycle_ && oscillation_count_ > 0) {
        int remaining_cycles = kExpectedCycles - oscillation_count_;
        if (remaining_cycles <= 0)
            return 0;
        // Subtract time elapsed within the current cycle for smooth countdown
        float remaining = static_cast<float>(remaining_cycles) * measured_cycle_period_;
        if (last_tick_ > last_oscillation_tick_) {
            float elapsed_in_cycle =
                static_cast<float>(last_tick_ - last_oscillation_tick_) / 1000.0f;
            remaining -= elapsed_in_cycle;
        }
        return static_cast<int>(std::max(remaining, 0.0f));
    }

    // No measured cycle yet — use best oscillation estimate minus elapsed
    float osc_est = best_oscillation_duration();
    if (first_oscillation_tick_ > 0 && last_tick_ > first_oscillation_tick_) {
        float elapsed = static_cast<float>(last_tick_ - first_oscillation_tick_) / 1000.0f;
        float remaining = osc_est - elapsed;
        return static_cast<int>(std::max(remaining, 0.0f));
    }

    return static_cast<int>(osc_est);
}

float PidProgressTracker::measured_oscillation_duration() const {
    if (!has_measured_cycle_ || oscillation_count_ < 2)
        return 0;
    // Total duration from first oscillation to last
    return static_cast<float>(last_oscillation_tick_ - first_oscillation_tick_) / 1000.0f;
}

void PidProgressTracker::set_history(float heat_rate_s_per_deg, float oscillation_duration_s) {
    thermal_model_.load_history(heat_rate_s_per_deg);
    if (oscillation_duration_s > 0.0f) {
        hist_oscillation_duration_ = oscillation_duration_s;
        has_history_ = true;
    }
}

void PidProgressTracker::on_sample_progress(int sample, int estimated_total) {
    has_sample_progress_ = true;
    sample_number_ = sample;
    sample_total_ = estimated_total;
}

float PidProgressTracker::default_heat_rate() const {
    return heater_ == Heater::EXTRUDER ? kDefaultExtruderHeatRate : kDefaultBedHeatRate;
}

float PidProgressTracker::default_oscillation_duration() const {
    return heater_ == Heater::EXTRUDER ? kDefaultExtruderOscillation : kDefaultBedOscillation;
}

float PidProgressTracker::best_heat_rate() const {
    return thermal_model_.best_rate();
}

float PidProgressTracker::best_oscillation_duration() const {
    if (has_history_ && hist_oscillation_duration_ > 0)
        return hist_oscillation_duration_;
    return default_oscillation_duration();
}
