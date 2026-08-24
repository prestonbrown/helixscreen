// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "panel_widget.h"

#include <string>

namespace helix {

class PrinterImageWidget : public PanelWidget {
  public:
    PrinterImageWidget();
    ~PrinterImageWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    /// Factory-registration key. Exposed so callers scanning a heterogeneous
    /// widget list can match on id() and static_cast, instead of dynamic_cast —
    /// the firmware builds -fno-rtti.
    static constexpr const char* WIDGET_ID = "printer_image";

    const char* id() const override {
        return WIDGET_ID;
    }

    /// Called when panel activates — re-check if printer image changed in settings
    void on_activate();

    /// Reload printer image and printer info subjects from config
    void reload_from_config();

    /// Re-check printer image setting and update the displayed image
    void refresh_printer_image();

    /// XML event callback — opens printer manager overlay
    static void printer_manager_clicked_cb(lv_event_t* e);

  private:
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    // Persistent disk cache for exact-size printer image
    lv_timer_t* cache_timer_ = nullptr;
    // Defers the image refresh out of the synchronous attach()/on_activate()
    // path: lv_image_set_inner_align forces lv_obj_update_layout, which cascades
    // into the parent grid's grid_update. Running that during a panel rebuild
    // (mid-rebuild grid) walked the freed descriptor off the heap end (#983/#1025).
    lv_timer_t* refresh_timer_ = nullptr;
    std::string current_source_path_; // Resolved source image (LVGL path)

    void schedule_image_refresh();
    void schedule_cache_check();
    void check_or_generate_cache();

    void handle_printer_manager_clicked();
};

} // namespace helix
