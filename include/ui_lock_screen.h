// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>
#include <string>

namespace helix::ui {

/**
 * @brief Full-screen PIN entry overlay for the lock screen feature.
 *
 * Creates a blocking overlay on lv_layer_top() that presents a numeric keypad
 * for PIN verification. Absorbs all touch events except the E-Stop FAB.
 *
 * Usage:
 *   LockScreenOverlay::instance().show();   // Display the lock screen
 *   LockScreenOverlay::instance().hide();   // Dismiss (called after successful unlock)
 *
 * Callbacks (registered in xml_registration.cpp):
 *   lock_digit_clicked, lock_backspace_clicked, lock_confirm_clicked
 */
class LockScreenOverlay {
  public:
    static LockScreenOverlay& instance();

    /** Show the lock screen overlay. No-op if already visible. */
    void show();

    /** Hide and destroy the overlay. No-op if not visible. */
    void hide();

    /** Returns true if the overlay is currently displayed. */
    bool is_visible() const;

    // Called from static XML event callbacks
    void on_digit(int digit);
    void on_backspace();
    void on_confirm();

    // Public so the animation completed callback can call it
    void clear_digits();

  private:
    LockScreenOverlay() = default;

    void create_overlay();
    void destroy_overlay();
    void update_dots();
    void shake_dots();
    void show_error(const char* text = nullptr);
    void hide_error();

    lv_obj_t* overlay_ = nullptr;
    std::string digit_buffer_;

    static constexpr int MAX_DIGITS = 6;
    static constexpr int MIN_DIGITS = 4;
};

/**
 * @brief Register lock screen XML event callbacks with LVGL.
 * Called from xml_registration.cpp during startup.
 */
void register_lock_screen_callbacks();

} // namespace helix::ui
