// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "lvgl.h"

#include <string>
#include <unordered_map>

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
     * @brief Undo register_textarea() for one widget.
     *
     * <text_input> registers every textarea it creates, so a field that takes its
     * value some other way - a numeric keypad, a picker - has to opt back out or the
     * tap raises the software keyboard underneath whatever it opened. Removes the
     * focus hooks and drops the widget from the input group; the delete hook stays,
     * so the manager still forgets the widget when it dies.
     *
     * @param textarea The textarea to stop managing. Safe on null and on a widget
     *                 that was never registered.
     */
    void unregister_textarea(lv_obj_t* textarea);

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
     * @brief Whether the textarea the keyboard is driving is masked
     *
     * Password fields must never contribute their content to a log line.
     * tail_ring_buffer() ships verbatim as the debug bundle's log_tail, and the
     * ring retains DEBUG whatever verbosity the user configured, so a logged
     * keypress leaves the device inside an artifact shared in public channels.
     *
     * Reads the widget's live state rather than a flag latched at registration:
     * a "show password" control can toggle it afterwards, and register_textarea()
     * runs before XML attributes have been applied.
     *
     * @return true if the active textarea is in password mode
     */
    bool is_password_context() const;

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

    /**
     * @brief Reset keyboard state for soft restart
     *
     * Deletes keyboard_ and overlay_ widgets (children of m_screen that survive
     * app_layout teardown), then resets all state so init() can be called again.
     *
     * Call from Application::tear_down_printer_state() before rebuilding UI.
     */
    void reset();

    /**
     * @brief Place an alternate-character hint glyph inside a key.
     *
     * Anchors the hint to the key's top-right corner with an inset proportional to
     * the glyph height, falling back to a 1px inset on keys too tight for that.
     *
     * The returned rect is guaranteed to lie fully inside @p btn, so callers draw
     * it unclipped and a hint can never bleed onto a neighbouring key. Key sizes
     * vary widely — a 2-unit `?123` against a 12-unit spacebar, across every
     * breakpoint from micro panels to 1024px displays — so containment is checked
     * rather than assumed.
     *
     * Static and free of widget state so the containment invariant is testable
     * without building a keyboard.
     *
     * @param btn     Key rect
     * @param hint_w  Hint glyph width
     * @param hint_h  Hint glyph height
     * @param out     Receives the hint rect; untouched when this returns false
     * @return false when the glyph cannot fit inside the key at any supported
     *         inset — draw nothing in that case
     */
    [[nodiscard]] static bool compute_hint_area(const lv_area_t& btn, int32_t hint_w,
                                                int32_t hint_h, lv_area_t* out);

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

    /**
     * @brief Re-derive every theme-dependent key style onto the keyboard.
     *
     * Called from init() and again whenever the theme changes. Sets each
     * property unconditionally rather than skipping the ones the current depth
     * does not use, so a re-apply clears the previous treatment instead of
     * leaving it behind.
     */
    void apply_key_styles();
    void overlay_cleanup();
    void longpress_reset();
    void show_overlay(const lv_area_t* key_area, const char* alternatives);
    bool point_in_area(const lv_area_t* area, const lv_point_t* point) const;

  public:
    /**
     * @brief Alternate characters reachable by long-pressing @p base_char.
     *
     * Returns the session's current order for that key — element 0 is what a plain
     * long-press yields and what the on-key hint draws. nullptr when the key has no
     * alternates.
     *
     * @warning The returned pointer is invalidated by promote_alternative() for the
     *          same key, which rewrites the backing string in place.
     */
    const char* find_alternatives(char base_char) const;

    /**
     * @brief Make @p chosen the default alternate for @p base_char.
     *
     * Moves the character to the front of that key's alternate list, so the next
     * plain long-press yields it and the on-key hint — which draws element 0 —
     * updates to match. Session-lifetime only; nothing is persisted, so the
     * shipped order is restored on restart.
     *
     * No-op when the key has no alternates, or when @p chosen is already the
     * default. Only meaningful for keys with more than one alternate.
     */
    void promote_alternative(char base_char, char chosen);

  private:
    /**
     * @brief The character to put in a log line for @p c.
     *
     * Returns @p c normally and '*' while the active textarea is masked. Every
     * log site that would otherwise name a typed character routes through this,
     * so keyboard debugging stays useful on ordinary fields without a passphrase
     * ever reaching the log. See is_password_context().
     */
    char log_char(char c) const;

    // Event callbacks (static to work with LVGL API)
    static void textarea_focus_event_cb(lv_event_t* e);
    static void textarea_delete_event_cb(lv_event_t* e);
    static void longpress_event_handler(lv_event_t* e);
    static void keyboard_event_cb(lv_event_t* e);
    static void keyboard_draw_alternative_chars(lv_event_t* e);

    // Re-applies key styles on theme change. The keyboard is built once at
    // startup and never rebuilt, so without this the keys keep the palette they
    // were created with.
    ObserverGuard theme_observer_;

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

    /// Per-key reordering of alt_char_map_ entries, seeded lazily on first promotion.
    /// Absent key = shipped order. Node-based so a string's address is stable across
    /// rehash, but a promoted string is rewritten in place — never hold a c_str()
    /// from find_alternatives() across a promote_alternative() call for that key.
    std::unordered_map<char, std::string> alt_order_;
};
