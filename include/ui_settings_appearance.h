// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_appearance.h
 * @brief Appearance settings overlay - dark mode, theme, animations, widget labels,
 *        printer visuals (toolhead style, G-code preview, Z movement, bed mesh)
 *
 * Owns the theme explorer (theme_preview_overlay) and the theme editor entry point.
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see DisplaySettingsManager for persistence
 */

#pragma once

#include "overlay_base.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"
#include "theme_loader.h"

#include <string>
#include <vector>

namespace helix::settings {

class AppearanceSettingsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Appearance";
    }
    const char* xml_component() const override {
        return "settings_appearance_overlay";
    }

    void init_subjects() override;
    void register_callbacks() override;
    void on_activate() override;

    //
    // === Theme explorer (called from the callback table) ===
    //

    void handle_theme_settings_clicked();
    /**
     * @brief Make the theme explorer treat the active theme as committed
     *
     * Rebuilds the preset list, selects the active theme, and makes it what
     * closing the explorer reverts to. Call after anything outside the explorer
     * persists a theme (the editor's Save / Save As), or closing the explorer
     * throws that theme away. No-op when the explorer is not open.
     */
    void sync_explorer_to_active_theme();

    void handle_theme_preset_changed(int index);
    void handle_apply_theme_clicked();
    void handle_edit_colors_clicked();
    void handle_explorer_theme_changed(int index);
    void handle_preview_dark_mode_toggled(bool is_dark);
    void apply_preview_palette_to_screen_popups();

  private:
    void init_toolhead_style_dropdown();
    void init_gcode_mode_dropdown();
    void init_theme_preset_dropdown(lv_obj_t* root);

    /// Theme Explorer overlay (primary - for browsing and selecting themes)
    helix::ui::WidgetRef theme_explorer_overlay_;

    /// Tracks original theme index for Apply button state
    int original_theme_index_{-1};
    /// Snapshot of active theme when explorer opens (for revert on close)
    helix::ThemeData original_theme_;
    /// Dark mode when the explorer opened; previews flip the live flag.
    bool original_is_dark_{true};
    /// Current preview dark mode state
    bool preview_is_dark_{true};
    /// Cached theme list (populated when explorer opens, avoids re-parsing on every toggle)
    std::vector<helix::ThemeInfo> cached_themes_;

    /// SubjectManager, declared ahead of the subjects it owns so it tears down
    /// after them (names withdraw before storage dies).
    SubjectManager subjects_;

    /// Subject for theme Apply button disabled state (1=disabled, 0=enabled)
    lv_subject_t theme_apply_disabled_subject_{};
};

inline AppearanceSettingsOverlay& get_appearance_settings_overlay() {
    return lazy_global<AppearanceSettingsOverlay>("AppearanceSettingsOverlay");
}

} // namespace helix::settings
