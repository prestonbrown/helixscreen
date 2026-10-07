// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "input_defaults.h"
#include "lvgl/lvgl.h"
#include "persisted_setting.h"
#include "subject_managed_panel.h"

namespace helix {

/// Numeric keypad digit order. Phone puts 1-2-3 on top, calculator 7-8-9.
enum class KeypadLayout { PHONE = 0, CALCULATOR = 1 };

/**
 * @brief Domain-specific manager for input/scroll settings
 *
 * Owns all input-related LVGL subjects and persistence:
 * - scroll_throw (momentum decay rate, 5-50)        — live-applied
 * - scroll_limit (pixels before scrolling starts, 1-20) — live-applied
 * - scroll_guard (suppress phantom click after scroll)  — restart required
 * - debug_touches (draw ripple at each touch point)     — live-applied
 * - keypad_layout (numeric keypad digit order)          — live-applied
 *
 * Thread safety: Single-threaded, main LVGL thread only.
 */
class InputSettingsManager {
  public:
    static InputSettingsManager& instance();

    /// Default scroll momentum decay; see helix::input_defaults::SCROLL_THROW.
    static constexpr int DEFAULT_SCROLL_THROW = helix::input_defaults::SCROLL_THROW;

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

    /** @brief Momentum decay rate (5-50, higher = faster decay), applied live to every pointer. */
    int get_scroll_throw() const {
        return settings_.get(Key::ScrollThrow);
    }
    void set_scroll_throw(int value);

    /** @brief Pixels before scrolling starts (1-20), applied live to every pointer. */
    int get_scroll_limit() const {
        return settings_.get(Key::ScrollLimit);
    }
    void set_scroll_limit(int value);

    /**
     * @brief Long-press hold time in ms (300-1500, default 500), applied live to
     *        every pointer indev. Governs every long-press in the app, not just
     *        home edit mode (#1245).
     */
    int get_long_press_time() const {
        return settings_.get(Key::LongPressTime);
    }
    void set_long_press_time(int value);

    /** @brief Suppress phantom clicks after scrolling. Restart required. */
    bool get_scroll_guard() const {
        return settings_.get_bool(Key::ScrollGuard);
    }
    void set_scroll_guard(bool enabled);

    /** @brief Ripple at each touch point, applied live via RuntimeConfig. */
    bool get_debug_touches() const {
        return settings_.get_bool(Key::DebugTouches);
    }
    void set_debug_touches(bool enabled);

    /**
     * @brief Whether the long-press into home-screen edit mode is allowed.
     *        Checked live by should_suppress_edit_mode (#1245).
     */
    bool get_home_edit_mode_enabled() const {
        return settings_.get_bool(Key::HomeEditMode);
    }
    void set_home_edit_mode_enabled(bool enabled) {
        settings_.set(Key::HomeEditMode, enabled);
    }

    /** @brief Numeric keypad digit order; the keypad XML binds it live. */
    KeypadLayout get_keypad_layout() const {
        return static_cast<KeypadLayout>(settings_.get(Key::KeypadLayout));
    }
    void set_keypad_layout(KeypadLayout layout) {
        settings_.set(Key::KeypadLayout, static_cast<int>(layout));
    }

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
        return settings_.subject(Key::ScrollThrow);
    }

    /** @brief Scroll limit subject (integer: 1-20) */
    lv_subject_t* subject_scroll_limit() {
        return settings_.subject(Key::ScrollLimit);
    }

    /** @brief Long-press time subject (integer ms: 300-1500) */
    lv_subject_t* subject_long_press_time() {
        return settings_.subject(Key::LongPressTime);
    }

    /** @brief Scroll guard subject (integer: 0 or 1) */
    lv_subject_t* subject_scroll_guard() {
        return settings_.subject(Key::ScrollGuard);
    }

    /** @brief Touch debug subject (integer: 0 or 1) */
    lv_subject_t* subject_debug_touches() {
        return settings_.subject(Key::DebugTouches);
    }

    /** @brief Home edit mode enabled subject (integer: 0 or 1) */
    lv_subject_t* subject_home_edit_mode_enabled() {
        return settings_.subject(Key::HomeEditMode);
    }

    /** @brief Keypad layout subject (integer: KeypadLayout) */
    lv_subject_t* subject_keypad_layout() {
        return settings_.subject(Key::KeypadLayout);
    }

  private:
    InputSettingsManager();
    ~InputSettingsManager() = default;

    void apply_to_pointers() const;

    enum class Key : uint8_t {
        ScrollThrow,
        ScrollLimit,
        LongPressTime,
        ScrollGuard,
        DebugTouches,
        HomeEditMode,
        KeypadLayout,
        COUNT
    };

    SubjectManager subjects_;
    settings::PersistedSettings<Key, static_cast<size_t>(Key::COUNT)> settings_;

    bool subjects_initialized_ = false;
    bool restart_pending_ = false;
};

} // namespace helix
