// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_printer_switch_menu.h"

#include "config.h"
#include "theme_manager.h"
#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_icon_codepoints.h"
#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// Static member initialization
PrinterSwitchMenu* PrinterSwitchMenu::s_active_instance_ = nullptr;
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

    s_active_instance_ = this;
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
    if (!cfg) {
        spdlog::warn("[PrinterSwitchMenu] No config instance");
        return;
    }

    auto printer_ids = cfg->get_printer_ids();
    auto active_id = cfg->get_active_printer_id();

    lv_obj_t* printer_list = lv_obj_find_by_name(menu(), "printer_list");
    if (!printer_list) {
        spdlog::error("[PrinterSwitchMenu] printer_list not found in XML");
        return;
    }

    // Cap list height at 2/3 screen height
    lv_obj_t* screen = lv_obj_get_screen(menu());
    int screen_h = lv_obj_get_height(screen);
    lv_obj_set_style_max_height(printer_list, screen_h * 2 / 3, 0);

    // Resolve spacing tokens
    auto get_token = [](const char* name, int fallback) {
        const char* s = lv_xml_get_const(nullptr, name);
        return s ? std::atoi(s) : fallback;
    };
    int space_xs = get_token("space_xs", 4);
    int space_sm = get_token("space_sm", 6);

    lv_color_t accent = theme_manager_get_color("accent");
    lv_color_t text_color = theme_manager_get_color("text");

    // Resolve fonts via XML token system (same pattern as other context menus)
    const char* body_font_name = lv_xml_get_const(nullptr, "font_body");
    const lv_font_t* body_font =
        body_font_name ? lv_xml_get_font(nullptr, body_font_name) : lv_font_get_default();
    const char* icon_font_name = lv_xml_get_const(nullptr, "icon_font_xs");
    const lv_font_t* icon_font =
        icon_font_name ? lv_xml_get_font(nullptr, icon_font_name) : body_font;

    // Get check icon codepoint from our icon system
    const char* check_codepoint = ui_icon::lookup_codepoint("check");

    for (const auto& id : printer_ids) {
        bool is_active = (id == active_id);
        std::string name = cfg->get<std::string>("/printers/" + id + "/printer_name",
                                                  id);

        // Row container
        lv_obj_t* row = lv_obj_create(printer_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, space_sm, 0);
        lv_obj_set_style_pad_gap(row, space_xs, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        // Pressed state styling
        lv_obj_set_style_bg_opa(row, 30, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(row, accent, LV_STATE_PRESSED);

        // Checkmark icon (active) or spacer (inactive) — fixed width for alignment
        lv_obj_t* indicator = lv_label_create(row);
        lv_obj_set_style_text_font(indicator, icon_font, 0);
        lv_obj_set_style_min_width(indicator, 16, 0);
        if (is_active && check_codepoint) {
            lv_label_set_text(indicator, check_codepoint);
            lv_obj_set_style_text_color(indicator, text_color, 0);
        } else {
            lv_label_set_text(indicator, "");
        }
        lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(indicator, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Printer name label
        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, name.c_str());
        lv_obj_set_flex_grow(label, 1);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(label, body_font, 0);
        lv_obj_set_style_text_color(label, text_color, 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Store printer ID for click handler
        lv_obj_set_name(row, id.c_str());

        lv_obj_add_event_cb(row, on_printer_row_cb, LV_EVENT_CLICKED, nullptr);
    }

    spdlog::debug("[PrinterSwitchMenu] Populated {} printers (active: {})",
                  printer_ids.size(), active_id);
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

void PrinterSwitchMenu::dispatch_switch_action(MenuAction action,
                                                const std::string& printer_id) {
    auto callback = switch_callback_;
    s_active_instance_ = nullptr;
    hide();  // Safe: uses lv_obj_delete_async internally

    if (callback) {
        helix::ui::queue_update([callback, action, printer_id]() {
            callback(action, printer_id);
        });
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
        {"printer_switch_backdrop_cb", on_backdrop_cb},
        {"printer_switch_add_cb", on_add_printer_cb},
    });

    s_callbacks_registered_ = true;
    spdlog::debug("[PrinterSwitchMenu] Callbacks registered");
}

// ============================================================================
// Static callbacks
// ============================================================================

PrinterSwitchMenu* PrinterSwitchMenu::get_active_instance() {
    if (!s_active_instance_) {
        spdlog::warn("[PrinterSwitchMenu] No active instance for event");
    }
    return s_active_instance_;
}

void PrinterSwitchMenu::on_backdrop_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->on_backdrop_clicked();
    }
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

}  // namespace helix::ui
