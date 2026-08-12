// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "belt_tension_calibrator.h"
#include "belt_tension_types.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <memory>
#include <optional>
#include <string>

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
 * - START: Gate result + hardware summary + start button
 * - POSITION: Park the gantry so the free span is nominal, show which run to pluck
 * - LISTEN: Live meter for one belt at a time
 * - COMPARE: A vs B, delta, verdict
 * - ERROR: Error display with retry
 *
 * START's action is gated on `bt_can_start`, which is driven solely by
 * evaluate_belt_gate(). Hiding the Advanced-panel menu row is not a gate: the
 * panel is still reachable by `ctl navigate`, by a deep link, and by a printer
 * whose accelerometer drops out after entry. Binding the button's disabled
 * state to a subject holds in all three cases.
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
        START = 0,    ///< gate result + hardware summary + start button
        POSITION = 1, ///< park the gantry, show which run to pluck
        LISTEN = 2,   ///< live meter for one belt
        COMPARE = 3,  ///< A vs B
        ERROR = 4,
    };

    /// Positioning move, not a print move - the gantry is empty and nothing is
    /// being extruded, so this only has to be quick and undramatic.
    static constexpr double PARK_FEEDRATE_MM_MIN = 3000.0;

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
    void handle_position_confirmed();

  private:
    void set_view_state(ViewState state);
    void on_hardware_detected(const helix::calibration::BeltTensionHardware& hw);
    void on_sweep_complete(const helix::calibration::BeltTensionResult& result);
    void on_error(const std::string& message);
    void populate_results(const helix::calibration::BeltTensionResult& result);

    /// The single place the gate is computed. Nothing else may decide whether
    /// Start is live.
    void refresh_gate();
    /// Attach the gate's subject observers once. Idempotent: the accelerometer
    /// subject is owned by PrinterCapabilitiesState and may not exist yet the
    /// first time this runs, so activation retries.
    void ensure_gate_observers();
    void handle_park_gantry();
    /// Fetch the klippy UDS path from Moonraker and probe co-location.
    void probe_klippy_socket();
    [[nodiscard]] std::optional<float> span_offset_for_current_printer() const;

    // Subject manager for RAII cleanup
    SubjectManager subjects_;

    // Start screen subjects
    lv_subject_t hw_kinematics_subject_{};
    char hw_kinematics_buf_[64] = {};
    lv_subject_t hw_adxl_subject_{};
    char hw_adxl_buf_[64] = {};
    lv_subject_t target_freq_subject_{};
    char target_freq_buf_[32] = {};

    // Gate subjects - START's action is bound to these, not to a hidden menu row
    lv_subject_t can_start_subject_{};
    lv_subject_t gate_message_subject_{};
    char gate_message_buf_[128] = {};

    // Positioning subjects
    lv_subject_t has_target_subject_{};
    lv_subject_t park_status_subject_{};
    char park_status_buf_[64] = {};
    lv_subject_t current_belt_subject_{};
    char current_belt_buf_[8] = {};

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

    // Gate observers. Every one carries PrinterState's own SubjectLifetime -
    // the observe_* factories take it as a defaulted fourth parameter, so
    // omitting it silently leaves the guard tokenless (#705).
    ObserverGuard accel_observer_;
    ObserverGuard print_active_observer_;
    ObserverGuard connected_observer_;
    bool gate_observers_wired_ = false;

    // Klippy's UDS path, from Moonraker's /server/config. Reachability is
    // probed once per activation, not per gate refresh: the gate recomputes on
    // every subject change and a connect() syscall each time would be waste.
    std::string klippy_socket_path_;
    bool klippy_socket_reachable_ = false;

    // Calibrator
    std::unique_ptr<helix::calibration::BeltTensionCalibrator> calibrator_;
    MoonrakerAPI* api_ = nullptr;
    helix::MoonrakerClient* client_ = nullptr;

    // Chart
    ui_frequency_response_chart_t* chart_ = nullptr;
    int chart_series_a_ = -1;
    int chart_series_b_ = -1;

    // Hardware detection cache. Written but not yet read - its readers were
    // the strobe fine-tuning handlers removed with the strobe placeholder;
    // phase 2 (live streaming) needs this again for its own hardware summary.
    helix::calibration::BeltTensionHardware detected_hw_;

    // Last results for re-display. Same phase-2 state as detected_hw_ above:
    // written but not yet read after the strobe handlers were removed.
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
