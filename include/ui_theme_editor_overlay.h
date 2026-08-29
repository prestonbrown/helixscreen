// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_theme_editor_overlay.h
 * @brief Theme editor overlay with live preview
 */

#pragma once

#include "ui_color_picker.h"

#include "overlay_base.h"
#include "theme_loader.h"

#include <array>
#include <functional>
#include <memory>

/**
 * @brief Theme editor overlay with live preview
 *
 * Allows editing theme colors and properties with immediate preview.
 * Tracks dirty state and prompts for save on exit.
 */
class ThemeEditorOverlay : public OverlayBase {
  public:
    ThemeEditorOverlay();
    ~ThemeEditorOverlay() override;

    //
    // === OverlayBase Implementation ===
    //

    /**
     * @brief Initialize subjects for XML binding
     *
     * No local subjects needed for initial implementation.
     */
    void init_subjects() override;

    /**
     * @brief Create overlay UI from XML
     * @param parent Parent widget to attach overlay to (usually screen)
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Register XML event callbacks
     *
     * Registers swatch, slider, save, and close button callbacks.
     */
    void register_callbacks() override;

    /**
     * @brief Get human-readable overlay name
     * @return "Theme Editor"
     */
    [[nodiscard]] const char* get_name() const override {
        return "Theme Editor";
    }

    /**
     * @brief Called when overlay becomes visible
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is hidden
     */
    void on_deactivate() override;

    /**
     * @brief Clean up resources for async-safe destruction
     */
    void cleanup() override;

    //
    // === Theme Editor API ===
    //

    /**
     * @brief Check if theme has unsaved changes
     * @return true if there are unsaved modifications
     */
    [[nodiscard]] bool is_dirty() const {
        return dirty_;
    }

    /**
     * @brief Get the theme currently being edited
     * @return Reference to editing theme data
     */
    [[nodiscard]] const helix::ThemeData& get_editing_theme() const {
        return editing_theme_;
    }

    /**
     * @brief Load theme for editing
     * @param filename Theme filename (without .json extension)
     */
    void load_theme(const std::string& filename);

    /**
     * @brief Set which mode (dark/light) to edit
     * @param is_dark true to edit dark palette, false for light palette
     *
     * Call this before load_theme() to match the preview state.
     */
    void set_editing_dark_mode(bool is_dark);

  private:
    void setup_callbacks();
    void update_swatch_colors();
    void update_property_sliders();
    void update_slider_value_label(const char* row_name, int value);
    void update_slider_value_label(const char* row_name, const char* text);
    void mark_dirty();
    void clear_dirty();

    // Static callbacks for XML event_cb registration
    static void on_swatch_clicked(lv_event_t* e);
    static void on_slider_changed(lv_event_t* e);
    static void on_close_requested(lv_event_t* e);
    static void on_back_clicked(lv_event_t* e);

    // Unified slider property callback (uses user_data to identify property)
    static void on_property_changed(lv_event_t* e);

    // Action button callbacks (registered with XML)
    static void on_theme_save_clicked(lv_event_t* e);
    static void on_theme_save_as_clicked(lv_event_t* e);
    static void on_theme_reset_clicked(lv_event_t* e);

    // Save As dialog callbacks
    static void on_save_as_confirm(lv_event_t* e);
    static void on_save_as_cancel(lv_event_t* e);

    // Theme preset dropdown callback
    static void on_theme_preset_changed(lv_event_t* e);

    // Preview button callback
    static void on_theme_preview_clicked(lv_event_t* e);

    // Instance handlers for slider property changes
    void handle_border_radius_changed(int value);
    void handle_border_width_changed(int value);
    void handle_border_opacity_changed(int value);
    void handle_shadow_intensity_changed(int value);

    // Instance handlers for action buttons
    void handle_save_clicked();
    void handle_save_as_clicked();
    void handle_reset_clicked();
    void perform_reset_to_default();

    // Legacy handlers (to be refactored)
    void handle_swatch_click(int palette_index);
    void handle_slider_change(const char* slider_name, int value);
    void show_color_picker(int palette_index);
    void show_save_as_dialog();
    void show_discard_confirmation(std::function<void()> on_discard);
    void update_title_dirty_indicator();
    void handle_back_clicked();

    // Save As dialog handlers
    void handle_save_as_confirm();

    // Theme preset handlers
    void init_theme_preset_dropdown();
    void handle_theme_preset_changed(int index);

    // Preview handler
    void handle_preview_clicked();

    // Filename helpers
    static std::string sanitize_filename(const std::string& name);
    static std::string generate_unique_filename(const std::string& base_name,
                                                const std::string& themes_dir);

    helix::ThemeData editing_theme_;
    helix::ThemeData original_theme_;
    bool dirty_ = false;
    bool editing_dark_mode_ = true; ///< Which palette to edit (true=dark, false=light)

    /** @brief Get the active palette based on editing mode */
    [[nodiscard]] helix::ModePalette& get_active_palette();
    [[nodiscard]] const helix::ModePalette& get_active_palette() const;
    int editing_color_index_ = -1;

    lv_obj_t* panel_ = nullptr;
    std::array<lv_obj_t*, 16> swatch_objects_{};

    // Color picker for swatch editing
    std::unique_ptr<helix::ui::ColorPicker> color_picker_;

    // Discard confirmation dialog tracking
    lv_obj_t* discard_dialog_ = nullptr;
    std::function<void()> pending_discard_action_;

    // Save As dialog tracking
    lv_obj_t* save_as_dialog_ = nullptr;
};

/**
 * @brief Get global ThemeEditorOverlay instance
 * @return Reference to singleton instance (auto-initializes on first access)
 */
ThemeEditorOverlay& get_theme_editor_overlay();
