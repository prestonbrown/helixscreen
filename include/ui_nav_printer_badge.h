// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_printer_switch_menu.h"
#include "ui_widget_ref.h"

#include <functional>
#include <string>

namespace helix::ui {

/// The color of a connection dot for a printer_connection_state value: connected,
/// connecting or reconnecting, and anything else.
lv_color_t connection_dot_color(int connection_state);

/**
 * @brief The navbar's printer badge: its connection dot and the switch menu it opens
 *
 * Application registers the switch and add-printer callbacks during init, so the
 * navbar can trigger them without depending on Application.
 */
class PrinterBadgeMenu {
  public:
    using SwitchCallback = std::function<void(const std::string& printer_id)>;
    using AddCallback = std::function<void()>;

    /// Remember the navbar whose badge the menu opens under. The badge's tap
    /// (on_nav_printer_badge_clicked) and its connection dot are bound in XML.
    void wire(lv_obj_t* navbar);

    /// Open or close the switch menu under the badge.
    void toggle_menu();

    void set_callbacks(SwitchCallback switch_cb, AddCallback add_cb);

    /// Run the registered switch callback; logs when none is registered.
    void trigger_switch(const std::string& printer_id);

    /// Run the registered add-printer callback; logs when none is registered.
    void trigger_add();

    /// Close the menu and drop the callbacks.
    /// They capture Application pointers that are invalid after a soft restart.
    void shutdown();

  private:
    WidgetRef navbar_;
    PrinterSwitchMenu menu_;
    SwitchCallback switch_cb_;
    AddCallback add_cb_;
};

} // namespace helix::ui
