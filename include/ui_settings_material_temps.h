// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "core/static_panel_registry.h"
#include "core/subject.h"
#include "overlay_base.h"
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
    bool destroy_on_close() const override {
        return true;
    }

    /// Custom create(): caches the list and edit views. The back button's handler comes
    /// from the XML root's back_button_callback (on_material_back_clicked).
    lv_obj_t* create(lv_obj_t* parent) override;

    void on_activate() override;

    // === Event Handlers ===

    void handle_material_row_clicked(const std::string& material_name);
    void handle_save();
    void handle_reset_defaults();
    void handle_back_clicked();
    /// The switch beside the macro field changed: the field's name and Save follow.
    void handle_macro_heating_changed(lv_event_t* e);

    /// Open on @p material_name's edit view instead of the list, for a caller
    /// that was asked for that material's setting.
    void show_material(lv_obj_t* parent_screen, const std::string& material_name);

    /// A field of the open material changed: Save enables, Back asks first.
    void mark_dirty();

    /// Reject-toast buffer for the localized range messages (nozzle, bed,
    /// chamber). Must hold the longest locale at the widest bound (ru is the
    /// longest today); test_material_temps_chamber pins that it does.
    static constexpr size_t kToastBufBytes = 128;

  protected:
    /// Every open starts on the list view, or on the material show_material() named.
    void before_show() override;

  private:
    void populate_material_list();
    void show_edit_view(const std::string& material_name);
    void show_list_view();
    /// Back to the list, refreshed, at the scroll position it had.
    void return_to_list();
    void set_dirty(bool dirty);
    bool is_dirty() const;
    /// Back with unsaved edits: Discard returns to the list, Cancel stays.
    void show_discard_confirmation();

    // SubjectManager, declared ahead of the subjects it owns so it tears down
    // after them (names withdraw before storage dies).
    SubjectManager subjects_;

    // Subject for toggling between list/edit views (0=list, 1=edit)
    helix::Subject editing_subject_{};

    // Subjects for edit view text bindings
    helix::Subject edit_name_subject_{};
    char edit_name_buf_[64];

    helix::Subject edit_defaults_subject_{};
    char edit_defaults_buf_[128];

    // Currently edited material name
    std::string editing_material_;
    // The material the next show() opens on; empty for the list.
    std::string opening_material_;

    // Widget refs
    lv_obj_t* list_view_ = nullptr;
    lv_obj_t* edit_view_ = nullptr;

    // Macro dropdown state
    helix::Subject has_macro_subject_{}; // 0=no macro, 1=has macro (controls toggle visibility)
    // 1 while the open material's macro handles its heating: the switch beside
    // the macro field and the field's name both bind to it.
    helix::Subject macro_heats_subject_{};
    helix::Subject shipped_subject_{}; // 1=editing a shipped type (has a default to reset to)
    helix::Subject dirty_subject_{};   // 1=the open material has unsaved edits
    helix::ui::WidgetRef discard_dialog_;
    lv_obj_t* macro_dropdown_ = nullptr;

    void on_ui_destroyed() override;
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
