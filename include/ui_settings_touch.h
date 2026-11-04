// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_touch.h
 * @brief Touch & Input settings overlay — calibration, debug viz, scroll feel
 *
 * Reached from Settings → Touch & Input. Groups everything that affects how the
 * screen reads finger input, plus the Scroll Buttons, System Keyboard and Keep
 * Navigation Bar rows.
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 */

#pragma once

#include "core/static_panel_registry.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

namespace helix::settings {

class TouchSettingsOverlay : public OverlayBase {
  public:
    ~TouchSettingsOverlay() override;

    const char* get_name() const override {
        return "TouchInput";
    }
    const char* xml_component() const override {
        return "settings_touch_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void init_subjects() override;
    void deinit_subjects();
    void register_callbacks() override;
    void on_activate() override;

  private:
    void init_input_sliders();
    void refresh_calibration_status();
    void show_calibration_status(bool calibrated);
    void handle_touch_calibration_clicked();

    SubjectManager subjects_;
    lv_subject_t show_touch_calibration_subject_{}; ///< 1 when a manual calibration entry applies
    lv_subject_t touch_cal_status_subject_{};       ///< "Calibrated" / "Not calibrated"
    char touch_cal_status_buf_[48] = {};
};

inline TouchSettingsOverlay& get_touch_settings_overlay() {
    return lazy_global<TouchSettingsOverlay>("TouchSettingsOverlay");
}

} // namespace helix::settings
