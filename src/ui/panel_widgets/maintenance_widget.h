// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/observer_guard.h"
#include "panel_widget.h"
#include "src/ui/panel_widgets/maintenance_tile_layout.h"

#include <lvgl.h>

namespace helix {

/// Home tile for the maintenance meters (prestonbrown/helixscreen#1807). The meters
/// themselves live in maintenance_model; this widget measures what each rung would draw,
/// decides the rung with decide_maintenance_tile_layout(), and publishes
/// `maintenance_rung`, `maintenance_show_total` and `maintenance_ring_d` for
/// panel_widget_maintenance.xml. A tap opens the detail overlay, where meters are reset
/// and intervals set.
class MaintenanceWidget : public PanelWidget {
  public:
    MaintenanceWidget();
    ~MaintenanceWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool fits_at(int width_px, int height_px) const override;
    const char* id() const override {
        return "maintenance";
    }

    static void maintenance_clicked_cb(lv_event_t* e);

  protected:
    void on_hooked_root_deleted() override;

  private:
    [[nodiscard]] MaintenanceTileVerdict decide(int width_px, int height_px) const;
    /// Re-decide against the granted size and publish, when this instance owns the tile.
    void refresh();

    friend class MaintenanceWidgetTestAccess;

    lv_obj_t* parent_screen_ = nullptr;
    int granted_w_ = 0;
    int granted_h_ = 0;
    ObserverGuard version_observer_;
};

void register_maintenance_widget();

} // namespace helix
