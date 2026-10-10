// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <string>

#include "hv/json.hpp"

namespace helix {

class TileSizing;

/// Marks the root object of a home panel widget tile, set once at the single
/// creation site in PanelWidgetManager::populate_widgets(). A tile is sized by
/// the home grid and scrolled by dragging it, so page-level affordances do not
/// belong anywhere inside one: PageScrollAutoInject cuts its tree walk here
/// rather than descending into a tile's scrollable innards.
///
/// LVGL gives us four user flag bits. Before claiming one, check the ledger:
///   USER_1  free
///   USER_2  ui_utils.h           EDIT_CLICK_SUPPRESSED_FLAG, clicks edit mode took
///   USER_3  here                 home panel widget tile
///   USER_4  ui_sound_preview_*   suppress the button tap sound
/// USER_3 deliberately over USER_2: helix-xml's flag_to_enum() maps user_1 and
/// user_2 for <bind_flag_if_*>, so USER_3 is the one XML cannot reach.
constexpr lv_obj_flag_t PANEL_WIDGET_TILE_FLAG = LV_OBJ_FLAG_USER_3;

/// Base class for home widget instances: behavior, sizing and lifecycle hooks.
/// A def with no factory (ams) is created as pure XML with no instance; tiles
/// without a class of their own get a sizing-only TileWidget.
class PanelWidget {
  public:
    virtual ~PanelWidget();

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

    /// Bind @p root as this widget's tile root, then attach(). Every caller that
    /// hands a widget a tree goes through here, so the root's user_data
    /// back-pointer (what panel_widget_from_event<T> reads) is set before
    /// attach() on every reuse and no widget sets it itself.
    void attach_tile(lv_obj_t* root, lv_obj_t* parent_screen);

    /// detach(), then clear the root binding. Safe when the root was already
    /// deleted out from under the widget.
    void detach_tile();

    /// The tile root between attach_tile() and detach_tile(); null once the tree
    /// is deleted.
    lv_obj_t* root() const {
        return root_;
    }

    /// Called when the owning panel becomes visible.
    virtual void on_activate() {}

    /// Called when the owning panel goes offscreen.
    virtual void on_deactivate() {}

    /// Called when home edit mode ends, after it gave back CLICKABLE to every
    /// object it took it from. A widget that turns its own controls clickable
    /// or not at runtime re-applies that here: a change it made during the
    /// session is overwritten by the restore.
    virtual void on_edit_mode_exited() {}

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

