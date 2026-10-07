// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
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

    /// Attach the badge tap and the connection dot found in @p navbar.
    void wire(lv_obj_t* navbar);

    void set_callbacks(SwitchCallback switch_cb, AddCallback add_cb);

    /// Run the registered switch callback; logs when none is registered.
    void trigger_switch(const std::string& printer_id);

    /// Run the registered add-printer callback; logs when none is registered.
    void trigger_add();

    /// Close the menu, stop following the connection state and drop the callbacks.
    /// They capture Application pointers that are invalid after a soft restart.
    void shutdown();

  private:
    static void badge_clicked_cb(lv_event_t* e);
    void toggle_menu();

    WidgetRef navbar_;
    WidgetRef dot_;
    ObserverGuard dot_observer_;
    PrinterSwitchMenu menu_;
    SwitchCallback switch_cb_;
    AddCallback add_cb_;
};

} // namespace helix::ui
