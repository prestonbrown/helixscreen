// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_pin_entry_modal.h
 * @brief Reusable PIN entry modal with numeric keypad for PIN set/change/remove flows.
 *
 * Provides a numeric keypad modal similar to the lock screen, but inside a dialog.
 * The caller supplies a heading string and a completion callback that receives the
 * entered PIN (or empty string on cancel).
 *
 * ## Usage:
 * @code
 * PinEntryModal::show_pin_entry("Enter New PIN", [](const std::string& pin) {
 *     if (pin.empty()) return; // cancelled
 *     LockManager::instance().set_pin(pin);
 * });
 * @endcode
 *
 * @pattern Singleton lifecycle; static XML callbacks route through g_active_modal
 * @threading Main thread only
 */

#pragma once

#include "lvgl/lvgl.h"

#include <functional>
#include <string>

namespace helix::ui {

/**
 * @brief PIN entry modal with numeric keypad.
 *
 * Not a Modal subclass — creates its own backdrop and dialog directly so that
 * it can stack above the security settings overlay without conflicting with
 * the standard Modal backdrop click-to-close behaviour.
 *
 * Only one instance can be active at a time (enforced by g_active_modal).
 */
class PinEntryModal {
  public:
    /// Callback receives entered PIN (or "" on cancel).
    /// Return empty string to accept/dismiss. Return non-empty string to show as
    /// error message and clear the entry for retry.
    using PinCallback = std::function<std::string(const std::string& pin)>;

    /**
     * @brief Show a PIN entry dialog with the given heading.
     *
     * Creates the modal on lv_screen_active() and stores the completion callback.
     * On confirm: callback is invoked with the entered PIN string.
     * On cancel:  callback is invoked with an empty string.
     *
     * @param heading Text displayed below the icon (e.g., "Enter New PIN")
     * @param on_complete Callback receiving the PIN or "" on cancel
     */
    static void show_pin_entry(const std::string& heading, PinCallback on_complete);

    /**
     * @brief Dismiss the active PIN entry modal.
     * Calls the completion callback with an empty string.
     */
    static void dismiss();

    // ========================================================================
    // Called by static XML callbacks — do not call directly
    // ========================================================================
    void on_digit(int digit);
    void on_backspace();
    void on_confirm();
    void on_cancel();

    // ========================================================================
    // Registration (called from xml_registration.cpp)
    // ========================================================================
    static void register_callbacks();

  private:
    PinEntryModal(const std::string& heading, PinCallback on_complete);
    ~PinEntryModal();

    void create();
    void destroy();
    void destroy_async();
    void update_dots();
    void clear_digits();
    void show_error(const char* text = nullptr);
    void hide_error();

    // State
    std::string heading_;
    PinCallback on_complete_;
    std::string digit_buffer_;

    lv_obj_t* backdrop_ = nullptr;
    lv_obj_t* dialog_ = nullptr;

    static constexpr int MIN_DIGITS = 4;
    static constexpr int MAX_DIGITS = 6;

    // Singleton active instance — only one PIN modal at a time
    static PinEntryModal* g_active_modal;

    // Static XML event callbacks (registered globally)
    static void on_digit_clicked(lv_event_t* e);
    static void on_backspace_clicked(lv_event_t* e);
    static void on_confirm_clicked(lv_event_t* e);
    static void on_cancel_clicked(lv_event_t* e);
};

} // namespace helix::ui
