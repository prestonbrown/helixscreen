// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_temperature_utils.h"

#include "lvgl/lvgl.h"
#include "printer_temperature_state.h" // helix::ChamberMode

/**
 * @brief Tints heating icons with the shared 4-state thermal color and pulse effect
 *
 * This class provides visual feedback for heating progress on temperature icons.
 * The icon shows the same four-state thermal color as the temperature labels
 * (muted off / red heating / green at-temp / blue cooling), and pulses (opacity
 * oscillation) only while actively heating.
 *
 * The pulse is motion, so the user's "Animations" preference governs it: with
 * animations switched off the icon holds still at full opacity and keeps its
 * per-state color, so the four states remain distinguishable. Colors are never
 * gated — only the oscillation.
 *
 * Usage:
 *   HeatingIconAnimator animator;
 *   animator.attach(icon_widget);
 *   // Call on every temperature update (temperatures in decidegrees):
 *   animator.update(current_temp_deci, target_temp_deci);
 *   // Cleanup:
 *   animator.detach();
 *
 * State is classified by helix::ui::temperature::classify_heat_state() —
 * see HeatState for the four states and their thresholds. Note: 20 decidegrees
 * = 2°C tolerance.
 */
class HeatingIconAnimator {
  public:
    /**
     * @brief Heating states — shared with the temperature labels.
     *
     * The icon and the number must never disagree, so both classify through
     * helix::ui::temperature::classify_heat_state().
     */
    using State = helix::ui::temperature::HeatState;

    HeatingIconAnimator() = default;
    ~HeatingIconAnimator();

    // Non-copyable (owns animation state)
    HeatingIconAnimator(const HeatingIconAnimator&) = delete;
    HeatingIconAnimator& operator=(const HeatingIconAnimator&) = delete;

    // Movable
    HeatingIconAnimator(HeatingIconAnimator&& other) noexcept;
    HeatingIconAnimator& operator=(HeatingIconAnimator&& other) noexcept;

    /**
     * @brief Attach animator to an icon widget
     *
     * @param icon LVGL icon widget (created by ui_icon)
     */
    void attach(lv_obj_t* icon);

    /**
     * @brief Detach from icon and cleanup animations
     */
    void detach();

    /**
     * @brief Update heating state based on current and target temperatures
     *
     * Call this whenever temperature readings change. The animator will:
     * - Classify the new thermal state via classify_heat_state_with_mode()
     * - Update the icon's tint color for that state
     * - Start/stop pulse animation based on state transitions
     *
     * @param current_temp Current temperature in decidegrees (31.5°C = 315)
     * @param target_temp Target temperature in decidegrees (0 = heater off)
     * @param mode Chamber control mode. Defaults to Heating, which makes
     *             classify_heat_state_with_mode() a pure passthrough to
     *             classify_heat_state() — the correct behavior for nozzle/bed,
     *             which have no mode concept. Only the chamber ever passes
     *             Off/Maintaining.
     */
    void update(int current_temp, int target_temp,
                helix::ChamberMode mode = helix::ChamberMode::Heating);

    /**
     * @brief Refresh colors from theme (call after theme toggle)
     *
     * Re-fetches color values from the current theme and re-applies them.
     * This ensures the icon color updates when switching between light/dark mode.
     */
    void refresh_theme();

    /**
     * @brief Get current heating state
     */
    State get_state() const {
        return state_;
    }

    /**
     * @brief Check if animator is attached to an icon
     */
    bool is_attached() const {
        return icon_ != nullptr;
    }

  private:
    /// Temperature tolerance for "at target" detection in decidegrees (2°C = 20)
    static constexpr int TEMP_TOLERANCE = 20;

    /// Pulse animation opacity range (80% to 100%)
    static constexpr lv_opa_t PULSE_OPA_MIN = 204; // ~80%
    static constexpr lv_opa_t PULSE_OPA_MAX = 255; // 100%

    /// Pulse animation duration (one direction)
    static constexpr uint32_t PULSE_DURATION_MS = 400;

    lv_obj_t* icon_ = nullptr;
    State state_ = State::Off;

    int current_temp_ = 250; ///< Current temperature (decidegrees)
    int target_temp_ = 0;    ///< Target temperature (decidegrees)

    lv_color_t current_color_; ///< Current thermal-state color
    lv_opa_t current_opacity_ = LV_OPA_COVER;

    /// Last values apply_color() actually wrote to the widgets, and whether it
    /// has written anything yet. Each lv_obj_set_style_* refreshes the style and
    /// invalidates the widget whether or not the value moved, and a pulse step
    /// changes nothing but the opacity — so the color write is skipped unless
    /// the color really differs.
    lv_color_t applied_color_{};
    lv_opa_t applied_opacity_ = 0;
    bool style_written_ = false;

    bool pulse_active_ = false;

    /// Whether the user's "Animations" preference allows the pulse to run.
    /// Seeded by the observer's subscribe-time notification; true when the
    /// preference subject does not exist yet.
    bool animations_enabled_ = true;

    /**
     * @brief Start pulse animation (opacity oscillation)
     */
    void start_pulse();

    /**
     * @brief Stop pulse animation
     */
    void stop_pulse();

    /**
     * @brief Run or halt the pulse to match state_ and the animations preference
     *
     * The single place that decides whether the icon should be moving, so a
     * temperature update and a preference toggle can never reach different
     * conclusions.
     */
    void sync_pulse_to_state();

    /**
     * @brief Apply current color and opacity to icon
     */
    void apply_color();

    /**
     * @brief Write the current tint to one widget
     *
     * @param obj        Icon widget or one of its children
     * @param with_color Write the color as well as the opacity
     */
    void tint(lv_obj_t* obj, bool with_color) const;

    /**
     * @brief Register the icon's delete callback and both observers against `this`
     *
     * Shared by attach() and the move operations: LVGL stores the owner pointer
     * inside each registration, so an animator that takes over another's icon
     * has to re-register rather than inherit.
     */
    void install_hooks();

    /**
     * @brief Take over @p other's icon, leaving nothing registered against it
     *
     * @param other Animator being moved from; detached by the time this returns
     */
    void adopt_from(HeatingIconAnimator& other);

    /**
     * @brief Get the off-state color, shared with the temperature label (text_muted)
     */
    lv_color_t get_secondary_color();

    /**
     * @brief RAII observer for theme/dark mode changes
     * ObserverGuard auto-removes the observer on destruction/reset.
     */
    ObserverGuard theme_observer_;

    /**
     * @brief RAII observer for the user's "Animations" preference
     */
    ObserverGuard animations_observer_;

    /**
     * @brief Static callback for theme change observer
     */
    static void theme_change_cb(lv_observer_t* observer, lv_subject_t* subject);

    /**
     * @brief Static callback for the animations-preference observer
     */
    static void animations_pref_cb(lv_observer_t* observer, lv_subject_t* subject);

    /**
     * @brief Animation callback for pulse effect
     */
    static void pulse_anim_cb(void* var, int32_t value);

    /**
     * @brief LV_EVENT_DELETE callback — auto-detaches when the icon widget is destroyed
     *
     * This prevents use-after-free when a shared TemperatureService outlives the
     * overlay panel whose children include the attached icon (issue #177).
     */
    static void icon_delete_cb(lv_event_t* e);
};
