// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_save_z_offset_modal.h"

#include "lvgl/lvgl.h"
#include "observer_factory.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

// Forward declarations
class IMoonrakerAPI;
namespace helix {
class PrinterState;
}

/**
 * @file ui_print_tune_overlay.h
 * @brief Tune panel singleton for print speed, flow, and Z-offset adjustment
 *
 * Manages the tune overlay panel that allows adjusting:
 * - Print speed (M220 command)
 * - Flow rate (M221 command)
 * - Z-offset / baby stepping (SET_GCODE_OFFSET command)
 *
 * Accessed via get_print_tune_overlay() singleton. Can be shown from:
 * - PrintStatusPanel (Tune button during active print)
 * - ControlsPanel (Z-Offset row click for calibration)
 *
 * @pattern Lazy singleton with subject management
 * @threading Main thread only (LVGL)
 */
class PrintTuneOverlay : public OverlayBase {
  public:
    PrintTuneOverlay();
    ~PrintTuneOverlay() override;

    // Non-copyable
    PrintTuneOverlay(const PrintTuneOverlay&) = delete;
    PrintTuneOverlay& operator=(const PrintTuneOverlay&) = delete;

    /**
     * @brief Show the tune panel overlay
     *
     * Lazy initialization - creates panel on first call. Handles:
     * - Subject initialization
     * - Panel creation from XML
     * - Standard overlay setup (back button, scrolling)
     * - Pushes onto navigation stack
     *
     * @param parent_screen The parent screen for the overlay
     * @param api IMoonrakerAPI for sending G-code commands
     * @param printer_state Reference to helix::PrinterState for kinematics/values
     */
    void show(lv_obj_t* parent_screen, IMoonrakerAPI* api, helix::PrinterState& printer_state);

    /**
     * @brief Handle reset button click - resets speed/flow to 100%
     */
    void handle_reset();

    /**
     * @brief Handle Z-offset button click (baby stepping)
     * @param delta Z-offset change in mm (negative = closer/more squish)
     */
    void handle_z_offset_changed(double delta);

    /**
     * @brief Handle step amount selection (radio-style buttons)
     * @param idx Step index (0=0.05, 1=0.025, 2=0.01, 3=0.005)
     */
    void handle_z_step_select(int idx);

    /**
     * @brief Handle Z-offset adjust in direction by selected step amount
     * @param direction -1 for closer (more squish), +1 for farther (less squish)
     */
    void handle_z_adjust(int direction);

    /**
     * @brief Handle save Z-offset button click
     * Shows warning modal since SAVE_CONFIG will restart Klipper
     */
    void handle_save_z_offset();

    /**
     * @brief Update Z-offset icons based on printer kinematics
     *
     * Sets appropriate icons for CoreXY (bed moves) vs Cartesian (head moves).
     *
     * @param panel The tune panel widget
     */
    void update_z_offset_icons(lv_obj_t* panel);

    /**
     * @brief Adjust speed by a delta and send G-code
     * @param delta Speed change in percent (e.g. -10, -5, -1, +1, +5, +10)
     */
    void handle_speed_adjust(int delta);

    /**
     * @brief Adjust flow by a delta and send G-code
     * @param delta Flow change in percent (e.g. -10, -5, -1, +1, +5, +10)
     */
    void handle_flow_adjust(int delta);

    /**
     * @brief Update display from current speed/flow values
     *
     * Called by PrintStatusPanel when helix::PrinterState values change.
     *
     * @param speed_percent Current speed percentage
     * @param flow_percent Current flow percentage
     */
    void update_speed_flow_display(int speed_percent, int flow_percent);

    /**
     * @brief Update Z-offset display from helix::PrinterState
     * @param microns Z-offset in microns from helix::PrinterState
     */
    void update_z_offset_display(int microns);

    /**
     * @brief Get the tune panel widget
     * @return Tune panel widget, or nullptr if not set up
     */
    lv_obj_t* get_panel() const {
        return tune_panel_;
    }

    //
    // === OverlayBase Interface ===
    //

