// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"
#include "subject_managed_panel.h"

#include <lvgl.h>

namespace helix {

/**
 * @brief Kinematic envelope from `toolhead.axis_minimum` / `axis_maximum` (mm)
 *
 * Has-bits distinguish "Klipper hasn't sent us this yet" from "Klipper said 0".
 * Only valid for the duration of a connection — reset on reconnect.
 */
struct AxisBounds {
    float x_min = 0.0f, x_max = 0.0f;
    float y_min = 0.0f, y_max = 0.0f;
    float z_min = 0.0f, z_max = 0.0f;
    bool has_x = false;
    bool has_y = false;
    bool has_z = false;
};

/// Shift machine-space bounds into G-code space. machine = gcode + homing_origin
/// (SET_GCODE_OFFSET, saved Z offset, toolchanger tool offsets), so the valid
/// G-code range per axis is [min - origin, max - origin]. Has-bits are untouched.
/// Resolution of the stored gcode_position subjects: centimillimetres,
/// truncated, so a reading sits up to this far below the real position.
inline constexpr double kPositionResolutionMm = 0.01;

/// Clamps stop this far inside the G-code envelope. Klipper adds the offset to
/// a target in doubles and range-checks the sum, so a target exactly at a
/// shifted edge can land a hair past the machine limit and be refused (Z
/// 274.94 + 0.06 on a Snapmaker U1); and a relative jog computed from a
/// truncated reading can overshoot by up to kPositionResolutionMm.
inline constexpr double kGcodeEdgeMarginMm = 2 * kPositionResolutionMm;

/// Shrink each axis's range by `margin` from both ends. Has-bits are untouched.
inline AxisBounds inset_bounds(const AxisBounds& b, double margin) {
    AxisBounds r = b;
    r.x_min = static_cast<float>(b.x_min + margin);
    r.x_max = static_cast<float>(b.x_max - margin);
    r.y_min = static_cast<float>(b.y_min + margin);
    r.y_max = static_cast<float>(b.y_max - margin);
    r.z_min = static_cast<float>(b.z_min + margin);
    r.z_max = static_cast<float>(b.z_max - margin);
    return r;
}

inline AxisBounds to_gcode_space(const AxisBounds& machine, double ox, double oy, double oz) {
    AxisBounds g = machine;
    g.x_min = static_cast<float>(machine.x_min - ox);
    g.x_max = static_cast<float>(machine.x_max - ox);
    g.y_min = static_cast<float>(machine.y_min - oy);
    g.y_max = static_cast<float>(machine.y_max - oy);
    g.z_min = static_cast<float>(machine.z_min - oz);
    g.z_max = static_cast<float>(machine.z_max - oz);
    return g;
}

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

    // Live position accessors - where the nozzle physically is mid-move
    // (centimillimeters, from motion_report.live_position: includes bed mesh
    // and z-offset, updates during moves)
    lv_subject_t* get_live_position_x_subject() {
        return &live_position_x_;
    }
    lv_subject_t* get_live_position_y_subject() {
        return &live_position_y_;
    }
    lv_subject_t* get_live_position_z_subject() {
        return &live_position_z_;
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
    lv_subject_t* get_max_velocity_subject() {
        return &max_velocity_;
    }
    lv_subject_t* get_live_extruder_velocity_subject() {
        return &live_extruder_velocity_;
    }
    lv_subject_t* get_live_velocity_subject() {
        return &live_velocity_;
    }

    // Z-offset accessors (microns)
    lv_subject_t* get_gcode_z_offset_subject() {
        return &gcode_z_offset_;
    }
    lv_subject_t* get_pending_z_offset_delta_subject() {
        return &pending_z_offset_delta_;
    }
    lv_subject_t* get_persisted_z_offset_subject() {
        return &persisted_z_offset_;
    }
    lv_subject_t* get_persisted_z_offset_valid_subject() {
        return &persisted_z_offset_valid_;
    }

    // Pending Z-offset methods
    void add_pending_z_offset_delta(int delta_microns);
    int get_pending_z_offset_delta() const;
    bool has_pending_z_offset_adjustment() const;
    void clear_pending_z_offset_delta();

    /// Kinematic envelope (mm) from toolhead.axis_minimum/axis_maximum.
    /// has_x/y/z is false until the first subscription update arrives.
    [[nodiscard]] AxisBounds get_axis_bounds() const {
        return axis_bounds_;
    }

    /// The envelope in G-code coordinates: the machine envelope shifted by
    /// minus gcode_move.homing_origin. Everything the motion panel compares
    /// against gcode_move.gcode_position (jog clamps, keypad limits, Z-button
    /// blocking) must use these, not the machine bounds.
    [[nodiscard]] AxisBounds get_gcode_axis_bounds() const {
        return to_gcode_space(axis_bounds_, homing_origin_x_, homing_origin_y_, homing_origin_z_);
    }

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

    // Live position subjects (physical position mid-move, motion_report)
    lv_subject_t live_position_x_{};
    lv_subject_t live_position_y_{};
    lv_subject_t live_position_z_{};

    lv_subject_t homed_axes_{};
    char homed_axes_buf_[8]{};

    // Speed/flow subjects
    lv_subject_t speed_factor_{};
    lv_subject_t flow_factor_{};

    // Actual speed/velocity subjects
    lv_subject_t max_velocity_{};           // mm/s (from toolhead.max_velocity)
    lv_subject_t live_extruder_velocity_{}; // centimm/s (from motion_report, ×100 for precision)
    lv_subject_t live_velocity_{};          // mm/s, measured toolhead speed (from motion_report)

    // Z-offset subjects
    lv_subject_t gcode_z_offset_{};
    lv_subject_t pending_z_offset_delta_{};
    // Firmware-persisted z-offset (ZMOD save_variables.gcode_offsets.z). Valid
    // flag is separate because 0 is a legitimate stored offset, so it cannot
    // double as "nothing stored".
    lv_subject_t persisted_z_offset_{};
    lv_subject_t persisted_z_offset_valid_{};

    // Kinematic envelope (not subjects — read on demand by jog clamping etc.)
    AxisBounds axis_bounds_{};

    // gcode_move.homing_origin per axis (mm): machine = gcode + origin
    double homing_origin_x_ = 0.0;
    double homing_origin_y_ = 0.0;
    double homing_origin_z_ = 0.0;
};

} // namespace helix
