// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_printer_switch_menu.h"

#include "ui_callback_helpers.h"
#include "ui_change_host_modal.h"
#include "ui_event_safety.h"
#include "ui_icon_codepoints.h"
#include "ui_row_text.h"
#include "ui_update_queue.h"

#include "config.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// Static member initialization
bool PrinterSwitchMenu::s_callbacks_registered_ = false;

// ============================================================================
// Public API
// ============================================================================

void PrinterSwitchMenu::show(lv_obj_t* parent, lv_obj_t* near_widget) {
    register_callbacks();

    // Set click point to right edge center of the badge widget
    lv_point_t pt;
    pt.x = lv_obj_get_x(near_widget) + lv_obj_get_width(near_widget);
    pt.y = lv_obj_get_y(near_widget) + lv_obj_get_height(near_widget) / 2;
    set_click_point(pt);

    // show_near_widget() also claims the active-menu slot the static callbacks
    // resolve through.
    show_near_widget(parent, 0, near_widget);

    spdlog::debug("[PrinterSwitchMenu] Shown");
}

// ============================================================================
// ContextMenu overrides
// ============================================================================

void PrinterSwitchMenu::on_created(lv_obj_t* /*menu_obj*/) {
    populate_printer_list();
}

void PrinterSwitchMenu::on_backdrop_clicked() {
    dispatch_switch_action(MenuAction::CANCELLED);
}

// ============================================================================
// Printer list population
// ============================================================================

void PrinterSwitchMenu::populate_printer_list() {
    auto* cfg = Config::get_instance();

    auto printer_ids = cfg->get_printer_ids();
    auto active_id = cfg->get_active_printer_id();

    lv_obj_t* printer_list = helix::ui::find_required(menu(), "printer_list", "PrinterSwitchMenu");
    if (!printer_list) {
        return;
    }

    // Cap list height at 2/3 screen height
    lv_obj_t* screen = lv_obj_get_screen(menu());
    int screen_h = lv_obj_get_height(screen);
    lv_obj_set_style_max_height(printer_list, screen_h * 2 / 3, 0);

    // Get check icon codepoint from our icon system
    const char* check_codepoint = helix::ui::icon::lookup_codepoint("check");

    for (const auto& id : printer_ids) {
        bool is_active = (id == active_id);
        std::string name = cfg->get<std::string>("/printers/" + id + "/printer_name", id);

        const char* attrs[] = {
            "check_text",
            (is_active && check_codepoint) ? check_codepoint : "",
            nullptr,
        };
        auto* row =
            static_cast<lv_obj_t*>(lv_xml_create(printer_list, "printer_switch_row", attrs));
        if (!row) {
            continue;
        }
        helix::ui::set_row_label_text(row, "printer_name", name.c_str());

        // Store printer ID for click handler
        lv_obj_set_name(row, id.c_str());

        lv_obj_add_event_cb(row, on_printer_row_cb, LV_EVENT_CLICKED, nullptr);
    }

    spdlog::debug("[PrinterSwitchMenu] Populated {} printers (active: {})", printer_ids.size(),
                  active_id);
}

// ============================================================================
// Action handlers
// ============================================================================

void PrinterSwitchMenu::handle_printer_selected(const std::string& printer_id) {
    spdlog::info("[PrinterSwitchMenu] Printer selected: {}", printer_id);
    dispatch_switch_action(MenuAction::SWITCH, printer_id);
}

void PrinterSwitchMenu::handle_add_printer() {
    spdlog::info("[PrinterSwitchMenu] Add printer requested");
    dispatch_switch_action(MenuAction::ADD_PRINTER);
}

void PrinterSwitchMenu::dispatch_switch_action(MenuAction action, const std::string& printer_id) {
    // The switch below is queued, so it has not run when the menu closes.
    if (action != MenuAction::CANCELLED) {
        drop_held_connection_failed();
    }
    auto callback = switch_callback_;
    hide(); // Safe: uses lv_obj_delete_async internally

    if (callback) {
        helix::ui::queue_update("PrinterSwitchMenu::dispatch_switch_action",
                                [callback, action, printer_id]() { callback(action, printer_id); });
    }
}

// ============================================================================
// Static callback registration
// ============================================================================

void PrinterSwitchMenu::register_callbacks() {
    if (s_callbacks_registered_) {
        return;
    }

    register_xml_callbacks({
        {"printer_switch_add_cb", on_add_printer_cb},
    });

    s_callbacks_registered_ = true;
    spdlog::debug("[PrinterSwitchMenu] Callbacks registered");
}

// ============================================================================
// Static callbacks
// ============================================================================

PrinterSwitchMenu* PrinterSwitchMenu::get_active_instance() {
    auto* self = ContextMenu::active_as<PrinterSwitchMenu>();
    if (!self) {
        spdlog::warn("[PrinterSwitchMenu] No active instance for event");
    }
    return self;
}

void PrinterSwitchMenu::on_add_printer_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_add_printer();
    }
}

void PrinterSwitchMenu::on_printer_row_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrinterSwitchMenu] on_printer_row_cb");

    auto* self = get_active_instance();
    if (!self) {
        return;
    }

    // Walk up parent chain to find the row with a name (printer ID)
    // (click target may be a child label due to event bubbling)
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const char* name = nullptr;
    lv_obj_t* obj = target;
    while (obj) {
        name = lv_obj_get_name(obj);
        if (name && name[0] != '\0') {
            break;
        }
        obj = lv_obj_get_parent(obj);
    }

    if (!name || name[0] == '\0') {
        spdlog::warn("[PrinterSwitchMenu] Row click with no printer ID");
        return;
    }

    std::string selected_id(name);
    self->handle_printer_selected(selected_id);

    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui
