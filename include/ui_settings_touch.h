// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_touch.h
 * @brief Touch & Input settings overlay — calibration, debug viz, scroll feel
 *
 * Reached from Settings → System → Touch & Input. Groups everything that affects
 * how the screen reads finger input so System overlay stays focused on admin tasks.
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

class TouchSettingsOverlay : public OverlayBase {
  public:
    TouchSettingsOverlay();
    ~TouchSettingsOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "TouchInput";
    }

    void on_activate() override;
    void on_deactivate() override;

    lv_obj_t* create(lv_obj_t* parent) override;
    void show(lv_obj_t* parent_screen);

    bool is_created() const {
        return overlay_root_ != nullptr;
    }

  private:
    void init_input_sliders();
};

TouchSettingsOverlay& get_touch_settings_overlay();

} // namespace helix::settings
