// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "subject_managed_panel.h"

#include <lvgl.h>

#include "hv/json.hpp"

namespace helix {

/**
 * @brief Z-offset calibration strategy — determines gcode commands for calibration and save
 *
 * Different printers need different approaches to calibrate and persist Z-offset.
 * ForgeX-mod printers use SET_GCODE_OFFSET (auto-persisted by mod macro).
 * Standard Klipper uses PROBE_CALIBRATE -> ACCEPT -> SAVE_CONFIG.
 * Endstop printers use Z_ENDSTOP_CALIBRATE -> ACCEPT -> SAVE_CONFIG.
 */
enum class ZOffsetCalibrationStrategy {
    PROBE_CALIBRATE, ///< Standard Klipper: PROBE_CALIBRATE -> ACCEPT -> SAVE_CONFIG
    GCODE_OFFSET,    ///< ForgeX mod: G28 -> move -> G1 adjustments -> SET_GCODE_OFFSET
    ENDSTOP ///< Endstop: Z_ENDSTOP_CALIBRATE -> ACCEPT -> Z_OFFSET_APPLY_ENDSTOP -> SAVE_CONFIG
};

/**
 * @brief Manages motion-related subjects for printer state
 *
 * Extracted from PrinterState as part of god class decomposition.
 *
 * Position storage (all in centimillimeters, use from_centimm() for mm):
 * - position_x/y/z: toolhead.position - actual physical position (includes mesh compensation)
 * - gcode_position_x/y/z: gcode_move.position - commanded position (what user requested)
 *
 * Z-offset stored as microns.
 */
class PrinterMotionState {
  public:
    PrinterMotionState() = default;
    ~PrinterMotionState() = default;

    // Non-copyable
    PrinterMotionState(const PrinterMotionState&) = delete;
    PrinterMotionState& operator=(const PrinterMotionState&) = delete;

    /**
     * @brief Initialize motion subjects
     * @param register_xml If true, register subjects with LVGL XML system
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects (called by SubjectManager automatically)
     */
    void deinit_subjects();

    /**
     * @brief Update motion state from Moonraker status JSON
     * @param status JSON object containing "toolhead" and/or "gcode_move" keys
     */
    void update_from_status(const nlohmann::json& status);

    // Toolhead position accessors - actual physical position (centimillimeters)
    lv_subject_t* get_position_x_subject() {
        return &position_x_;
    }
    lv_subject_t* get_position_y_subject() {
        return &position_y_;
    }
    lv_subject_t* get_position_z_subject() {
        return &position_z_;
    }

    // Gcode position accessors - commanded position (centimillimeters)
    lv_subject_t* get_gcode_position_x_subject() {
        return &gcode_position_x_;
    }
    lv_subject_t* get_gcode_position_y_subject() {
        return &gcode_position_y_;
    }
    lv_subject_t* get_gcode_position_z_subject() {
        return &gcode_position_z_;
    }

    lv_subject_t* get_homed_axes_subject() {
        return &homed_axes_;
    }

    // Speed/flow factor accessors (percentage, 100 = 100%)
    lv_subject_t* get_speed_factor_subject() {
        return &speed_factor_;
    }
    lv_subject_t* get_flow_factor_subject() {
        return &flow_factor_;
    }

    // Actual speed/velocity accessors
    lv_subject_t* get_gcode_speed_subject() {
        return &gcode_speed_;
    }
    lv_subject_t* get_max_velocity_subject() {
        return &max_velocity_;
    }
    lv_subject_t* get_live_extruder_velocity_subject() {
        return &live_extruder_velocity_;
    }

    // Z-offset accessors (microns)
    lv_subject_t* get_gcode_z_offset_subject() {
        return &gcode_z_offset_;
    }
    lv_subject_t* get_pending_z_offset_delta_subject() {
        return &pending_z_offset_delta_;
    }

    // Pending Z-offset methods
    void add_pending_z_offset_delta(int delta_microns);
    int get_pending_z_offset_delta() const;
    bool has_pending_z_offset_adjustment() const;
    void clear_pending_z_offset_delta();

    /**
     * @brief Get computed subject for save Z-offset button visibility
     *
     * Returns 1 when button should be visible (strategy != GCODE_OFFSET AND gcode_z_offset != 0).
     * Use with bind_flag_if_eq in XML: hidden="true" + ref_value="0" = visible when subject is 1.
     */
    lv_subject_t* get_z_offset_save_button_visible_subject() {
        return &z_offset_save_button_visible_;
    }

    /**
     * @brief Update the save button visibility subject
     *
     * Called when either z_offset_calibration_strategy or gcode_z_offset changes.
     * Button is visible (1) when strategy != GCODE_OFFSET AND gcode_z_offset != 0.
     */
    void update_z_offset_save_button_visibility(ZOffsetCalibrationStrategy strategy);

  private:
    friend class PrinterMotionStateTestAccess;

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    // Toolhead position subjects (actual physical position)
    lv_subject_t position_x_{};
    lv_subject_t position_y_{};
    lv_subject_t position_z_{};

    // Gcode position subjects (commanded position)
    lv_subject_t gcode_position_x_{};
    lv_subject_t gcode_position_y_{};
    lv_subject_t gcode_position_z_{};

    lv_subject_t homed_axes_{};
    char homed_axes_buf_[8]{};

    // Speed/flow subjects
    lv_subject_t speed_factor_{};
    lv_subject_t flow_factor_{};

    // Actual speed/velocity subjects
    lv_subject_t gcode_speed_{};            // mm/s (from gcode_move.speed)
    lv_subject_t max_velocity_{};           // mm/s (from toolhead.max_velocity)
    lv_subject_t live_extruder_velocity_{}; // centimm/s (from motion_report, ×100 for precision)

    // Z-offset subjects
    lv_subject_t gcode_z_offset_{};
    lv_subject_t pending_z_offset_delta_{};
    lv_subject_t z_offset_save_button_visible_{}; // Computed: 1 if visible
};

} // namespace helix
