// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"

/**
 * @brief Singleton manager for global keyboard handling
 *
 * Provides a single shared keyboard instance that automatically shows/hides
 * when textareas receive focus. The keyboard features:
 * - Number row always visible (1-0)
 * - Shift key for uppercase/lowercase with iOS-style behavior
 * - ?123/ABC buttons for symbol mode switching
 * - Long-press keys for alternative characters (e.g., hold 'a' for '@')
 * - Backspace positioned above Enter key
 *
 * Usage:
 *   KeyboardManager::instance().init(screen);  // Call once at startup
 *   KeyboardManager::instance().register_textarea(textarea);  // For each textarea
 */
class KeyboardManager {
  public:
    /**
     * @brief Get singleton instance
     * @return Reference to the KeyboardManager singleton
     */
    static KeyboardManager& instance();

    // Non-copyable, non-movable (singleton)
    KeyboardManager(const KeyboardManager&) = delete;
    KeyboardManager& operator=(const KeyboardManager&) = delete;
    KeyboardManager(KeyboardManager&&) = delete;
    KeyboardManager& operator=(KeyboardManager&&) = delete;

    /**
     * @brief Initialize the global keyboard instance
     *
     * Creates a keyboard widget at the bottom of the screen, initially hidden.
     * Should be called once during application initialization.
     *
     * @param parent Parent object (typically lv_screen_active())
     */
    void init(lv_obj_t* parent);

    /**
     * @brief Register a textarea with the keyboard system
     *
     * Adds event handlers to the textarea so the keyboard automatically shows
     * when focused and hides when defocused.
     *
     * @param textarea The textarea widget to register
     */
    void register_textarea(lv_obj_t* textarea);

    /**
     * @brief Register textarea with optional password field marking
     *
     * @param textarea The textarea widget to register
     * @param is_password true if this is a password field
     */
    void register_textarea_ex(lv_obj_t* textarea, bool is_password);

    /**
     * @brief Manually show the keyboard for a specific textarea
     *
     * @param textarea The textarea to assign to the keyboard (NULL to clear)
     */
    void show(lv_obj_t* textarea);

    /**
     * @brief Manually hide the keyboard
     */
    void hide();

    /**
     * @brief Check if the keyboard is currently visible
     * @return true if keyboard is visible, false otherwise
     */
    bool is_visible() const;

    /**
     * @brief Get the global keyboard instance
     * @return Pointer to the keyboard widget, or NULL if not initialized
     */
    lv_obj_t* get_instance() const;

    /**
     * @brief Set the keyboard mode
     * @param mode Keyboard mode (text_lower, text_upper, special, number)
     */
    void set_mode(lv_keyboard_mode_t mode);

    /**
     * @brief Set keyboard position
     * @param align Alignment (e.g., LV_ALIGN_BOTTOM_MID, LV_ALIGN_RIGHT_MID)
     * @param x_ofs X offset from alignment point
     * @param y_ofs Y offset from alignment point
     */
    void set_position(lv_align_t align, int32_t x_ofs, int32_t y_ofs);

  private:
    // Private constructor for singleton
    KeyboardManager() = default;
    ~KeyboardManager() = default;

    // Keyboard mode enum
    enum KeyboardMode {
        MODE_ALPHA_LC,        // Lowercase alphabet
        MODE_ALPHA_UC,        // Uppercase alphabet
        MODE_NUMBERS_SYMBOLS, // Numbers and symbols (?123)
        MODE_ALT_SYMBOLS      // Alternative symbols (#+= mode)
    };

    // Long-press state machine
    enum LongPressState { LP_IDLE, LP_PRESSED, LP_LONG_DETECTED, LP_ALT_SELECTED };

    // Alternative character mapping
    struct AltCharMapping {
        char base_char;
        const char* alternatives;
    };

    // Helper methods
    void apply_keyboard_mode();
    void overlay_cleanup();
    void longpress_reset();
    void show_overlay(const lv_area_t* key_area, const char* alternatives);
    const char* find_alternatives(char base_char) const;
    bool point_in_area(const lv_area_t* area, const lv_point_t* point) const;

    // Event callbacks (static to work with LVGL API)
    static void textarea_focus_event_cb(lv_event_t* e);
    static void textarea_delete_event_cb(lv_event_t* e);
    static void longpress_event_handler(lv_event_t* e);
    static void keyboard_event_cb(lv_event_t* e);
    static void keyboard_draw_alternative_chars(lv_event_t* e);

    // Global keyboard instance
    lv_obj_t* keyboard_ = nullptr;
    lv_obj_t* context_textarea_ = nullptr;

    // Keyboard font with MDI fallback
    lv_font_t keyboard_font_{};
    bool keyboard_font_initialized_ = false;

    // Keyboard state
    KeyboardMode mode_ = MODE_ALPHA_LC;

    // Long-press state tracking
    LongPressState longpress_state_ = LP_IDLE;
    lv_obj_t* overlay_ = nullptr;
    uint32_t pressed_btn_id_ = 0;
    char pressed_char_ = 0;
    const char* alternatives_ = nullptr;
    lv_point_t press_point_{};
    lv_area_t pressed_key_area_{};

    // Shift key behavior tracking (iOS-style)
    bool shift_just_pressed_ = false;
    bool one_shot_shift_ = false;
    bool caps_lock_ = false;

    bool initialized_ = false;

    // When true, long-press auto-inserts the alt character immediately.
    // When false, user must slide finger over the overlay to select.
    bool auto_insert_alt_ = true;

    // Static alternative character mapping table
    static const AltCharMapping alt_char_map_[];
};
