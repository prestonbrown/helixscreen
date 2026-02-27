// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_ams_current_tool.cpp
 * @brief C++ backing for the ams_current_tool XML component
 *
 * Handles:
 * - Color swatch updates (observing ams_current_color subject)
 * - Click handler (opens AMS panel)
 * - Cleanup on widget deletion
 */

#include "ui_nav_manager.h"
#include "ui_observer_guard.h"
#include "ui_panel_ams.h"
#include "ui_panel_ams_overview.h"

#include "ams_state.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "observer_factory.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <unordered_map>

namespace {

// Per-widget data for ams_current_tool instances
struct AmsCurrentToolData {
    lv_obj_t* color_swatch = nullptr;
    ObserverGuard color_observer;
};

// Registry for cleanup
static std::unordered_map<lv_obj_t*, std::unique_ptr<AmsCurrentToolData>> s_registry;

// on_color_changed migrated to lambda observer in on_widget_created()

// Cleanup callback when widget is deleted
static void on_delete(lv_event_t* e) {
    lv_obj_t* widget = lv_event_get_target_obj(e);
    auto it = s_registry.find(widget);
    if (it != s_registry.end()) {
        // Release observer before deleting data
        it->second->color_observer.release();
        s_registry.erase(it);
        spdlog::debug("[AmsCurrentTool] Widget cleaned up");
    }
}

// Click callback - opens AMS panel (overview for multi-unit, detail for single)
static void on_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[AmsCurrentTool] Clicked - opening AMS panel");
    navigate_to_ams_panel();
}

// Post-create hook called after XML creates the widget
static void on_widget_created(lv_obj_t* widget) {
    if (!widget)
        return;

    // Create per-widget data
    auto data = std::make_unique<AmsCurrentToolData>();

    // Find the color swatch child
    data->color_swatch = lv_obj_find_by_name(widget, "color_swatch");
    if (!data->color_swatch) {
        spdlog::warn("[AmsCurrentTool] Could not find color_swatch child");
        return;
    }

    // Set initial color from current subject value
    // Using observer factory for type-safe lambda observer
    using helix::ui::observe_int_sync;
    lv_subject_t* color_subject = AmsState::instance().get_current_color_subject();
    if (color_subject) {
        int color_int = lv_subject_get_int(color_subject);
        lv_color_t color = lv_color_hex(static_cast<uint32_t>(color_int));
        lv_obj_set_style_bg_color(data->color_swatch, color, 0);

        // Capture widget (lv_obj_t*) instead of data pointer to prevent
        // use-after-free when deferred callback executes after widget deletion.
        // The registry lookup acts as a validity check. (fixes #83)
        data->color_observer =
            observe_int_sync<lv_obj_t>(color_subject, widget, [](lv_obj_t* obj, int color_int) {
                auto it = s_registry.find(obj);
                if (it == s_registry.end() || !it->second)
                    return;
                auto* d = it->second.get();
                if (!d->color_swatch)
                    return;
                lv_color_t color = lv_color_hex(static_cast<uint32_t>(color_int));
                lv_obj_set_style_bg_color(d->color_swatch, color, 0);
                spdlog::trace("[AmsCurrentTool] Color updated to 0x{:06X}", color_int);
            });
    }

    // Register cleanup callback
    lv_obj_add_event_cb(widget, on_delete, LV_EVENT_DELETE, nullptr);

    // Store data
    s_registry[widget] = std::move(data);

    spdlog::debug("[AmsCurrentTool] Widget initialized");
}

} // namespace

// Module initialization - call once during app startup
void ui_ams_current_tool_init() {
    // Register click callback for XML event_cb [L007]
    lv_xml_register_event_cb(nullptr, "on_ams_current_tool_clicked", on_clicked);

    spdlog::debug("[AmsCurrentTool] Callbacks registered");
}

// Called after lv_xml_create() for ams_current_tool components
// Must be called manually since LVGL doesn't have automatic post-create hooks
void ui_ams_current_tool_setup(lv_obj_t* widget) {
    on_widget_created(widget);
}
