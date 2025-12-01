// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_panel_base.h"

#include "moonraker_domain_service.h" // For BedMeshProfile

#include <vector>

/**
 * @brief Bed mesh visualization panel with TinyGL 3D renderer
 *
 * Interactive 3D visualization of printer bed mesh height maps with touch-drag
 * rotation, color-coded height mapping, profile switching, and statistics.
 *
 * Unlike most panels, the TinyGL renderer is imperative (not reactive) for
 * performance. LVGL subjects are used only for info labels.
 *
 * @see ui_bed_mesh.h for TinyGL widget API
 */

class BedMeshPanel : public PanelBase {
  public:
    /**
     * @brief Construct BedMeshPanel with injected dependencies
     * @param printer_state Reference to PrinterState
     * @param api Pointer to MoonrakerAPI (for profile loading)
     */
    BedMeshPanel(PrinterState& printer_state, MoonrakerAPI* api);
    ~BedMeshPanel() override;

    void init_subjects() override;
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;
    const char* get_name() const override { return "Bed Mesh Panel"; }
    const char* get_xml_component_name() const override { return "bed_mesh_panel"; }

    /**
     * @brief Load mesh data and render
     * @param mesh_data 2D vector of height values (row-major order)
     */
    void set_mesh_data(const std::vector<std::vector<float>>& mesh_data);

    /** @brief Force redraw of bed mesh visualization */
    void redraw();

  private:
    lv_subject_t bed_mesh_available_;
    lv_subject_t bed_mesh_profile_name_;
    lv_subject_t bed_mesh_dimensions_;
    lv_subject_t bed_mesh_z_range_;
    lv_subject_t bed_mesh_variance_;

    char profile_name_buf_[64];
    char dimensions_buf_[64];
    char z_range_buf_[96];
    char variance_buf_[64];

    lv_obj_t* canvas_ = nullptr;
    lv_obj_t* profile_dropdown_ = nullptr;

    void setup_profile_dropdown();
    void setup_moonraker_subscription();
    void on_mesh_update_internal(const BedMeshProfile& mesh);
    void update_info_subjects(const std::vector<std::vector<float>>& mesh_data, int cols, int rows);

    static void on_panel_delete(lv_event_t* e);
    static void on_profile_dropdown_changed(lv_event_t* e);
};

// Global instance accessor (needed by main.cpp)
BedMeshPanel& get_global_bed_mesh_panel();
