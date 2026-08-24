// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"

#include <memory>

class IMoonrakerAPI;

namespace helix {

class ActiveSpoolWidget : public PanelWidget {
  public:
    explicit ActiveSpoolWidget(IMoonrakerAPI* api);
    ~ActiveSpoolWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    const char* id() const override {
        return "active_spool";
    }

    static void clicked_cb(lv_event_t* e);

  private:
    IMoonrakerAPI* api_;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    // Compact mode elements
    lv_obj_t* spool_compact_ = nullptr;

    // Wide mode elements
    lv_obj_t* wide_layout_ = nullptr;
    lv_obj_t* spool_wide_ = nullptr;
    lv_obj_t* material_label_ = nullptr;
    lv_obj_t* brand_color_label_ = nullptr;
    lv_obj_t* weight_label_ = nullptr;

    // No-spool label
    lv_obj_t* no_spool_label_ = nullptr;

    ObserverGuard spool_color_observer_;
    ObserverGuard current_slot_observer_;
    ObserverGuard slots_version_observer_;

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer destructs. Without this, queued observer callbacks captured
    // via tok.defer() see token.expired() == false after the observers are
    // already gone and dereference a half-destroyed widget. See temp_stack_widget.h
    // (commit 45abc8c2a, bundle AX3CKAKB).
    helix::AsyncLifetimeGuard lifetime_;

    bool is_wide_ = false;

    void update_spool_display();
    void resize_spool_canvases();
    void apply_layout_visibility();
    void handle_clicked();
    void open_external_spool_edit();
};

} // namespace helix
