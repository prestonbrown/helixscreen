// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "klipper_config_editor.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <lvgl.h>
#include <memory>
#include <string>

class IMoonrakerAPI;

/**
 * @file ui_probe_overlay.h
 * @brief Probe management overlay with type-specific controls
 *
 * Provides a dedicated overlay for probe management:
 * - Header showing probe identity, type, and status
 * - Type-specific control panel (BLTouch, Cartographer, Beacon, etc.)
 * - Universal actions (accuracy test, z-offset calibration, bed mesh)
 *
 * ## Architecture:
 *
 * The overlay uses a swappable type-specific panel approach. On activation,
 * it checks the detected probe type from ProbeSensorManager and loads
 * the appropriate component XML into the `probe_type_panel` container.
 *
 * ## Usage:
 * ```cpp
 * ProbeOverlay& overlay = get_global_probe_overlay();
 * overlay.init_subjects();  // Once at startup
 * overlay.create(screen);   // Lazy create
 * overlay.show();           // Opens overlay
 * ```
 */
class ProbeOverlay : public OverlayBase {
  public:
    ProbeOverlay() = default;
    ~ProbeOverlay() override;

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    lv_obj_t* create(lv_obj_t* parent) override;

    const char* get_name() const override {
        return "Probe";
    }

    void on_activate() override;
    void on_deactivate() override;
    void cleanup() override;

    //
    // === Public API ===
    //

    void show();
    void set_api(IMoonrakerAPI* api);

    //
    // === Event Handlers (public for XML event_cb callbacks) ===
    //

    void handle_probe_accuracy();
    void handle_zoffset_cal();
    void handle_bed_mesh();

    /// Show probe accuracy results in a modal
    void show_accuracy_results(const std::string& results_line);

    /// Close the accuracy results modal
    void handle_accuracy_close();

    /// Emergency stop during probe accuracy test
    void handle_accuracy_estop();

    /// Open edit modal for a specific config field
    void handle_config_edit(const std::string& field_key, const std::string& title,
                            const std::string& description);

    /// Save the currently-edited config value via KlipperConfigEditor
    void handle_config_save();

    /// Cancel config edit modal
    void handle_config_cancel();

  private:
    // Subject manager for RAII cleanup
    SubjectManager subjects_;

    // Display subjects
    char probe_display_name_buf_[48] = {};
    lv_subject_t probe_display_name_{};
    char probe_type_label_buf_[64] = {};
    lv_subject_t probe_type_label_{};
    char probe_z_offset_display_buf_[32] = {};
    lv_subject_t probe_z_offset_display_{};

    // State subject for overlay mode
    lv_subject_t probe_overlay_state_{};

    // Cartographer subjects
    char probe_carto_coil_temp_buf_[32] = {};
    lv_subject_t probe_carto_coil_temp_{};

    // Beacon subjects
    char probe_beacon_temp_buf_[32] = {};
    lv_subject_t probe_beacon_temp_{};
    char probe_beacon_temp_comp_status_buf_[32] = {};
    lv_subject_t probe_beacon_temp_comp_status_{};

    // Klicky subject (1 if klicky type detected, 0 otherwise)
    lv_subject_t probe_is_klicky_{};

    // Config display subjects (current values shown in setting rows)
    lv_subject_t probe_config_loaded_{}; // 1 when config values are loaded
    char probe_cfg_x_offset_buf_[32] = {};
    lv_subject_t probe_cfg_x_offset_{};
    char probe_cfg_y_offset_buf_[32] = {};
    lv_subject_t probe_cfg_y_offset_{};
    char probe_cfg_samples_buf_[16] = {};
    lv_subject_t probe_cfg_samples_{};
    char probe_cfg_speed_buf_[32] = {};
    lv_subject_t probe_cfg_speed_{};
    char probe_cfg_retract_dist_buf_[32] = {};
    lv_subject_t probe_cfg_retract_dist_{};
    char probe_cfg_tolerance_buf_[32] = {};
    lv_subject_t probe_cfg_tolerance_{};

    // Config edit modal subjects
    char probe_config_edit_title_buf_[64] = {};
    lv_subject_t probe_config_edit_title_{};
    char probe_config_edit_desc_buf_[128] = {};
    lv_subject_t probe_config_edit_desc_{};
    char probe_config_edit_current_buf_[32] = {};
    lv_subject_t probe_config_edit_current_{};
    char probe_config_edit_value_buf_[32] = {};
    lv_subject_t probe_config_edit_value_{};

    // Probe accuracy state machine: 0=idle, 1=probing, 2=results, 3=error
    lv_subject_t probe_acc_state_{};

    // Probe accuracy progress (probing state)
    lv_subject_t probe_acc_progress_{}; // 0-100 percentage
    char probe_acc_progress_text_buf_[64] = {};
    lv_subject_t probe_acc_progress_text_{};

    // Probe accuracy results subjects
    char probe_acc_maximum_buf_[32] = {};
    lv_subject_t probe_acc_maximum_{};
    char probe_acc_minimum_buf_[32] = {};
    lv_subject_t probe_acc_minimum_{};
    char probe_acc_range_buf_[32] = {};
    lv_subject_t probe_acc_range_{};
    char probe_acc_average_buf_[32] = {};
    lv_subject_t probe_acc_average_{};
    char probe_acc_median_buf_[32] = {};
    lv_subject_t probe_acc_median_{};
    char probe_acc_stddev_buf_[32] = {};
    lv_subject_t probe_acc_stddev_{};

    // Quality assessment: 1 = good (range < 0.01), 0 = poor
    lv_subject_t probe_acc_quality_{};
    char probe_acc_quality_text_buf_[64] = {};
    lv_subject_t probe_acc_quality_text_{};

    // Error message
    char probe_acc_error_msg_buf_[128] = {};
    lv_subject_t probe_acc_error_msg_{};

    lv_obj_t* accuracy_modal_ = nullptr;

    // Track samples during probing
    int probe_acc_sample_count_ = 0;
    int probe_acc_total_samples_ = 10;

    // Gcode response handler name (for unregistering)
    std::string probe_acc_handler_name_;

    // Config editor
    helix::system::KlipperConfigEditor config_editor_;
    std::string editing_field_key_; // Current field being edited
    std::string probe_section_;     // Config section name (e.g., "probe", "bltouch")
    lv_obj_t* edit_modal_ = nullptr;

    // Widget/client references
    lv_obj_t* parent_screen_ = nullptr;
    IMoonrakerAPI* api_ = nullptr;

    // Type-specific panel container
    lv_obj_t* type_panel_container_ = nullptr;

    // Transition accuracy modal to error state (call from UI thread only)
    void set_accuracy_error(const std::string& msg);

    // Load type-specific panel based on detected probe type
    void load_type_panel();

    // Update display subjects from ProbeSensorManager
    void update_display_subjects();

    // Load probe config values from Klipper via query_configfile
    void load_config_values();

    // Get the config section name for the current probe type
    std::string get_probe_config_section() const;
};

// Global instance accessor
ProbeOverlay& get_global_probe_overlay();

/**
 * @brief Register XML event callbacks for probe overlay
 *
 * Call once at startup before creating any probe_overlay XML.
 */
void ui_probe_overlay_register_callbacks();

/**
 * @brief Initialize row click callback for opening from Advanced panel
 *
 * Registers "on_probe_row_clicked" callback.
 */
void init_probe_row_handler();
