// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/static_panel_registry.h"
#include "overlay_base.h"

namespace helix {

/// The maintenance meters' overview, opened from the home tile: a card per enabled meter,
/// which opens its task page, the total print time, and the way to the reminder intervals.
class MaintenanceOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Maintenance";
    }
    const char* xml_component() const override {
        return "maintenance_overlay";
    }
    // Keeps its tree: opened during a print on slow boards, where rebuild cost is unmeasured.
    bool destroy_on_close() const override {
        return false;
    }

    void register_callbacks() override;
};

/// One meter's task page: what it is, how to do it, and Mark done. Filled through
/// maintenance_model::set_task_meter().
class MaintenanceTaskOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Maintenance Task";
    }
    const char* xml_component() const override {
        return "maintenance_task_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void register_callbacks() override;
};

/// Every meter's hours and day limit; the only place an Off meter is turned back on.
class MaintenanceIntervalsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Maintenance Intervals";
    }
    const char* xml_component() const override {
        return "maintenance_intervals_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void register_callbacks() override;

    /// Fills each row's dropdowns with the shared choices and selects its meter's values.
    static void apply_options(lv_obj_t* root);

  protected:
    void before_show() override;
};

inline MaintenanceOverlay& get_maintenance_overlay() {
    return lazy_global<MaintenanceOverlay>("MaintenanceOverlay");
}

inline MaintenanceTaskOverlay& get_maintenance_task_overlay() {
    return lazy_global<MaintenanceTaskOverlay>("MaintenanceTaskOverlay");
}

inline MaintenanceIntervalsOverlay& get_maintenance_intervals_overlay() {
    return lazy_global<MaintenanceIntervalsOverlay>("MaintenanceIntervalsOverlay");
}

} // namespace helix
