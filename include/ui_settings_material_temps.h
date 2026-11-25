// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "overlay_base.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"
#include "temperature_controller.h"

#include <string>
#include <vector>

namespace helix::settings {

/**
 * @class MaterialTempsOverlay
 * @brief Overlay for customizing per-material temperature presets
 *
 * Two-view overlay:
 * - List view: all materials grouped by category, showing current temps
 * - Edit view: four number inputs (nozzle min/max, bed, chamber) + preheat macro;
 *   Save sits in the header, Reset at the end of the form
 *
 * Overrides are stored via MaterialSettingsManager and applied transparently
 * in filament::find_material().
 */
class MaterialTempsOverlay : public OverlayBase {
  public:
    // === OverlayBase Interface ===

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Material Temperatures";
    }

    /// Custom create(): caches the list and edit views and intercepts the back button.
    lv_obj_t* create(lv_obj_t* parent) override;

    void on_activate() override;

    // === Event Handlers ===

    void handle_material_row_clicked(const std::string& material_name);
    void handle_save();
    void handle_reset_defaults();
    void handle_back_clicked();

    /// A field of the open material changed: Save enables, Back asks first.
    void mark_dirty();

    /// Reject-toast buffer for the localized range messages (nozzle, bed,
    /// chamber). Must hold the longest locale at the widest bound (ru is the
    /// longest today); test_material_temps_chamber pins that it does.
    static constexpr size_t kToastBufBytes = 128;

  protected:
    /// Every open starts on the list view.
    void before_show() override;

  private:
    void populate_material_list();
    void show_edit_view(const std::string& material_name);
    void show_list_view();
    /// Back to the list, refreshed, at the scroll position it had.
    void return_to_list();
    void set_dirty(bool dirty);
    bool is_dirty();
    /// Back with unsaved edits: Discard returns to the list, Cancel stays.
    void show_discard_confirmation();
    void on_ui_destroyed() override;

    // SubjectManager, declared ahead of the subjects it owns so it tears down
    // after them (names withdraw before storage dies).
    SubjectManager subjects_;

    // Subject for toggling between list/edit views (0=list, 1=edit)
    lv_subject_t editing_subject_{};

    // Subjects for edit view text bindings
    lv_subject_t edit_name_subject_{};
    char edit_name_buf_[64];

    lv_subject_t edit_defaults_subject_{};
    char edit_defaults_buf_[128];

    // Currently edited material name
    std::string editing_material_;

    // Widget refs
    lv_obj_t* list_view_ = nullptr;
    lv_obj_t* edit_view_ = nullptr;

    // Macro dropdown state
    lv_subject_t has_macro_subject_{};   // 0=no macro, 1=has macro (controls toggle visibility)
    lv_subject_t shipped_subject_{};     // 1=editing a shipped type (has a default to reset to)
    lv_subject_t dirty_subject_{};       // 1=the open material has unsaved edits
    lv_subject_t save_hidden_subject_{}; // 1=no material open: the header's Save hides
    lv_obj_t* discard_dialog_ = nullptr;
    lv_obj_t* macro_dropdown_ = nullptr;
    lv_obj_t* macro_heating_switch_ = nullptr;
    std::vector<std::string> macro_names_; // Parallel to dropdown options (index 0 = "None")

    void populate_macro_dropdown();
    void handle_macro_dropdown_changed();

    // === Static Callbacks ===

    static void on_material_row_clicked(lv_event_t* e);
    static void on_back_clicked(lv_event_t* e);
};

/**
 * @brief Global instance accessor (lazy singleton with StaticPanelRegistry cleanup)
 */
inline MaterialTempsOverlay& get_material_temps_overlay() {
    return lazy_global<MaterialTempsOverlay>("MaterialTempsOverlay");
}

} // namespace helix::settings
