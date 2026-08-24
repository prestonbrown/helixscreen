// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_component_header_bar.h"

#include "ui_utils.h"

#include "display_manager.h"
#include "spdlog/spdlog.h"
#include "theme_manager.h"

#include <algorithm>
#include <stdio.h>
#include <vector>

// ============================================================================
// LIFECYCLE & RESPONSIVE BEHAVIOR
// ============================================================================

// Track all header_bar instances for resize handling
static std::vector<lv_obj_t*> header_instances;

// Event handler for DELETE event (cleanup)
static void header_bar_delete_cb(lv_event_t* e) {
    lv_obj_t* header = (lv_obj_t*)lv_event_get_target(e);

    // Remove from tracking list
    auto it = std::find(header_instances.begin(), header_instances.end(), header);
    if (it != header_instances.end()) {
        header_instances.erase(it);
        spdlog::debug("[HeaderBar] Removed from tracking ({} remain)", header_instances.size());
    }
}

// Global resize callback (called by app resize handler system)
static void on_app_resize() {
    for (lv_obj_t* header : header_instances) {
        if (!header)
            continue;

        lv_obj_t* screen = lv_obj_get_screen(header);
        if (!screen)
            continue;

        lv_coord_t header_height = ui_get_responsive_header_height(lv_obj_get_height(screen));
        lv_obj_set_height(header, header_height);
    }

    if (!header_instances.empty()) {
        spdlog::debug("[HeaderBar] Updated {} header heights on resize", header_instances.size());
    }
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void ui_component_header_bar_init() {
    // Register global resize callback for all header_bar instances
    if (auto* dm = DisplayManager::instance()) {
        dm->register_resize_callback(on_app_resize);
    }

    spdlog::trace("[HeaderBar] Component system initialized");
}

void ui_component_header_bar_setup(lv_obj_t* header, lv_obj_t* screen) {
    if (!header || !screen) {
        return;
    }

    // Attach DELETE event for cleanup
    lv_obj_add_event_cb(header, header_bar_delete_cb, LV_EVENT_DELETE, nullptr);

    // Track this instance for global resize handling
    header_instances.push_back(header);

    // Apply responsive height immediately
    lv_coord_t header_height = ui_get_responsive_header_height(lv_obj_get_height(screen));
    lv_obj_set_height(header, header_height);

    // Extend back button's click area for easier touch targeting.
    // Use half the header height so the hit area spans the full header vertically
    // and is generously wide (~header_height total width beyond the icon).
    lv_obj_t* back_btn = lv_obj_find_by_name(header, "back_button");
    if (back_btn) {
        lv_obj_set_ext_click_area(back_btn, header_height / 2);

        // Apply pressed style so back button has visual touch feedback
        static lv_style_t back_pressed;
        static bool inited = false;
        if (!inited) {
            lv_style_init(&back_pressed);
            lv_style_set_opa(&back_pressed, LV_OPA_50);
            inited = true;
        }
        lv_obj_add_style(back_btn, &back_pressed, LV_PART_MAIN | LV_STATE_PRESSED);

        // NOTE: this function is currently unreachable — its only caller,
        // ui_panel_setup_header() (ui_panel_common.cpp), has no callers of its
        // own. Every real overlay panel goes through
        // ui_overlay_panel_setup_standard() instead (directly, or via
        // OverlayBase), which is where the portrait back-icon swap
        // (chevron_up) actually lives. Don't "fix" the icon here and wonder
        // why nothing changes at runtime.
    }

    spdlog::trace("[HeaderBar] Setup complete: height={}px", header_height);
}
