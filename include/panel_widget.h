// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <string>

#include "hv/json.hpp"

namespace helix {

/// Base class for home widgets that need C++ behavioral wiring.
/// Widgets that are pure XML binding (filament, probe, humidity, etc.) don't need this.
class PanelWidget {
  public:
    virtual ~PanelWidget() = default;

    /// Called BEFORE lv_xml_create() — create and register any LVGL subjects
    /// that XML bindings depend on. Default is no-op.
    virtual void init_subjects() {}

    /// Set per-widget config from PanelWidgetEntry. Called after factory
    /// creation, before get_component_name() and attach().
    virtual void set_config(const nlohmann::json& config) {
        (void)config;
    }

    /// Return the XML component name to use for this widget. Allows widgets
    /// to select different XML layouts based on their config (e.g. carousel
    /// vs stack mode). Default returns "panel_widget_<id>".
    virtual std::string get_component_name() const {
        return std::string("panel_widget_") + id();
    }

    /// Called after XML obj is created. Wire observers, animators, callbacks.
    /// @param widget_obj  The root lv_obj from lv_xml_create()
    /// @param parent_screen  Screen for lazy overlay creation
    virtual void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) = 0;

    /// Called before widget destruction. Clean up observers and state.
    virtual void detach() = 0;

    /// Called when the owning panel becomes visible.
    virtual void on_activate() {}

    /// Called when the owning panel goes offscreen.
    virtual void on_deactivate() {}

    /// Called when settings change and widgets should re-read config.
    virtual void reload_from_config() {}

    /// Called after attach() with the number of widgets sharing this row.
    /// Widgets can use this to adjust font sizes or layout density.
    /// Default is no-op.
    virtual void set_row_density(size_t widgets_in_row) {
        (void)widgets_in_row;
    }

    /// Stable identifier matching PanelWidgetDef::id
    virtual const char* id() const = 0;
};

/// Safe recovery of PanelWidget pointer from event callback.
/// Returns nullptr if widget was detached or obj has no user_data.
template <typename T> T* panel_widget_from_event(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!obj)
        return nullptr;
    auto* raw = lv_obj_get_user_data(obj);
    if (!raw)
        return nullptr;
    return static_cast<T*>(raw);
}

} // namespace helix
