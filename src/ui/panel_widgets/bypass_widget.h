// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_ams_context_menu.h"
#include "ui_bypass_toggle_controller.h"
#include "ui_widget_ref.h"

#include "core/observer_guard.h"
#include "src/ui/panel_widgets/tiled_panel_widget.h"

#include <memory>

namespace helix {

/// Home-panel Bypass tile. Pure renderer: state comes from the ams_bypass_* /
/// print_active / ams_external_spool_* subjects; the only C++ behavior is the
/// click and the dynamic color of the external-spool dot (XML styles cannot
/// bind non-constant colors).
///
/// The click opens the external-spool context menu rather than toggling
/// directly. The tile is the one place on the home screen that represents the
/// bypass spool, so every question asked of that spool — engage, load, unload,
/// purge, which spool is on it — is answered from the same menu the AMS panels
/// show, instead of the tile answering one of them and the rest living a panel
/// away.
class BypassWidget : public TiledPanelWidget {
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
    helix::ui::WidgetRef widget_obj_;
    helix::ui::WidgetRef parent_screen_;
    helix::ui::BypassToggleController toggle_;
    /// Lazily created on first tap and reused, like the AMS panels' own.
    std::unique_ptr<helix::ui::AmsContextMenu> context_menu_;
    // External-spool color observer guard (reset in detach()).
    ObserverGuard spool_color_observer_;
    /// Re-measures the tile when the spool row under its caption comes or goes.
    ObserverGuard bypass_active_observer_;

    void handle_click();
};

void register_bypass_widget();

} // namespace helix
