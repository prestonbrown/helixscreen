// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/observer_factory.h"
#include "core/static_panel_registry.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"
#include "z_offset_utils.h"

// Forward declarations
namespace helix {
class IMoonrakerAPI;
class PrinterState;
} // namespace helix

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
    ~PrintTuneOverlay() override;

    /**
     * @brief Show the tune panel overlay
     *
     * Stores the dependencies, then OverlayBase::show() builds and pushes it.
     *
     * @param parent_screen The parent screen for the overlay
     * @param api IMoonrakerAPI for sending G-code commands
     * @param printer_state Reference to helix::PrinterState for kinematics/values
     */
    void show(lv_obj_t* parent_screen, helix::IMoonrakerAPI* api,
              helix::PrinterState& printer_state);

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

    /// Pick which z-offset the direction buttons drive: 0 machine-wide, 1 the
    /// active tool's own.
    void handle_z_target_select(int target);

    /// Apply @p delta mm to the ACTIVE TOOL's own z-offset. Separate from
    /// handle_z_offset_changed(): a different store, a different command, and
    /// no session travel guard — that guard bounds baby-stepping away from the
    /// value a print started with, which is a machine-wide notion.
    void handle_tool_z_offset_changed(double delta);

    /**
     * @brief Handle Z-offset adjust in direction by selected step amount
     * @param direction -1 for closer (more squish), +1 for farther (less squish)
     */
    void handle_z_adjust(int direction);

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

    /// Nudge the active extruder's pressure advance by @p steps of the PA step.
    void handle_pa_adjust(int steps);

    /// Open the pressure-advance tuner, when a tuning print is running.
    void handle_open_pa_tuner();

    /// Re-render the PA readout from the printer's reported value.
    void update_pa_display();

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

    /// Refresh the tool button's label and the muted "other target" value from
    /// ToolState. Cheap, and driven by observers rather than polled.
    void update_tool_z_displays();

    /**
     * @brief Get the tune panel widget
     * @return Tune panel widget, or nullptr if not set up
     */
    lv_obj_t* get_panel() const {
        return overlay_root_;
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

    const char* xml_component() const override {
        return "print_tune_panel";
    }
    // Keeps its tree: opened during a print on slow boards, where rebuild cost is unmeasured.
    bool destroy_on_close() const override {
        return false;
    }

    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Called when overlay becomes visible
     *
     * Syncs sliders to current printer state values.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being hidden
     */
    void on_deactivating(DeactivateReason reason) override;

  private:
    void setup_panel();
    /// Renders the speed/flow headlines and their live readouts, led by
    /// whichever units the speed/flow preference picks.
    void update_display();
    void sync_to_state();

    //
    // === Dependencies ===
    //

    helix::IMoonrakerAPI* api_ = nullptr;

    helix::PrinterState* printer_state_ = nullptr;

    //
    // === Subject Management ===
    //

    SubjectManager subjects_;

    // Subjects for reactive UI
    lv_subject_t tune_speed_subject_{};
    lv_subject_t tune_flow_subject_{};
    lv_subject_t tune_z_offset_subject_{};
    /// Which z-step amount is selected, as an index. ONE int, not four
    /// booleans: the buttons are mutually exclusive by construction, so four
    /// parallel flags were four chances for the UI to disagree with itself,
    /// and every selection wrote all four. The XML compares against it with
    /// bind_style_if (CLAUDE.md rule 7).
    lv_subject_t z_step_selected_subject_{};

    /// Which z-offset the direction buttons drive: 0 machine-wide, 1 the active
    /// tool's own. Only meaningful on a tool changer — see helix::tool_offsets.
    lv_subject_t z_tune_target_subject_{};
    /// The target the user is NOT on, formatted for the muted context label.
    lv_subject_t tune_z_other_subject_{};
    /// Label for the tool button ("T1"). Not tool_badge_text: that one is
    /// deliberately index-only, sized for a disc overlaid on the nozzle glyph.
    lv_subject_t tune_z_tool_label_subject_{};
    lv_subject_t z_closer_icon_subject_{};  ///< Icon name for closer button (kinematic-aware)
    lv_subject_t z_farther_icon_subject_{}; ///< Icon name for farther button (kinematic-aware)

    /// The active extruder's reported K ("0.0420"), and whether a tuning
    /// print is running that the tuner could be reopened for.
    lv_subject_t tune_pa_subject_{};
    char tune_pa_buf_[16] = "--";
    lv_subject_t tune_pa_session_subject_{};

    // Actual speed/flow display subjects
    lv_subject_t tune_actual_speed_subject_{};
    lv_subject_t tune_actual_flow_subject_{};

    // Subject storage buffers
    char tune_speed_buf_[32] = "100%";
    char tune_flow_buf_[32] = "100%";
    char tune_z_offset_buf_[16] = "0.000mm";
    char z_closer_icon_buf_[24] = "arrow_down";
    char z_farther_icon_buf_[24] = "arrow_up";
    char tune_actual_speed_buf_[32] = "";
    char tune_actual_flow_buf_[32] = "";
    // Carries a translated position label and its offset value together
    // ("Печатающая головка 16 -0.150mm"), so it needs more room than the
    // label-only buffer below.
    char tune_z_other_buf_[96] = "";
    // Holds a translated position label ("Tool 1", "Печатающая головка 16") —
    // sized like current_slot_text_buf_/layer_text_buf_ elsewhere, since a
    // translated noun plus a two-digit number can run well past ASCII length.
    char tune_z_tool_label_buf_[64] = "Tool 1";

    //
    // === State ===
    //

    // Z-offset step table and travel guard live in helix::zoffset
    // (z_offset_utils.h): kZStepAmountsMm/kZStepDefaultIndex and
    // kZOffsetMaxSessionTravelMm.

    double current_z_offset_ = 0.0;
    /// Offset the overlay opened on; session travel is measured from here.
    double session_base_z_offset_ = 0.0;
    int selected_z_step_idx_ = helix::zoffset::kZStepDefaultIndex;
    int speed_percent_ = 100;
    int flow_percent_ = 100;

    //
    // === Observers ===
    //

    ObserverGuard speed_observer_;
    ObserverGuard tools_version_observer_;
    ObserverGuard active_tool_observer_;
    ObserverGuard live_velocity_observer_;
    ObserverGuard max_velocity_observer_;
    ObserverGuard extruder_vel_observer_;
    ObserverGuard units_observer_;
    ObserverGuard pa_observer_;
};

/// Lazy singleton; shared by the XML event callbacks and the panels that open it.
inline PrintTuneOverlay& get_print_tune_overlay() {
    return helix::lazy_global<PrintTuneOverlay>("PrintTuneOverlay");
}
