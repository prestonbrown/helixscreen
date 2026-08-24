// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <string>

#include "hv/json.hpp"

namespace helix {

/// Marks the root object of a home panel widget tile, set once at the single
/// creation site in PanelWidgetManager::populate_widgets(). A tile is sized by
/// the home grid and scrolled by dragging it, so page-level affordances do not
/// belong anywhere inside one: PageScrollAutoInject cuts its tree walk here
/// rather than descending into a tile's scrollable innards.
///
/// LVGL gives us four user flag bits and this repo has now claimed three of
/// them. Check this ledger before taking another:
///   USER_1  ui_dialog.cpp        "inside a dialog" elevated-surface marker
///   USER_2  free
///   USER_3  here                 home panel widget tile
///   USER_4  ui_sound_preview_*   suppress the button tap sound
/// USER_3 deliberately over USER_2: helix-xml's flag_to_enum() maps user_1 and
/// user_2 for <bind_flag_if_*>, so USER_3 is the one XML cannot reach.
constexpr lv_obj_flag_t PANEL_WIDGET_TILE_FLAG = LV_OBJ_FLAG_USER_3;

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

    /// Called after grid cell placement and whenever the widget is resized.
    /// Widgets can adapt their content layout based on available space.
    /// @param colspan  Grid columns spanned
    /// @param rowspan  Grid rows spanned
    /// @param width_px  Actual pixel width of the widget
    /// @param height_px  Actual pixel height of the widget
    virtual void on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
        (void)colspan;
        (void)rowspan;
        (void)width_px;
        (void)height_px;
    }

    /// Whether this widget currently has an overlay open (e.g. fullscreen camera).
    /// Gate observer rebuilds must not run while an overlay is open — detach()
    /// would destroy the overlay's LVGL objects mid-display.
    virtual bool has_overlay_open() const {
        return false;
    }

    /// Whether this widget's C++ instance can be reused across rebuilds.
    /// When true, detach() must be lightweight (clear LVGL pointers only),
    /// preserving expensive state like camera streams. The destructor
    /// handles full cleanup. Default: true. Override to return false if
    /// the widget's detach() is irreversible or cannot be re-attached.
    virtual bool supports_reuse() const {
        return true;
    }

    /// Whether this widget supports configuration in edit mode.
    /// Override to return true to show the configure (gear) button.
    virtual bool has_edit_configure() const {
        return false;
    }

    /// Called when the configure button is pressed in edit mode. Return true
    /// if handled (triggers rebuild). Widgets can toggle display modes, open
    /// config modals, etc.
    virtual bool on_edit_configure() {
        return false;
    }

    /// Stable identifier matching PanelWidgetDef::id
    virtual const char* id() const = 0;

    /// Panel ID this widget belongs to. Set by PanelWidgetManager before attach().
    const std::string& panel_id() const {
        return panel_id_;
    }
    void set_panel_id(const std::string& panel_id) {
        panel_id_ = panel_id;
    }

    /// Persist per-widget config through the PanelWidgetManager.
    /// Widgets call this instead of reaching into PanelWidgetManager directly.
    void save_widget_config(const nlohmann::json& config);

  protected:
    /// Call from widget event callbacks to track user interactions for telemetry
    void record_interaction();

  private:
    std::string panel_id_;
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
