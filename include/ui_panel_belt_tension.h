// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "belt_tension_calibrator.h"
#include "belt_tension_types.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <memory>

class MoonrakerAPI;

namespace helix {
class MoonrakerClient;
}

// Forward declare
struct ui_frequency_response_chart_t;

/**
 * @file ui_panel_belt_tension.h
 * @brief Belt tension tuning panel for CoreXY/Cartesian belt frequency measurement
 *
 * Interactive overlay that guides users through belt tension measurement:
 * - Detects hardware (kinematics, accelerometer)
 * - Runs frequency sweeps on belt paths A and B
 * - Displays resonant frequency comparison and recommendations
 *
 * ## State Machine:
 * - START: Hardware summary + start button
 * - PROGRESS: Testing in progress with progress bar
 * - RESULTS: Show measurements, delta, similarity, recommendation
 * - ERROR: Error display with retry
 *
 * ## Usage:
 * ```cpp
 * auto& panel = get_global_belt_tension_panel();
 * panel.init_subjects();
 * panel.create(screen);
 * panel.show();
 * ```
 */
class BeltTensionPanel : public OverlayBase {
  public:
    enum class ViewState {
        START = 0,    ///< Hardware summary + start button
        PROGRESS = 1, ///< Testing in progress
        RESULTS = 2,  ///< Show measurements + chart
        ERROR = 3,    ///< Error display
    };

    BeltTensionPanel() = default;
    ~BeltTensionPanel() override;

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void deinit_subjects();
    lv_obj_t* create(lv_obj_t* parent) override;

    const char* get_name() const override {
        return "Belt Tension";
    }

    void on_activate() override;
    void on_deactivate() override;
    void cleanup() override;
    void on_ui_destroyed() override;

    //
    // === Public API ===
    //

    void show();
    void set_api(helix::MoonrakerClient* client, MoonrakerAPI* api);

    //
    // === Event Handlers (public for XML callbacks) ===
    //

    void handle_start_clicked();
    void handle_cancel_clicked();
    void handle_retry_clicked();

  private:
    void set_view_state(ViewState state);
    void on_hardware_detected(const helix::calibration::BeltTensionHardware& hw);
    void on_sweep_complete(const helix::calibration::BeltTensionResult& result);
    void on_error(const std::string& message);
    void populate_results(const helix::calibration::BeltTensionResult& result);

    // Subject manager for RAII cleanup
    SubjectManager subjects_;

    // Start screen subjects
    lv_subject_t hw_kinematics_subject_{};
    char hw_kinematics_buf_[64] = {};
    lv_subject_t hw_adxl_subject_{};
    char hw_adxl_buf_[64] = {};
    lv_subject_t target_freq_subject_{};
    char target_freq_buf_[32] = {};

    // Progress subjects
    lv_subject_t progress_subject_{};
    lv_subject_t progress_label_subject_{};
    char progress_label_buf_[64] = {};

    // Result subjects
    lv_subject_t result_a_freq_subject_{};
    char result_a_freq_buf_[32] = {};
    lv_subject_t result_a_status_subject_{};
    char result_a_status_buf_[32] = {};
    lv_subject_t result_b_freq_subject_{};
    char result_b_freq_buf_[32] = {};
    lv_subject_t result_b_status_subject_{};
    char result_b_status_buf_[32] = {};
    lv_subject_t result_delta_subject_{};
    char result_delta_buf_[64] = {};
    lv_subject_t result_similarity_subject_{};
    char result_similarity_buf_[32] = {};
    lv_subject_t result_recommendation_subject_{};
    char result_recommendation_buf_[256] = {};
    lv_subject_t has_results_subject_{};

    // Error subject
    lv_subject_t error_message_subject_{};
    char error_message_buf_[256] = {};

    // Calibrator
    std::unique_ptr<helix::calibration::BeltTensionCalibrator> calibrator_;
    MoonrakerAPI* api_ = nullptr;
    helix::MoonrakerClient* client_ = nullptr;

    // Chart
    ui_frequency_response_chart_t* chart_ = nullptr;
    int chart_series_a_ = -1;
    int chart_series_b_ = -1;

    // Hardware detection cache
    helix::calibration::BeltTensionHardware detected_hw_;

    // Last results for re-display
    helix::calibration::BeltTensionResult last_result_;
};

// Global instance accessor
BeltTensionPanel& get_global_belt_tension_panel();

/**
 * @brief Register XML event callbacks for belt tension panel
 *
 * Call once at startup before creating any panel_belt_tension XML.
 * Registers callbacks for all button events and initializes subjects.
 */
void ui_panel_belt_tension_register_callbacks();

/**
 * @brief Initialize row click callback for opening from Advanced panel
 *
 * Registers "on_belt_tension_row_clicked" callback.
 */
void init_belt_tension_row_handler();
