// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "panel_widget.h"
#include "src/ui/panel_widgets/speed_flow_layout.h"

namespace helix {

class PrinterState;

/// Home widget for the speed and flow overrides. Measures what each rung would
/// draw, decides the rung with decide_speed_flow_layout(), and publishes the
/// verdict and the readings as subjects that panel_widget_speed_flow.xml binds.
/// A tap opens the Tune overlay; the Full rung adds steppers while printing.
class SpeedFlowWidget : public PanelWidget {
  public:
    explicit SpeedFlowWidget(PrinterState& printer_state);
    ~SpeedFlowWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool fits_at(int width_px, int height_px) const override;
    const char* id() const override {
        return "speed_flow";
    }

    static void clicked_cb(lv_event_t* e);

  private:
    struct LiveState;
    LiveState read_live() const;
    SpeedFlowVerdict decide(int width_px, int height_px, bool printing, bool physical) const;
    void publish_texts();
    void on_live_changed();
    void open_tune_overlay();

    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    SpeedFlowVerdict verdict_;
    /// The state the verdict was decided for; a change re-decides, because the
    /// content being measured changed.
    bool decided_printing_ = false;
    bool decided_physical_ = false;

    ObserverGuard speed_observer_;
    ObserverGuard flow_observer_;
    ObserverGuard velocity_observer_;
    ObserverGuard extruder_velocity_observer_;
    ObserverGuard print_active_observer_;
    ObserverGuard units_observer_;

    // Declared last so it is torn down first, expiring every token the
    // observers captured before any of them is destroyed.
    helix::AsyncLifetimeGuard lifetime_;
};

void register_speed_flow_widget();

} // namespace helix
