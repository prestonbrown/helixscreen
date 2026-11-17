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

    void register_callbacks() override;
    void on_activate() override;

    /// populate_screensaver_row() on this overlay.
    void init_screensaver_dropdown();

    /// apply_screensaver_pick() on this overlay.
    void handle_screensaver_changed(int index);

  private:
    void init_display_rotation_dropdown();
};

inline DisplaySettingsOverlay& get_display_settings_overlay() {
    return lazy_global<DisplaySettingsOverlay>("DisplaySettingsOverlay");
}

// Rows Settings > Display shares with the setup wizard. Each helper takes the
// container that holds the rows (found by name), so both pages run the same code
// and pick from the same option lists; the callbacks are the overlay's.

/// Moves the brightness slider (row_brightness) to the stored brightness.
void select_brightness_slider(lv_obj_t* root);

/// Selects the stored dim timeout in row_display_dim and row_screensaver_after.
void select_dim_rows(lv_obj_t* root);

/// Selects the stored sleep timeout in row_display_sleep.
void select_sleep_row(lv_obj_t* root);

/// Which explicit UI scale steps a UI scale row offers below Automatic.
enum class UiScaleOffer {
    All,     ///< Every step, for Settings
    Fitting, ///< Only steps that leave the screen room for the smallest layout
};

/// The explicit UI scale percentages, in dropdown order after Automatic.
std::vector<int> ui_scale_percents(UiScaleOffer offer);

/// The explicit steps at which a @p width x @p height screen still holds the
/// smallest layout (480x272) in either orientation.
std::vector<int> fitting_ui_scale_percents(int width, int height);

/// Whether a row offering Automatic (resolving to @p auto_percent) and @p fitting
/// gives more than one size to choose from.
bool ui_scale_has_choice(const std::vector<int>& fitting, int auto_percent);

/// ui_scale_has_choice() for this display: whether a Fitting row offers a real choice.
bool fitting_ui_scale_has_choice();

/// Fills row_ui_scale with Automatic and the offered steps, selecting the stored one.
void populate_ui_scale_row(lv_obj_t* root, UiScaleOffer offer);

/// Stores the scale at dropdown @p index of a row filled by populate_ui_scale_row().
void apply_ui_scale_pick(int index);

/// Lists Off and the savers this board can run in row_screensaver, selecting the one
/// that runs for the stored type: a too-heavy stored choice shows its fallback, and
/// the stored value is left alone.
void populate_screensaver_row(lv_obj_t* root);

/// Stores the saver at dropdown @p index of row_screensaver, then refreshes the rows
/// under @p root that the choice can change (the saver list, dim and sleep).
void apply_screensaver_pick(lv_obj_t* root, int index);

} // namespace helix::settings
