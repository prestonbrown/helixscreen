// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include <lvgl.h>

namespace helix::ui {

/**
 * @brief A button kept over one of the navbar's slots
 *
 * It lives on the screen, not inside the navbar, so it stays above every overlay
 * and keypad backdrop. An open modal covers it, except the E-stop's own
 * confirmation; an open keyboard over its slot covers it unless it rides above
 * the keyboard. It is registered as always-on-top screen chrome, which is what
 * keeps stale-overlay sweeps from hiding it. The E-stop and the
 * spools-on-the-bed button are the two.
 */
class NavbarButton {
  public:
    /// @param slot_name      The navbar placeholder the button is kept over
    /// @param component      The XML component that builds the button
    /// @param widget_name    The name the built button carries
    /// @param lifts_over_keyboard Beside a side navbar, ride up the navbar column
    ///        above an open keyboard instead of being covered by it
    NavbarButton(const char* slot_name, const char* component, const char* widget_name,
                 bool lifts_over_keyboard)
        : slot_name_(slot_name), component_(component), widget_name_(widget_name),
          lifts_over_keyboard_(lifts_over_keyboard) {}

    /// Build the button over @p navbar's slot. No slot, no button.
    void create(lv_obj_t* navbar);

    /// Move the button onto the slot's current position.
    void sync();

    /// The keyboard's top edge in screen coordinates while it is open, or -1
    /// once it closes. Beside a side navbar a lifting button moves up the navbar
    /// column to clear it and back to its slot after; under a portrait bottom
    /// bar the keyboard covers it.
    void set_keyboard_top(int32_t top);

    /// Delete the button. The next create() builds a fresh one over the new navbar.
    void destroy();

    /// The button, or nullptr.
    [[nodiscard]] lv_obj_t* widget() const {
        return button_;
    }

  private:
    /// Beside a side navbar with the keyboard open, a lifting button sits in
    /// the navbar column above the keyboard rather than under it.
    [[nodiscard]] bool rides_above_keyboard() const;
    /// Bring the button to the front, but under an open keyboard it does not ride above.
    void raise();

    static void button_deleted_cb(lv_event_t* e);
    static void navbar_layout_changed_cb(lv_event_t* e);

    const char* slot_name_;
    const char* component_;
    const char* widget_name_;
    bool lifts_over_keyboard_;
    helix::ui::WidgetRef button_;
    WidgetRef navbar_;
    int32_t keyboard_top_ = -1;
};

} // namespace helix::ui
