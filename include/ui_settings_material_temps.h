// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"
#include "temperature_controller.h"

#include <string>

namespace helix::settings {

/**
 * @class MaterialTempsOverlay
 * @brief Overlay for customizing per-material temperature presets
 *
 * Two-view overlay:
 * - List view: all materials grouped by category, showing current temps
 * - Edit view: four number inputs (nozzle min/max, bed, chamber) + save/reset
 *
 * Overrides are stored via MaterialSettingsManager and applied transparently
 * in filament::find_material().
 */
class MaterialTempsOverlay : public OverlayBase {
  public:
    MaterialTempsOverlay();
    ~MaterialTempsOverlay() override;

    // === OverlayBase Interface ===

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Material Temperatures";
    }

    lv_obj_t* create(lv_obj_t* parent) override;
    void show(lv_obj_t* parent_screen);

    void on_activate() override;

    // === Event Handlers (public for static callbacks) ===

    void handle_material_row_clicked(const std::string& material_name);
    void handle_save();
    void handle_reset_defaults();
    void handle_back_clicked();

    /// Reject-toast buffer for the localized range messages (nozzle, bed,
    /// chamber). Must hold the longest locale at the widest cap (ru is the
    /// longest today); test_material_temps_chamber pins that it does.
    static constexpr size_t kToastBufBytes = 128;

  private:
    void populate_material_list();
    void show_edit_view(const std::string& material_name);
    void show_list_view();

    /// Effective input ceiling (°C) for one heater column: the shared
    /// keypad-ceiling authority when a controller is registered, otherwise the
    /// input's absolute ceiling. handle_save() and the cap hints both ask
    /// this, so the view can never diverge from what a send will apply.
    int input_cap(HeaterType type, int abs_max_c);

    /// Set or clear one heater's cap-hint subjects: the gate subject carries
    /// the cap (0 = nothing to surface), the text subject the formatted line —
    /// the XML evaluator cannot format ints. The hint shows only when the
    /// printer's ceiling is tighter than the input's own absolute maximum.
    void update_cap_hint(lv_subject_t& gate, lv_subject_t& text, char* text_buf, const char* format,
                         HeaterType type, int abs_max_c);

    /// Cap-hint text buffers: the longest locale's formatted hint (ru) plus
    /// the widest cap's digits, with room to spare.
    static constexpr size_t kCapHintBufBytes = 96;

    // SubjectManager, declared ahead of the subjects it owns so it tears down
    // after them (names withdraw before storage dies).
    SubjectManager subjects_;

    // Subject for toggling between list/edit views (0=list, 1=edit)
    lv_subject_t editing_subject_;

    // Subjects for edit view text bindings
    lv_subject_t edit_name_subject_;
    char edit_name_buf_[64];

    lv_subject_t edit_defaults_subject_;
    char edit_defaults_buf_[128];

    // Effective ceiling (°C) from TemperatureController when tighter than the
    // input's own absolute maximum, 0 when there is nothing to surface. Drives
    // each cap hint's visibility; the parallel string subject carries the
    // formatted line (the XML evaluator cannot format ints).
    lv_subject_t nozzle_cap_subject_;
    lv_subject_t nozzle_cap_text_subject_;
    char nozzle_cap_text_buf_[kCapHintBufBytes];
    lv_subject_t bed_cap_subject_;
    lv_subject_t bed_cap_text_subject_;
    char bed_cap_text_buf_[kCapHintBufBytes];
    lv_subject_t chamber_cap_subject_;
    lv_subject_t chamber_cap_text_subject_;
    char chamber_cap_text_buf_[kCapHintBufBytes];

    // Currently edited material name
    std::string editing_material_;

    // Widget refs
    lv_obj_t* list_view_ = nullptr;
    lv_obj_t* edit_view_ = nullptr;

    // Macro dropdown state
    lv_subject_t has_macro_subject_; // 0=no macro, 1=has macro (controls toggle visibility)
    lv_obj_t* macro_dropdown_ = nullptr;
    lv_obj_t* macro_heating_switch_ = nullptr;
    std::vector<std::string> macro_names_; // Parallel to dropdown options (index 0 = "None")

    void populate_macro_dropdown();
    void handle_macro_dropdown_changed();

    // === Static Callbacks ===

    static void on_material_row_clicked(lv_event_t* e);
    static void on_material_save(lv_event_t* e);
    static void on_material_reset_defaults(lv_event_t* e);
    static void on_back_clicked(lv_event_t* e);
    static void on_macro_dropdown_changed(lv_event_t* e);
};

/**
 * @brief Global instance accessor (lazy singleton with StaticPanelRegistry cleanup)
 */
MaterialTempsOverlay& get_material_temps_overlay();

} // namespace helix::settings