    /// Record the granted cell size, then hand it to on_size_changed().
    ///
    /// PanelWidgetManager calls THIS, never on_size_changed() directly. The grid
    /// announces a size once per attach, but a widget whose contents arrive later
    /// -- tools discovered over the network, sensors registering -- rebuilds after
    /// that announcement and has no size to lay the new objects out against. Every
    /// widget that measures needs the last granted size, so the base keeps it
    /// rather than each widget carrying its own copy
    /// (prestonbrown/helixscreen#1490).
    void notify_size_changed(int colspan, int rowspan, int width_px, int height_px) {
        granted_colspan_ = colspan;
        granted_rowspan_ = rowspan;
        granted_width_px_ = width_px;
        granted_height_px_ = height_px;
        has_granted_size_ = true;
        on_size_changed(colspan, rowspan, width_px, height_px);
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

    /// Attributes to hand this instance's component at creation, as the flat
    /// nullptr-terminated key/value list lv_xml_create() takes. Returning
    /// nullptr creates the component with no attributes, which is what a widget
    /// that binds nothing per-instance wants.
    ///
    /// This is how a tile's per-instance subject NAMES reach its XML. They must
    /// exist before the component is parsed, so whatever supplies them is
    /// constructed with the widget, not in attach().
    virtual const char** xml_attrs() const {
        return nullptr;
    }

    /// The tile's sizing helper, for widgets that have one. The manager hands
    /// it the created root so the verdict is computed against the box the
    /// content really draws in rather than the tile's outer box.
    virtual TileSizing* tile_sizing() {
        return nullptr;
    }

    /// Whether this widget can render its identifying content in a box of this
    /// size. Edit mode's resize clamp and the load path both ask before
    /// offering a size, and a widget that returns false at a size is never
    /// given it. Default true, so a widget that has not opted in keeps exactly
    /// its registry limits.
    ///
    /// Pixels are the grid's arithmetic extent for the span
    /// (grid_track_extent), not laid-out geometry: the same numbers
    /// on_size_changed() carries, and available before any layout pass runs.
    ///
    /// Must be MONOTONIC. A widget that fits at a size fits at every larger
    /// size on both axes, because the clamp walks outward assuming the first
    /// accepting size is the nearest one.
    virtual bool fits_at(int width_px, int height_px) const {
        (void)width_px;
        (void)height_px;
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

    /**
     * @brief Install the raw-delete hook on this widget's tile root.
     *
     * Call from attach() once the root object is known. detach() is called by
     * every CURRENT deletion path before the tree is condemned, but a raw
     * lv_obj_delete() of the home page container (screen teardown) gives the
     * widget no call: the async-guard tokens stay live and the cached child
     * pointers dangle, so the next UpdateQueue drain writes into freed widgets
     * (#776 family — the PowerPanel/PrintStatusPanel LV_EVENT_DELETE hook,
     * lifted into the base so every PanelWidget gets it once).
     *
     * Safe across rebuilds: removes the hook from a previously hooked root
     * first, so a recycled instance never leaves one behind on the tree it was
     * detached from (or re-attached away from).
     */
    void install_delete_hook(lv_obj_t* root);

    /// The cell size the grid last granted. Zero until on_size_changed() has run,
    /// which has_granted_size() distinguishes from a genuine zero.
    bool has_granted_size() const {
        return has_granted_size_;
    }
    int granted_colspan() const {
        return granted_colspan_;
    }
    int granted_rowspan() const {
        return granted_rowspan_;
    }
    int granted_width_px() const {
        return granted_width_px_;
    }
    int granted_height_px() const {
        return granted_height_px_;
    }

    /// Re-run the layout decision against the last granted size.
    ///
    /// Call at the end of any rebuild that recreates the objects on_size_changed()
    /// lays out; without it those objects keep whatever the XML gave them. A no-op
    /// before the first size arrives, so an early rebuild is harmless. Do not call
    /// it from on_size_changed() itself.
    ///
    /// These accessors serve the REPLAY, and only notify_size_changed() fills
    /// them: an override reached by a direct on_size_changed() call still sees
    /// zeroes here. So an override must work from its own parameters and keep
    /// whatever it needs later in its own members, exactly as ToolSwitcherWidget
    /// does — reading these instead is a silent dependency on which caller you
    /// arrived through.
    void relayout_for_granted_size() {
        if (has_granted_size_) {
            on_size_changed(granted_colspan_, granted_rowspan_, granted_width_px_,
                            granted_height_px_);
        }
    }

    /**
     * @brief Remove the hook installed by install_delete_hook().
     *
     * Call from detach(). The destructor calls it too, so a tile tree that
     * outlives the widget (manager dropping a non-reused instance, app
     * shutdown destroying panels before lv_deinit()) can never fire the hook
     * on freed memory.
     */
    void uninstall_delete_hook();

    /**
     * @brief Override to drop cached child pointers when the tile tree dies
     *        by anything other than detach().
     *
     * Runs inside LVGL's delete event, mid-teardown of the tree: pointer drops
     * and async-guard expiry ONLY. Never reset observers or reparent/clean
     * widgets from here — those need a live tree and a non-delete context
     * (detach() and the destructor still run later on the normal paths).
     */
    virtual void on_hooked_root_deleted() {}

  private:
    static void on_root_deleted_event(lv_event_t* e);
    static void on_bound_root_deleted(lv_event_t* e);
    void bind_root(lv_obj_t* obj);
    void unbind_root();

    lv_obj_t* root_ = nullptr;

    /// Where the delete hook is currently installed. Kept separately from any
    /// widget-owned root pointer because those are cleared by paths that
    /// detach the tree before its deferred deletion runs.
    lv_obj_t* delete_hook_root_ = nullptr;

  private:
    std::string panel_id_;
    bool has_granted_size_ = false;
    int granted_colspan_ = 0;
    int granted_rowspan_ = 0;
    int granted_width_px_ = 0;
    int granted_height_px_ = 0;
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
