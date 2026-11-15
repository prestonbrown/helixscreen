// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "src/ui/panel_widgets/full_page_host.h"
#include "src/ui/panel_widgets/tiled_panel_widget.h"

#include <string>

namespace helix {

/// What a Filament operations full page hosts, for jump_to_full_page().
inline constexpr const char kFilamentOpsFunction[] = "filament";

/// Open filament operations from a home tile: the full page when one is placed,
/// else the Filament panel, the same request as its navigation bar button.
void open_filament_controls();

/// The Filament operations tile. Multi-instance, so a compact tile on one page
/// and a full page (FullPageHost, lending the Filament panel's tree) on another
/// can both exist; the compact one jumps there.
class FilamentOpsWidget : public TiledPanelWidget {
  public:
    explicit FilamentOpsWidget(const std::string& instance_id = "filament_ops");
    ~FilamentOpsWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_activate() override;
    void on_deactivate() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;

  protected:
    void on_hooked_root_deleted() override;

  public:
    std::string get_component_name() const override {
        return "panel_widget_filament_ops";
    }
    const char* id() const override {
        return instance_id_.c_str();
    }

    const FullPageHost& full_page() const {
        return full_page_;
    }

    static void clicked_cb(lv_event_t* e);

  private:
    std::string instance_id_;
    FullPageHost full_page_;

    void handle_click();
};

void register_filament_ops_widget();

} // namespace helix
