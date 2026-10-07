// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_nav_printer_badge.h"

#include "app_globals.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

lv_color_t connection_dot_color(int connection_state) {
    switch (connection_state) {
    case 2: // connected
        return theme_manager_get_color("success");
    case 1: // connecting
    case 3: // reconnecting
        return theme_manager_get_color("warning");
    default: // disconnected, failed
        return theme_manager_get_color("danger");
    }
}

void PrinterBadgeMenu::wire(lv_obj_t* navbar) {
    navbar_ = navbar;

    lv_obj_t* printer_badge = lv_obj_find_by_name(navbar, "nav_printer_badge");
    if (printer_badge) {
        lv_obj_add_event_cb(printer_badge, badge_clicked_cb, LV_EVENT_CLICKED, this);
    }

    // Connection status dot — color reflects WebSocket connection state
    dot_ = lv_obj_find_by_name(navbar, "nav_printer_dot");
    if (dot_) {
        dot_observer_ = observe<int>(
            get_printer_state().network_state().get_printer_connection_state_subject(), this,
            [](PrinterBadgeMenu* self, int state) {
                if (!self->dot_)
                    return;
                lv_obj_set_style_bg_color(self->dot_, connection_dot_color(state), 0);
            },
            get_printer_state().get_subjects_lifetime());
    }
}

void PrinterBadgeMenu::set_callbacks(SwitchCallback switch_cb, AddCallback add_cb) {
    switch_cb_ = std::move(switch_cb);
    add_cb_ = std::move(add_cb);
}

void PrinterBadgeMenu::trigger_switch(const std::string& printer_id) {
    if (switch_cb_) {
        switch_cb_(printer_id);
    } else {
        spdlog::warn("[NavigationManager] No printer switch callback registered");
    }
}

void PrinterBadgeMenu::trigger_add() {
    if (add_cb_) {
        add_cb_();
    } else {
        spdlog::warn("[NavigationManager] No add printer callback registered");
    }
}

void PrinterBadgeMenu::shutdown() {
    // The menu's widget is a child of the screen.
    menu_.hide();

    dot_observer_.reset();
    dot_ = nullptr;

    switch_cb_ = nullptr;
    add_cb_ = nullptr;
}

void PrinterBadgeMenu::badge_clicked_cb(lv_event_t* e) {
    static_cast<PrinterBadgeMenu*>(lv_event_get_user_data(e))->toggle_menu();
}

void PrinterBadgeMenu::toggle_menu() {
    if (menu_.is_visible()) {
        menu_.hide();
        return;
    }
    if (!navbar_) {
        return;
    }

    lv_obj_t* badge = lv_obj_find_by_name(navbar_, "nav_printer_badge");
    if (!badge)
        return;

    lv_obj_t* screen = lv_obj_get_screen(navbar_);

    menu_.set_switch_callback(
        [this](PrinterSwitchMenu::MenuAction action, const std::string& printer_id) {
            switch (action) {
            case PrinterSwitchMenu::MenuAction::SWITCH:
                spdlog::info("[Nav] Switching to printer '{}'", printer_id);
                if (switch_cb_) {
                    switch_cb_(printer_id);
                }
                break;
            case PrinterSwitchMenu::MenuAction::ADD_PRINTER:
                spdlog::info("[Nav] Adding new printer via wizard");
                if (add_cb_) {
                    add_cb_();
                }
                break;
            case PrinterSwitchMenu::MenuAction::CANCELLED:
                break;
            }
        });

    menu_.show(screen, badge);
}

} // namespace helix::ui
