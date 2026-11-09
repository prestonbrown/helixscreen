// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/observer_guard.h"
#include "src/ui/panel_widgets/tiled_panel_widget.h"

namespace helix {

class LedControlsWidget : public TiledPanelWidget {
  public:
    // The widget only opens the LED overlay, which reaches LedController and
    // NavigationManager through their own singletons — it needs no printer state
    // or API handle of its own.
    LedControlsWidget()
        : TiledPanelWidget("led_controls", TileSizing::Content{"", "", "LEDs", false}) {}
    ~LedControlsWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "led_controls";
    }

    static void on_led_controls_clicked(lv_event_t* e);

  private:
    void handle_clicked();

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
};

} // namespace helix
