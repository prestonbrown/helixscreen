// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_motion_state.cpp
 * @brief Motion state management (a PrinterState sub-state)
 *
 * Manages position, speed/flow factors, and Z-offset subjects.
 */

#include "printer_motion_state.h"

#include "json_utils.h"
#include "state/subject_macros.h"
#include "unit_conversions_json.h"
#include "z_offset_persistence.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>

namespace helix {

void PrinterMotionState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterMotionState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterMotionState] Initializing subjects (register_xml={})", register_xml);

    // Toolhead position subjects (actual physical position)
    INIT_SUBJECT_INT(position_x, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(position_y, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(position_z, 0, subjects_, register_xml);

    // Gcode position subjects (commanded position)
    INIT_SUBJECT_INT(gcode_position_x, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(gcode_position_y, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(gcode_position_z, 0, subjects_, register_xml);

    // Live position subjects (physical position mid-move, motion_report)
    INIT_SUBJECT_INT(live_position_x, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(live_position_y, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(live_position_z, 0, subjects_, register_xml);

    INIT_SUBJECT_STRING(homed_axes, "", subjects_, register_xml);

    // Speed/Flow subjects (percentages)
    INIT_SUBJECT_INT(speed_factor, 100, subjects_, register_xml);
    INIT_SUBJECT_INT(flow_factor, 100, subjects_, register_xml);

    // Actual speed/velocity subjects
    INIT_SUBJECT_INT(max_velocity, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(live_extruder_velocity, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(live_velocity, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(gcode_z_offset, 0, subjects_,
                     register_xml); // Z-offset in microns from homing_origin[2]
    INIT_SUBJECT_INT(pending_z_offset_delta, 0, subjects_,
                     register_xml); // Accumulated adjustment during print
    INIT_SUBJECT_INT(persisted_z_offset, 0, subjects_,
                     register_xml); // Persisted z-offset from the firmware provider, microns
    INIT_SUBJECT_INT(persisted_z_offset_valid, 0, subjects_,
                     register_xml); // 1 once the firmware has told us a stored offset

    subjects_initialized_ = true;
    spdlog::trace("[PrinterMotionState] Subjects initialized successfully");
}

void PrinterMotionState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterMotionState] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
    axis_bounds_ = AxisBounds{};
    homing_origin_x_ = homing_origin_y_ = homing_origin_z_ = 0.0;
}

void PrinterMotionState::update_from_status(const nlohmann::json& status) {
    if (const auto* toolhead_obj = json_util::find_member(status, "toolhead")) {
        const auto& toolhead = *toolhead_obj;

        if (const auto* position = json_util::find_array(toolhead, "position")) {
            const auto& pos = *position;
            // Note: Klipper can send null position values before homing or during errors
            // Store positions as centimillimeters (×100) for 0.01mm precision
            if (pos.size() >= 3 && pos[0].is_number() && pos[1].is_number() && pos[2].is_number()) {
                int new_x = helix::units::to_centimm(pos[0].get<double>());
                int new_y = helix::units::to_centimm(pos[1].get<double>());
                int new_z = helix::units::to_centimm(pos[2].get<double>());
                lv_subject_set_int(&position_x_, new_x);
                lv_subject_set_int(&position_y_, new_y);
                lv_subject_set_int(&position_z_, new_z);
            }
        }

        if (const auto max_velocity = json_util::read_field<double>(toolhead, "max_velocity")) {
            int max_vel = static_cast<int>(*max_velocity);
            lv_subject_set_int(&max_velocity_, max_vel);
        }

        // Kinematic envelope — set has-bits when first values arrive so jog
        // clamping can tell "Klipper hasn't told us yet" from "limit is 0".
        const auto* axis_min = json_util::find_array(toolhead, "axis_minimum");
        const auto* axis_max = json_util::find_array(toolhead, "axis_maximum");
        if (axis_min && axis_max && axis_min->size() >= 3 && axis_max->size() >= 3) {
            const auto& mn = *axis_min;
            const auto& mx = *axis_max;
            if (mn[0].is_number() && mx[0].is_number()) {
                axis_bounds_.x_min = mn[0].get<float>();
                axis_bounds_.x_max = mx[0].get<float>();
                axis_bounds_.has_x = true;
            }
            if (mn[1].is_number() && mx[1].is_number()) {
                axis_bounds_.y_min = mn[1].get<float>();
                axis_bounds_.y_max = mx[1].get<float>();
                axis_bounds_.has_y = true;
            }
            if (mn[2].is_number() && mx[2].is_number()) {
                axis_bounds_.z_min = mn[2].get<float>();
                axis_bounds_.z_max = mx[2].get<float>();
                axis_bounds_.has_z = true;
            }
        }

        if (const auto homed_axes = json_util::read_field<std::string>(toolhead, "homed_axes")) {
            const std::string& axes = *homed_axes;
            if (strcmp(lv_subject_get_string(&homed_axes_), axes.c_str()) != 0) {
                lv_subject_set_string(&homed_axes_, axes.c_str());
            }
            // Note: Derived homing subjects (xy_homed, z_homed, all_homed) are
            // panel-local in ControlsPanel, which observes this homed_axes string.
        }
    }

    // Update gcode_move data (commanded position, speed/flow factors, z-offset)
    if (const auto* gcode_move_obj = json_util::find_member(status, "gcode_move")) {
        const auto& gcode_move = *gcode_move_obj;

        // Parse commanded position from gcode_move.gcode_position
        // Note: gcode_move.position is raw commanded, gcode_move.gcode_position is effective
        // (after offset adjustments). UI should display gcode_position to match Mainsail.
        if (const auto* gcode_position = json_util::find_array(gcode_move, "gcode_position")) {
            const auto& pos = *gcode_position;
            if (pos.size() >= 3 && pos[0].is_number() && pos[1].is_number() && pos[2].is_number()) {
                int new_x = helix::units::to_centimm(pos[0].get<double>());
                int new_y = helix::units::to_centimm(pos[1].get<double>());
                int new_z = helix::units::to_centimm(pos[2].get<double>());
                lv_subject_set_int(&gcode_position_x_, new_x);
                lv_subject_set_int(&gcode_position_y_, new_y);
                lv_subject_set_int(&gcode_position_z_, new_z);
            }
        }

        if (const auto speed_factor = json_util::read_field<double>(gcode_move, "speed_factor")) {
            int factor_pct = helix::units::to_percent(*speed_factor);
            lv_subject_set_int(&speed_factor_, factor_pct);
        }

        if (const auto extrude_factor =
                json_util::read_field<double>(gcode_move, "extrude_factor")) {
            int factor_pct = helix::units::to_percent(*extrude_factor);
            lv_subject_set_int(&flow_factor_, factor_pct);
        }

        // Parse Z-offset from homing_origin[2] (baby stepping / SET_GCODE_OFFSET Z=)
        if (const auto* homing_origin = json_util::find_array(gcode_move, "homing_origin")) {
            const auto& origin = *homing_origin;
            // Full origin per axis (mm): machine = gcode + homing_origin, so the
            // G-code envelope the motion panel clamps against is the machine
            // one shifted by minus each component (see to_gcode_space()).
            if (origin.size() >= 3 && origin[0].is_number() && origin[1].is_number() &&
                origin[2].is_number()) {
                homing_origin_x_ = origin[0].get<double>();
                homing_origin_y_ = origin[1].get<double>();
                homing_origin_z_ = origin[2].get<double>();
            }
            if (origin.size() >= 3 && origin[2].is_number()) {
                // Round (not truncate) so Klipper's float accumulation of relative
                // Z_ADJUST deltas (0.04 - 0.01 -> 0.0299999) snaps back to a clean
                // micron value; truncation left the display stuck at 0.029 and made
                // it impossible to return to a hundredth-rounded offset.
                int z_microns = static_cast<int>(std::lround(origin[2].get<double>() * 1000.0));
                if (lv_subject_get_int(&gcode_z_offset_) != z_microns) {
                    lv_subject_set_int(&gcode_z_offset_, z_microns);
                    spdlog::trace("[PrinterMotionState] G-code Z-offset: {}um", z_microns);
                }
            }
        }
    }
    // Firmware-persisted z-offset. Only assign when this frame actually carries
    // one: the carrying status object is delta-only, so the very next gcode_move
    // frame omits it and must not blank what we already know.
    if (auto persisted = zoffset::read_persisted_offset_microns(status)) {
        if (lv_subject_get_int(&persisted_z_offset_) != *persisted) {
            lv_subject_set_int(&persisted_z_offset_, *persisted);
            spdlog::debug("[PrinterMotionState] Persisted Z-offset: {}um", *persisted);
        }
        lv_subject_set_int(&persisted_z_offset_valid_, 1);
    }

    // Update motion_report data (live toolhead and extruder velocity)
    if (const auto* motion_report = json_util::find_member(status, "motion_report")) {
        const auto& mr = *motion_report;
        if (const auto* live_position = json_util::find_array(mr, "live_position")) {
            const auto& pos = *live_position;
            // Where the nozzle physically is, mesh and z-offset included,
            // updating during moves. Stored as centimillimeters like the
            // toolhead/gcode positions.
            if (pos.size() >= 3 && pos[0].is_number() && pos[1].is_number() && pos[2].is_number()) {
                int new_x = helix::units::to_centimm(pos[0].get<double>());
                int new_y = helix::units::to_centimm(pos[1].get<double>());
                int new_z = helix::units::to_centimm(pos[2].get<double>());
                lv_subject_set_int(&live_position_x_, new_x);
                lv_subject_set_int(&live_position_y_, new_y);
                lv_subject_set_int(&live_position_z_, new_z);
            }
        }
        if (const auto velocity = json_util::read_field<double>(mr, "live_extruder_velocity")) {
            int vel_centimm = static_cast<int>(*velocity * 100.0);
            lv_subject_set_int(&live_extruder_velocity_, vel_centimm);
        }
        if (const auto velocity = json_util::read_field<double>(mr, "live_velocity")) {
            int vel_mm_s = static_cast<int>(std::lround(*velocity));
            lv_subject_set_int(&live_velocity_, vel_mm_s);
        }
    }
}

// ============================================================================
// PENDING Z-OFFSET DELTA TRACKING
// ============================================================================

void PrinterMotionState::add_pending_z_offset_delta(int delta_microns) {
    int current = lv_subject_get_int(&pending_z_offset_delta_);
    int new_value = current + delta_microns;
    lv_subject_set_int(&pending_z_offset_delta_, new_value);
    spdlog::debug("[PrinterMotionState] Pending Z-offset delta: {:+}um (total: {:+}um)",
                  delta_microns, new_value);
}

int PrinterMotionState::get_pending_z_offset_delta() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&pending_z_offset_delta_));
}

bool PrinterMotionState::has_pending_z_offset_adjustment() const {
    return get_pending_z_offset_delta() != 0;
}

void PrinterMotionState::clear_pending_z_offset_delta() {
    if (has_pending_z_offset_adjustment()) {
        spdlog::info("[PrinterMotionState] Clearing pending Z-offset delta");
        lv_subject_set_int(&pending_z_offset_delta_, 0);
    }
}

} // namespace helix
