// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_ams_current_tool.cpp
 * @brief C++ backing for the ams_current_tool XML component
 *
 * Handles:
 * - Color swatch paint (the loaded filament's colour, colours and finish)
 * - Click handler (opens AMS panel)
 * - Cleanup on widget deletion
 */

#include "ui_callback_helpers.h"
#include "ui_observer_guard.h"
#include "ui_panel_ams.h"
#include "ui_panel_ams_overview.h"
#include "ui_swatch.h"

#include "ams_state.h"
#include "core/observer_factory.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <unordered_map>

namespace {

// Per-widget data for ams_current_tool instances
struct AmsCurrentToolData {
    lv_obj_t* color_swatch = nullptr;
    ObserverGuard color_observer;
    ObserverGuard colors_observer;
    ObserverGuard finish_observer;
};

// Registry for cleanup
static std::unordered_map<lv_obj_t*, std::unique_ptr<AmsCurrentToolData>> s_registry;

// Paint the swatch of @p widget from the loaded filament: its colour, its
// multi-colour list and its finish. The registry lookup is the validity check
// for a deferred observer that runs after the widget is gone.
static void repaint(lv_obj_t* widget) {
    auto it = s_registry.find(widget);
    if (it == s_registry.end() || !it->second || !it->second->color_swatch)
        return;
    const auto paint = helix::AmsState::instance().current_paint();
    helix::ui::apply_swatch_color(it->second->color_swatch, paint.rgb, paint.multi_color_hexes,
                                  helix::ui::swatch_finish(paint.finish));
}

// Cleanup callback when widget is deleted
static void on_delete(lv_event_t* e) {
    lv_obj_t* widget = lv_event_get_target_obj(e);
    auto it = s_registry.find(widget);
    if (it != s_registry.end()) {
        it->second->color_observer.reset();
        it->second->colors_observer.reset();
        it->second->finish_observer.reset();
        s_registry.erase(it);
        spdlog::trace("[AmsCurrentTool] Widget cleaned up");
    }
}

// Post-create hook called after XML creates the widget
static void on_widget_created(lv_obj_t* widget) {
    if (!widget)
        return;

    auto data = std::make_unique<AmsCurrentToolData>();
    data->color_swatch = lv_obj_find_by_name(widget, "color_swatch");
    if (!data->color_swatch) {
        spdlog::warn("[AmsCurrentTool] Could not find color_swatch child");
        return;
    }

    // Each observer captures the widget (lv_obj_t*), never the data pointer:
    // repaint() looks it up in the registry, so a deferred callback after the
    // widget's deletion does nothing (#83).
    using helix::ui::observe;
    auto& ams = helix::AmsState::instance();
    data->color_observer = observe<int>(
        ams.get_current_color_subject(), widget, [](lv_obj_t* obj, int) { repaint(obj); },
        ams.get_subjects_lifetime());
    data->colors_observer = observe<const char*>(
        ams.get_current_colors_subject(), widget, [](lv_obj_t* obj, const char*) { repaint(obj); },
        ams.get_subjects_lifetime());
    data->finish_observer = observe<const char*>(
        ams.get_current_finish_subject(), widget, [](lv_obj_t* obj, const char*) { repaint(obj); },
        ams.get_subjects_lifetime());

    // Register cleanup callback
    lv_obj_add_event_cb(widget, on_delete, LV_EVENT_DELETE, nullptr);

    s_registry[widget] = std::move(data);
    repaint(widget);

    spdlog::debug("[AmsCurrentTool] Widget initialized");
}

} // namespace

// Module initialization - call once during app startup
void ui_ams_current_tool_init() {
    // Register click callback for XML event_cb [L007]
    register_xml_callbacks({
        {"on_ams_current_tool_clicked",
         [](lv_event_t*) {
             spdlog::debug("[AmsCurrentTool] Clicked - opening AMS panel");
             navigate_to_ams_panel();
         }},
    });

    spdlog::debug("[AmsCurrentTool] Callbacks registered");
}

// Called after lv_xml_create() for ams_current_tool components
// Must be called manually since LVGL doesn't have automatic post-create hooks
void ui_ams_current_tool_setup(lv_obj_t* widget) {
    on_widget_created(widget);
}
