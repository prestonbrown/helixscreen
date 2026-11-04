// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dryer contract a filament system or a chamber heater publishes: what the
// dryer can do (DryerInfo), the presets offered (DryingPreset) and a request to
// start one (DryingRequest). Its own header so a backend that only answers the
// dryer question (chamber_heater_backend.h) does not include ams_types.h, the
// slot, unit and endless-spool model behind it; ams_types.h includes this.

#pragma once

#include "filament_database.h" // filament::get_drying_presets_by_group

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace helix {

// ============================================================================
// Dryer Types (for AMS systems with integrated drying)
// ============================================================================

/**
 * @brief Preset drying profile
 *
 * Standard drying profiles for common filament materials.
 * Can be overridden via settings.json "dryer_presets" array.
 */
struct DryingPreset {
    std::string name;       ///< Preset name (e.g., "PLA", "PETG", "ABS")
    float temp_c = 45.0f;   ///< Target temperature in Celsius
    int duration_min = 240; ///< Drying duration in minutes
    int fan_pct = 50;       ///< Fan speed percentage (0-100)

    /**
     * @brief Create a drying preset
     * @param n Preset name
     * @param t Temperature in Celsius
     * @param d Duration in minutes
     * @param f Fan speed percentage
     */
    DryingPreset(std::string n, float t, int d, int f = 50)
        : name(std::move(n)), temp_c(t), duration_min(d), fan_pct(f) {}
    DryingPreset() = default;
};

/**
 * @brief Dryer capability and state information
 *
 * Not all AMS systems have integrated dryers. Currently only ACE Pro
 * has dryer support. This struct provides a generic interface that other
 * backends can implement when dryer hardware becomes available.
 */
struct DryerInfo {
    bool supported = false;           ///< Does this AMS have a dryer?
    bool active = false;              ///< Currently drying?
    bool allows_during_print = false; ///< Can run while printing? (backend capability)

    // Current state
    float current_temp_c = 0.0f; ///< Current chamber temperature
    float target_temp_c = 0.0f;  ///< Target temperature (0 = off)
    int duration_min = 0;        ///< Total drying duration set
    int remaining_min = 0;       ///< Minutes remaining
    int fan_pct = 0;             ///< Current fan speed (0-100)

    // Hardware capabilities
    float min_temp_c = 35.0f;          ///< Minimum settable temperature
    float max_temp_c = 70.0f;          ///< Maximum settable temperature
    int max_duration_min = 720;        ///< Maximum drying time (12h default)
    int duration_step_min = 1;         ///< Granularity the firmware takes a duration in
    bool supports_fan_control = false; ///< Can fan speed be set independently?

    /**
     * Can a running cycle be retargeted without stopping it?
     *
     * Two flags because the hardware splits the same way. A backend can retarget the
     * heater mid-run and still have no way to move the clock: the temperature is a
     * setpoint it can re-send, while the duration is owned by a firmware timer that
     * only accepts a fresh cycle. Where a flag is false the caller must stop and
     * restart, which is a visible interruption and worth warning about, so the two
     * cases cannot be collapsed into one "adjustable" bool.
     *
     * Both default false: a backend that has not been checked against real hardware
     * should force the stop-and-restart path rather than silently drop an adjustment.
     */
    bool supports_live_temp = false;     ///< Target temp re-sends without stopping
    bool supports_live_duration = false; ///< Remaining time changes without stopping

    /// Can a cycle also end early once the box reads at or below a target relative
    /// humidity? The duration still bounds the cycle; the target only shortens it.
    bool supports_humidity_target = false;

    /// The target, in %RH, a start with none applies, when the backend knows it.
    /// Unset means the dryer either applies no target or does not say which.
    std::optional<float> default_humidity_target;

    /// Can a cycle be started and stopped on a chosen subset of this dryer's gates? True
    /// only where each gate has its own heater, so the subset is what actually heats.
    bool supports_gate_selection = false;

    /// Can a cycle turn the selected spools while it dries? Needs a motor that drives
    /// the spool itself, and an explicit gate selection to say which ones.
    bool supports_rotation = false;

    /// Highest humidity target, in %RH, a surface offers.
    static constexpr float kMaxHumidityTargetPct = 99.0f;

    /// @p pct as a humidity target this dryer takes, or nullopt for none: the dryer
    /// cannot take one, or @p pct is unset (0 or below). Above the ceiling clamps to it.
    [[nodiscard]] std::optional<float> humidity_target(float pct) const {
        if (!supports_humidity_target || !(pct > 0.0f)) {
            return std::nullopt;
        }
        return std::min(pct, kMaxHumidityTargetPct);
    }

    /// @p temp_c inside this dryer's settable range. A bound of 0 is unset and
    /// clamps nothing on its side. Every surface offering a preset clamps
    /// through here, so the value it shows is the value it sends.
    [[nodiscard]] float clamp_temp(float temp_c) const {
        if (max_temp_c > 0.0f && temp_c > max_temp_c) {
            temp_c = max_temp_c;
        }
        if (min_temp_c > 0.0f && temp_c < min_temp_c) {
            temp_c = min_temp_c;
        }
        return temp_c;
    }

    /// @p minutes rounded UP to whole duration steps, at least one step and at
    /// most max_duration_min. Rounding up keeps a preset from drying for less
    /// time than it names.
    [[nodiscard]] int clamp_duration(int minutes) const {
        const int step = duration_step_min > 0 ? duration_step_min : 1;
        int steps = (std::max(minutes, 0) + step - 1) / step;
        steps = std::max(steps, 1);
        int clamped = steps * step;
        if (max_duration_min > 0 && clamped > max_duration_min) {
            clamped = (max_duration_min / step) * step;
        }
        return clamped;
    }

    /**
     * @brief Get progress as percentage
     * @return 0-100 percentage, or -1 if not drying
     */
    [[nodiscard]] int get_progress_pct() const {
        if (!active || duration_min <= 0)
            return -1;
        int elapsed = duration_min - remaining_min;
        // Clamp to valid range (handles firmware reporting remaining > duration)
        if (elapsed < 0)
            elapsed = 0;
        if (elapsed > duration_min)
            elapsed = duration_min;
        return (elapsed * 100) / duration_min;
    }

    /**
     * @brief Check if dryer is at target temperature
     * @param tolerance_c Temperature tolerance in Celsius (default 2°C)
     * @return true if within tolerance of target
     */
    [[nodiscard]] bool is_at_temp(float tolerance_c = 2.0f) const {
        if (target_temp_c <= 0)
            return false;
        return std::abs(current_temp_c - target_temp_c) <= tolerance_c;
    }
};

/**
 * @brief Everything a drying start asks of a backend.
 *
 * Empty @p gates leaves the choice to the dryer, which is the whole unit or whatever
 * the firmware defaults to: exactly the start a backend sends with no selection at all.
 */
struct DryingRequest {
    float temp_c = 0.0f;
    int duration_min = 0;
    std::optional<float> humidity_pct; ///< Unset leaves the dryer's own target
    int fan_pct = -1;                  ///< -1 = backend default
    int unit = 0;
    std::vector<int> gates; ///< Global gate indices; needs DryerInfo::supports_gate_selection
    bool rotate = false;    ///< Needs DryerInfo::supports_rotation
};

/**
 * @brief Get default drying presets
 *
 * Returns presets derived from the filament database, one per compatibility group.
 * Uses filament::get_drying_presets_by_group() as the single source of truth.
 * These can be overridden via settings.json "dryer_presets" array.
 *
 * @return Vector of default DryingPreset structs
 */
inline std::vector<DryingPreset> get_default_drying_presets() {
    constexpr int kDefaultFanPct = 50;

    std::vector<DryingPreset> result;
    for (const auto& fp : filament::get_drying_presets_by_group()) {
        result.emplace_back(fp.name, static_cast<float>(fp.temp_c), fp.time_min, kDefaultFanPct);
    }
    return result;
}

} // namespace helix
