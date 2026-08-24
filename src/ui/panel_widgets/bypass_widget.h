// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_bypass_toggle_controller.h"
#include "ui_observer_guard.h"

#include "panel_widget.h"

namespace helix {

/// Home-panel Bypass tile. Pure renderer: state comes from the ams_bypass_* /
/// print_active / ams_external_spool_* subjects; the only C++ behavior is the
/// click (delegated to the shared BypassToggleController) and the dynamic
/// color of the external-spool dot (XML styles cannot bind non-constant
/// colors).
class BypassWidget : public PanelWidget {
  public:
    BypassWidget();
    ~BypassWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "bypass";
    }

    static void clicked_cb(lv_event_t* e);

  private:
    lv_obj_t* widget_obj_ = nullptr;
    helix::ui::BypassToggleController toggle_;
    // External-spool color observer guard (reset in detach()).
    ObserverGuard spool_color_observer_;

    void handle_click();
};

void register_bypass_widget();

} // namespace helix
