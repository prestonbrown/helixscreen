// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_exclude_object_map_view.h"

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "panel_widget.h"
#include "src/ui/panel_widgets/exclude_map_layout.h"

#include <memory>
#include <string>

namespace helix {

class PrinterState;

/// Home widget for the objects of a multi-object print. Measures what each rung would
/// draw, decides the rung with decide_exclude_map_layout(), and publishes the verdict and
/// the counts as subjects that panel_widget_exclude_map.xml binds. From the Column rung up
/// it holds the mini mode of the exclude-object map (outlines only). The tile is
/// read-only: a tap, or the Full rung's button, opens the full map in print status, where
/// the confirmation and the undo window guard an exclusion.
class ExcludeMapWidget : public PanelWidget {
  public:
    explicit ExcludeMapWidget(PrinterState& printer_state);
    ~ExcludeMapWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_activate() override;
    void on_deactivate() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool fits_at(int width_px, int height_px) const override;
    const char* id() const override {
        return "exclude_map";
    }

    static void clicked_cb(lv_event_t* e);
    static void skip_cb(lv_event_t* e);

    /// Navigate to print status with the full map open. Does nothing on a placeholder:
    /// without two objects there is no map to open.
    void open_full_map();

  protected:
    void on_hooked_root_deleted() override;

  private:
    struct Live;
    Live read_live() const;
    ExcludeMapVerdict decide(int width_px, int height_px, ExcludeMapState state, int digits) const;
    void on_objects_changed();
    void publish(const Live& live);
    /// @p current as the "Now printing" label can draw it; "" for none.
    std::string current_display_name(const std::string& current) const;
    void sync_map();

    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    ExcludeMapVerdict verdict_;
    /// The state and object-count width the verdict was decided for; a change re-decides,
    /// because the content being measured changed.
    ExcludeMapState decided_state_ = ExcludeMapState::NoObjects;
    int decided_digits_ = 1;
    /// Whether the home panel is showing this tile. The map is built only then.
    bool active_ = false;
    std::unique_ptr<ui::ExcludeObjectMapView> map_view_;

    ObserverGuard defined_observer_;
    ObserverGuard excluded_observer_;
    ObserverGuard print_active_observer_;
    ObserverGuard language_observer_;

    // Declared last so it is torn down first, expiring every token the
    // observers captured before any of them is destroyed.
    helix::AsyncLifetimeGuard lifetime_;
};

void register_exclude_map_widget();

} // namespace helix
