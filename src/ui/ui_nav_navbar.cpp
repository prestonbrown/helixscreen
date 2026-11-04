// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_event_safety.h"
#include "ui_nav_manager.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

using namespace helix;
using helix::ui::observe;

lv_obj_t* NavigationManager::navbar_target_at(lv_obj_t* navbar, const lv_point_t& point) {
    if (!navbar) {
        return nullptr;
    }
    static constexpr const char* kTargets[] = {"nav_btn_home",     "nav_btn_print_select",
                                               "nav_btn_controls", "nav_btn_filament",
                                               "nav_btn_settings", "nav_btn_advanced"};
    for (const char* name : kTargets) {
        lv_obj_t* btn = lv_obj_find_by_name(navbar, name);
        if (!btn || lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_area_t area;
        lv_obj_get_coords(btn, &area);
        if (point.x >= area.x1 && point.x <= area.x2 && point.y >= area.y1 && point.y <= area.y2) {
            return btn;
        }
    }
    return nullptr;
}

void NavigationManager::nav_button_clicked_cb(lv_event_t* event) {
    LVGL_SAFE_EVENT_CB_BEGIN("nav_button_clicked_cb");

    auto& mgr = NavigationManager::instance();
    lv_event_code_t code = lv_event_get_code(event);
    int panel_id = (int)(uintptr_t)lv_event_get_user_data(event);

    spdlog::trace("[NavigationManager] nav_button_clicked_cb fired: code={}, panel_id={}, "
                  "active_panel={}",
                  static_cast<int>(code), panel_id, static_cast<int>(mgr.active_panel_));

    if (code == LV_EVENT_CLICKED) {
        // Queued, not inline: this runs from an LVGL event during the render
        // phase, where mutating the widget tree corrupts the draw.
        mgr.request_panel(static_cast<PanelId>(panel_id), SwitchDispatch::Queued);
    }

    LVGL_SAFE_EVENT_CB_END();
}

void NavigationManager::wire_events(lv_obj_t* navbar) {
    if (!navbar) {
        spdlog::error("[NavigationManager] NULL navbar provided to wire_events");
        return;
    }

    if (!subjects_initialized_) {
        spdlog::error("[NavigationManager] Subjects not initialized! Call init() first!");
        return;
    }

    // Store navbar reference for z-order management when showing overlays
    navbar_widget_ = navbar;

    lv_obj_remove_flag(navbar, LV_OBJ_FLAG_CLICKABLE);

    const char* button_names[] = {"nav_btn_home",     "nav_btn_print_select", "nav_btn_controls",
                                  "nav_btn_filament", "nav_btn_settings",     "nav_btn_advanced"};

    for (int i = 0; i < kUiPanelCount; i++) {
        lv_obj_t* btn = lv_obj_find_by_name(navbar, button_names[i]);

        if (!btn) {
            spdlog::trace("[NavigationManager] Nav button {} not found (may be intentional)", i);
            continue;
        }

        lv_obj_add_event_cb(btn, nav_button_clicked_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

        // Remove focus ring — nav buttons use icon color swap for active state
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_group_remove_obj(btn);
    }

    // Register connection state observer for redirect on disconnect
    connection_state_observer_ = observe<int>(
        get_printer_state().network_state().get_printer_connection_state_subject(), this,
        [](NavigationManager* mgr, int value) { mgr->handle_connection_state_change(value); },
        get_printer_state().get_subjects_lifetime());

    // Register klippy state observer for redirect on SHUTDOWN/ERROR
    klippy_state_observer_ = observe<int>(
        get_printer_state().network_state().get_klippy_state_subject(), this,
        [](NavigationManager* mgr, int value) { mgr->handle_klippy_state_change(value); },
        get_printer_state().get_subjects_lifetime());

    printer_badge_.wire(navbar);

    // The printer badge is the one navbar element a setting can add or remove
    // while the user is looking at it, and its toggle lives inside an overlay —
    // so the navbar the user sees is the backdrop's frozen snapshot, not the
    // widget the binding just un-hid. Re-take the snapshot so the change lands
    // immediately instead of waiting for the stack to pop.
    printer_switcher_observer_ = observe<int>(
        SettingsManager::instance().subject_show_printer_switcher(), this,
        [](NavigationManager* mgr, int /* shown */) { mgr->refresh_overlay_backdrop(); },
        SettingsManager::instance().get_subjects_lifetime());

    // A live theme or mode switch from inside an overlay (Settings > Appearance)
    // repaints the navbar widget the snapshot hides, so re-take it too. The
    // change subject fires after the repaint, so the new shot shows the new mode.
    // It also fires on subscription and on re-applies of the same palette, which
    // change no pixel.
    theme_observer_ = observe<int>(
        theme_manager_get_changed_subject(), this,
        [](NavigationManager* mgr, int /* generation */) {
            if (mgr->backdrop_.palette_changed())
                mgr->refresh_overlay_backdrop();
        },
        subject_never_freed());

    navbar_estop_.create(navbar);
    navbar_drying_.create(navbar);

    spdlog::trace(
        "[NavigationManager] Navigation button events wired (with connection/klippy gating)");
}

void NavigationManager::wire_status_icons(lv_obj_t* navbar) {
    if (!navbar) {
        spdlog::error("[NavigationManager] NULL navbar provided to wire_status_icons");
        return;
    }

    const char* button_names[] = {"status_btn_printer", "status_btn_network",
                                  "status_notification_icon"};
    const char* icon_names[] = {"status_printer_icon", "status_network_icon",
                                "status_notification_icon"};
    const int status_icon_count = 3;

    for (int i = 0; i < status_icon_count; i++) {
        lv_obj_t* btn = lv_obj_find_by_name(navbar, button_names[i]);
        lv_obj_t* icon_widget = lv_obj_find_by_name(navbar, icon_names[i]);

        if (!btn || !icon_widget) {
            spdlog::warn("[NavigationManager] Status icon {}: btn={}, icon={} (may not exist yet)",
                         button_names[i], (void*)btn, (void*)icon_widget);
            continue;
        }

        lv_obj_add_flag(icon_widget, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(icon_widget, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        spdlog::trace("[NavigationManager] Status icon {} wired", button_names[i]);
    }
}
