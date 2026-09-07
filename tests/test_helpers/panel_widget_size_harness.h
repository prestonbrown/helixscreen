// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "panel_widget.h"
#include "theme_manager.h"

#include <string>
#include <utility>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace helix {

/// Per-widget config applied before the component name is resolved.
///
/// PanelWidgetManager's order is: construct, set_config(), get_component_name(),
/// attach(). Widgets whose component name depends on config — fan_stack picks
/// between its stack and carousel layouts — resolve to the wrong component
/// without this.
struct HarnessConfig {
    nlohmann::json value;
};

/// Creates a widget's real XML component, attaches the widget to it, and
/// drives on_size_changed().
///
/// Variadic over the constructor so one harness covers every shape in the set:
/// default-constructed, MoonrakerAPI*, PrinterState&, (string, PrinterState&),
/// and instance-id string.
template <typename W> class PanelWidgetHarness {
  public:
    template <typename... Args>
    explicit PanelWidgetHarness(lv_obj_t* screen, Args&&... args)
        : widget_(std::forward<Args>(args)...) {
        create_and_attach(screen);
    }

    /// Overload for widgets whose get_component_name() depends on config
    /// (e.g. fan_stack's stack/carousel choice). Calls set_config() before
    /// the component name is resolved, matching PanelWidgetManager's real
    /// construction order.
    template <typename... Args>
    PanelWidgetHarness(lv_obj_t* screen, HarnessConfig config, Args&&... args)
        : widget_(std::forward<Args>(args)...) {
        widget_.set_config(config.value);
        create_and_attach(screen);
    }

    ~PanelWidgetHarness() {
        widget_.detach();
    }

    PanelWidgetHarness(const PanelWidgetHarness&) = delete;
    PanelWidgetHarness& operator=(const PanelWidgetHarness&) = delete;

    /// Drive a size change and settle the layout. Does not pump timers; a test
    /// that needs those calls process_lvgl() itself.
    ///
    /// width_px/height_px are the widget's real granted cell size in production
    /// (PanelWidgetManager computes them via grid_track_extent() and the widget
    /// sits in that grid cell — panel_widget_manager.cpp). Apply them to obj_
    /// here too, or any assertion about geometry measures an unconstrained
    /// object rather than a sized one — LV_PCT()-based children resolve
    /// against whatever obj_'s last real size happened to be, not the size
    /// this call claims to represent.
    ///
    /// Order matters: obj_ must already report its new size (via an
    /// intervening layout pass) *before* on_size_changed() runs, because a
    /// widget's own on_size_changed can read that geometry — e.g.
    /// ToolSwitcherWidget::rebuild_pills() measures a child container's
    /// height, which depends on obj_'s width already having settled.
    /// lv_obj_get_width()-style getters read the last computed coord, not
    /// what was just lv_obj_set_size()'d (see tests/CLAUDE.md's "LVGL traps"
    /// section) — so set + update_layout has to happen first, not just
    /// first-in-program-order.
    void resize(int colspan, int rowspan, int width_px, int height_px) {
        lv_obj_set_size(obj_, width_px, height_px);
        lv_obj_update_layout(obj_);
        // notify_size_changed(), not on_size_changed(): PanelWidgetManager calls
        // the recording entry point, and a widget that replays the last granted
        // size on rebuild only sees one if the harness records it too.
        widget_.notify_size_changed(colspan, rowspan, width_px, height_px);
        lv_obj_update_layout(obj_);
    }

    lv_obj_t* child(const char* name) {
        return lv_obj_find_by_name(obj_, name);
    }
    W& widget() {
        return widget_;
    }
    lv_obj_t* root() {
        return obj_;
    }

  private:
    /// Shared tail of both constructors: resolve the component name, build
    /// it from XML, and attach the widget. Kept as one method so the two
    /// constructors cannot drift apart.
    void create_and_attach(lv_obj_t* screen) {
        // From the widget, not "panel_widget_" + id(): fan_stack selects between
        // stack and carousel components, and favorite_macro's id carries an
        // instance suffix that would produce an unregistered component name.
        const std::string component = widget_.get_component_name();
        obj_ = static_cast<lv_obj_t*>(lv_xml_create(screen, component.c_str(), nullptr));
        REQUIRE(obj_ != nullptr);
        widget_.attach(obj_, screen);
    }

    W widget_;
    lv_obj_t* obj_ = nullptr;
};

/// Fails when font tokens are not resolving.
///
/// theme_manager_get_font() falls back to lv_font_get_default() on any miss, so
/// with an uninitialized theme every token returns the same pointer and both
/// branches of a font assertion compare equal while proving nothing. Call this
/// before any font assertion.
inline void require_font_tokens_distinct() {
    REQUIRE(theme_manager_get_font("font_xs") != theme_manager_get_font("font_body"));
}

} // namespace helix
