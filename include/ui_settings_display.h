// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_display.h
 * @brief Display settings overlay - brightness, dim, sleep, screensaver, rotation, UI scale
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see DisplaySettingsManager for persistence
 */

#pragma once

#include "core/static_panel_registry.h"
#include "overlay_base.h"
#include "screensaver_registry.h"
#include "subject_managed_panel.h"

#include <vector>

namespace helix::settings {

class DisplaySettingsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Display";
    }
    const char* xml_component() const override {
        return "settings_display_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void init_subjects() override;
    void register_callbacks() override;
    void on_activate() override;

    /// Slider drag tick: previews the backlight and updates the readout without persisting.
    void handle_brightness_changed(int value);

    /// Lists Off and the savers this board can run, selecting the one that runs for the stored
    /// type: a too-heavy stored choice shows its fallback, and the stored value is left alone.
    void init_screensaver_dropdown();

    /// Stores the saver at dropdown `index` of the list init_screensaver_dropdown() built.
    void handle_screensaver_changed(int index);

  private:
    void init_display_rotation_dropdown();
    void init_brightness_controls();
    void init_dim_dropdown();
    void init_sleep_dropdown();
    void init_ui_scale_dropdown();

    /// SubjectManager, declared ahead of the subjects it owns so it tears down
    /// after them (names withdraw before storage dies).
    SubjectManager subjects_;

    /// Subject for brightness value label binding
    lv_subject_t brightness_value_subject_{};
    char brightness_value_buf_[8]; // e.g., "100%"

    /// Stable registry name ("off", "maze", ...) at each screensaver dropdown index.
    std::vector<const char*> screensaver_choices_;
};

inline DisplaySettingsOverlay& get_display_settings_overlay() {
    return lazy_global<DisplaySettingsOverlay>("DisplaySettingsOverlay");
}

} // namespace helix::settings
