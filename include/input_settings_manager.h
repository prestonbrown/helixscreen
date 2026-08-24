// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

namespace helix {

/**
 * @brief Domain-specific manager for input/scroll settings
 *
 * Owns all input-related LVGL subjects and persistence:
 * - scroll_throw (momentum decay rate, 5-50)        — restart required
 * - scroll_limit (pixels before scrolling starts, 1-20) — restart required
 * - jitter_threshold (touch jitter dead zone px, 0-30) — restart required
 * - scroll_guard (suppress phantom click after scroll)  — restart required
 * - debug_touches (draw ripple at each touch point)     — live-applied
 *
 * Thread safety: Single-threaded, main LVGL thread only.
 */
class InputSettingsManager {
  public:
    static InputSettingsManager& instance();

    // Non-copyable
    InputSettingsManager(const InputSettingsManager&) = delete;
    InputSettingsManager& operator=(const InputSettingsManager&) = delete;

    /** @brief Initialize LVGL subjects and load from Config */
    void init_subjects();

    /** @brief Deinitialize LVGL subjects (called by StaticSubjectRegistry) */
    void deinit_subjects();

    // =========================================================================
    // GETTERS / SETTERS
    // =========================================================================

    /**
     * @brief Get scroll throw (momentum decay rate)
     * @return Scroll throw value (5-50, higher = faster decay)
     */
    int get_scroll_throw() const;

    /**
     * @brief Set scroll throw (momentum decay rate)
     *
     * Persists to config. Requires restart to take effect.
     *
     * @param value Scroll throw (5-50)
     */
    void set_scroll_throw(int value);

    /**
     * @brief Get scroll limit (pixels before scrolling starts)
     * @return Scroll limit in pixels (1-20)
     */
    int get_scroll_limit() const;

    /**
     * @brief Set scroll limit (pixels before scrolling starts)
     *
     * Persists to config. Requires restart to take effect.
     *
     * @param value Scroll limit in pixels (1-20)
     */
    void set_scroll_limit(int value);

    /**
     * @brief Get the long-press hold time (ms) applied globally to the pointer
     *        input device. Governs every long-press in the app (home grid edit
     *        mode, file-card delete, macro edit, etc.), not just edit mode.
     * @return Hold time in ms (300-1500; default 500)
     */
    int get_long_press_time() const;

    /**
     * @brief Set the global long-press hold time.
     *
     * Persists to config and applies LIVE (no restart): the filer's #1245
     * report was that 500ms is easy to cross with a resting tablet finger, so
     * this lets the user raise it. Reaches DisplayManager's pointer indev
     * directly via lv_indev_set_long_press_time.
     *
     * @param value Hold time in ms (300-1500)
     */
    void set_long_press_time(int value);

    /** @brief Get jitter threshold in pixels (0-30; 0 disables) */
    int get_jitter_threshold() const;

    /**
     * @brief Set jitter threshold (touch coordinate dead zone)
     *
     * Persists to config. Requires restart to take effect.
     *
     * @param value Dead zone in pixels (0-30; 0 disables)
     */
    void set_jitter_threshold(int value);

    /** @brief Get scroll guard enable state */
    bool get_scroll_guard() const;

    /**
     * @brief Set scroll guard (suppress phantom clicks after scrolling)
     *
     * Persists to config. Requires restart to take effect.
     */
    void set_scroll_guard(bool enabled);

    /** @brief Get touch debug visualization state */
    bool get_debug_touches() const;

    /**
     * @brief Set touch debug visualization (ripple at each touch)
     *
     * Applied live via RuntimeConfig::set_debug_touches() — no restart needed.
     * Also persists to settings.json so the value survives reboots.
     */
    void set_debug_touches(bool enabled);

    /**
     * @brief Get whether home-screen edit mode (long-press to rearrange) is
     *        allowed. When false, the long-press is suppressed entirely —
     *        the #1245 report was that edit mode triggers by accident with a
     *        resting finger, and this is the hard kill-switch for it.
     * @return true if edit mode is enabled (default)
     */
    bool get_home_edit_mode_enabled() const;

    /**
     * @brief Enable/disable home-screen edit mode entirely.
     *
     * Persists to config and applies LIVE (no restart): should_suppress_edit_mode
     * checks this on every long-press.
     */
    void set_home_edit_mode_enabled(bool enabled);

    /**
     * @brief Check if restart is pending due to settings changes
     * @return true if settings changed that require restart
     */
    bool is_restart_pending() const {
        return restart_pending_;
    }

    /**
     * @brief Clear restart pending flag
     */
    void clear_restart_pending() {
        restart_pending_ = false;
    }

    // =========================================================================
    // SUBJECT ACCESSORS (for XML binding)
    // =========================================================================

    /** @brief Scroll throw subject (integer: 5-50) */
    lv_subject_t* subject_scroll_throw() {
        return &scroll_throw_subject_;
    }

    /** @brief Scroll limit subject (integer: 1-20) */
    lv_subject_t* subject_scroll_limit() {
        return &scroll_limit_subject_;
    }

    /** @brief Long-press time subject (integer ms: 300-1500) */
    lv_subject_t* subject_long_press_time() {
        return &long_press_time_subject_;
    }

    /** @brief Jitter threshold subject (integer: 0-30) */
    lv_subject_t* subject_jitter_threshold() {
        return &jitter_threshold_subject_;
    }

    /** @brief Scroll guard subject (integer: 0 or 1) */
    lv_subject_t* subject_scroll_guard() {
        return &scroll_guard_subject_;
    }

    /** @brief Touch debug subject (integer: 0 or 1) */
    lv_subject_t* subject_debug_touches() {
        return &debug_touches_subject_;
    }

    /** @brief Home edit mode enabled subject (integer: 0 or 1) */
    lv_subject_t* subject_home_edit_mode_enabled() {
        return &home_edit_mode_enabled_subject_;
    }

  private:
    InputSettingsManager();
    ~InputSettingsManager() = default;

    SubjectManager subjects_;

    lv_subject_t scroll_throw_subject_;
    lv_subject_t scroll_limit_subject_;
    lv_subject_t long_press_time_subject_;
    lv_subject_t jitter_threshold_subject_;
    lv_subject_t scroll_guard_subject_;
    lv_subject_t debug_touches_subject_;
    lv_subject_t home_edit_mode_enabled_subject_;

    bool subjects_initialized_ = false;
    bool restart_pending_ = false;
};

} // namespace helix