    /**
     * @brief Get overlay name for logging
     * @return "Print Tune"
     */
    const char* get_name() const override {
        return "Print Tune";
    }

    /**
     * @brief Initialize subjects (OverlayBase pure virtual)
     *
     * This overlay uses init_subjects_internal() called from show().
     * This method delegates to that implementation.
     */
    void init_subjects() override {
        init_subjects_internal();
    }

    /**
     * @brief Create overlay UI (OverlayBase pure virtual)
     *
     * This overlay uses show() for creation with additional parameters.
     * This method returns nullptr; use show() instead.
     *
     * @param parent Unused - show() provides parent
     * @return nullptr (use show() for proper creation)
     */
    lv_obj_t* create(lv_obj_t* /*parent*/) override {
        return nullptr;
    }

    /**
     * @brief Called when overlay becomes visible
     *
     * Syncs sliders to current printer state values.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being hidden
     */
    void on_deactivate() override;

  private:
    void init_subjects_internal();
    void setup_panel();
    void update_display();
    void sync_to_state();
    void update_actual_speed_display();
    void update_actual_flow_display();

    //
    // === Dependencies ===
    //

    IMoonrakerAPI* api_ = nullptr;
    helix::PrinterState* printer_state_ = nullptr;
    lv_obj_t* tune_panel_ = nullptr;

    //
    // === Subject Management ===
    //

    SubjectManager subjects_;

    // Subjects for reactive UI
    lv_subject_t tune_speed_subject_;
    lv_subject_t tune_flow_subject_;
    lv_subject_t tune_z_offset_subject_;
    lv_subject_t z_step_active_subjects_[4]; ///< Boolean subjects for step button radio styling
    lv_subject_t z_closer_icon_subject_;     ///< Icon name for closer button (kinematic-aware)
    lv_subject_t z_farther_icon_subject_;    ///< Icon name for farther button (kinematic-aware)

    // Actual speed/flow display subjects
    lv_subject_t tune_actual_speed_subject_;
    lv_subject_t tune_actual_flow_subject_;

    // Subject storage buffers
    char tune_speed_buf_[16] = "100%";
    char tune_flow_buf_[16] = "100%";
    char tune_z_offset_buf_[16] = "0.000mm";
    char z_closer_icon_buf_[24] = "arrow_down";
    char z_farther_icon_buf_[24] = "arrow_up";
    char tune_actual_speed_buf_[32] = "";
    char tune_actual_flow_buf_[32] = "";

    //
    // === State ===
    //

    static constexpr double Z_STEP_AMOUNTS[] = {0.05, 0.025, 0.01, 0.005};
    static constexpr int Z_STEP_DEFAULT = 2; ///< Default step: 0.01mm

    /// How far one tuning session may travel from the offset it opened on, in
    /// either direction. The guard bounds the *travel*, not the absolute
    /// offset: printers with several toolheads or nozzles legitimately run
    /// offsets past this, and clamping the absolute value snapped them toward
    /// zero on the first tap and drove the nozzle into the print.
    static constexpr double Z_OFFSET_MAX_SESSION_TRAVEL = 2.0;

    double current_z_offset_ = 0.0;
    /// Offset the overlay opened on; session travel is measured from here.
    double session_base_z_offset_ = 0.0;
    int selected_z_step_idx_ = Z_STEP_DEFAULT;
    int speed_percent_ = 100;
    int flow_percent_ = 100;

    //
    // === Observers ===
    //

    ObserverGuard speed_observer_;
    ObserverGuard gcode_speed_observer_;
    ObserverGuard max_velocity_observer_;
    ObserverGuard extruder_vel_observer_;

    //
    // === Modals ===
    //

    SaveZOffsetModal save_z_offset_modal_;
};

/**
 * @brief Get the singleton PrintTuneOverlay instance
 *
 * Lazy singleton - creates on first access, registers with StaticPanelRegistry
 * for cleanup on shutdown. Used by XML event callbacks and panels that need
 * to show the tuning overlay.
 *
 * @return Reference to the shared PrintTuneOverlay instance
 */
PrintTuneOverlay& get_print_tune_overlay();
