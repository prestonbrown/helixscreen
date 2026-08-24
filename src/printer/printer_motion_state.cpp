// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_motion_state.cpp
 * @brief Motion state management extracted from PrinterState
 *
 * Manages position, speed/flow factors, and Z-offset subjects.
 * Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_motion_state.h"

#include "state/subject_macros.h"
#include "unit_conversions.h"
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

    INIT_SUBJECT_STRING(homed_axes, "", subjects_, register_xml);

    // Speed/Flow subjects (percentages)
    INIT_SUBJECT_INT(speed_factor, 100, subjects_, register_xml);
    INIT_SUBJECT_INT(flow_factor, 100, subjects_, register_xml);

    // Actual speed/velocity subjects
    INIT_SUBJECT_INT(gcode_speed, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(max_velocity, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(live_extruder_velocity, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(gcode_z_offset, 0, subjects_,
                     register_xml); // Z-offset in microns from homing_origin[2]
    INIT_SUBJECT_INT(pending_z_offset_delta, 0, subjects_,
                     register_xml); // Accumulated adjustment during print
    INIT_SUBJECT_INT(persisted_z_offset, 0, subjects_,
                     register_xml); // ZMOD save_variables.gcode_offsets.z, microns
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
}

void PrinterMotionState::update_from_status(const nlohmann::json& status) {
    // Update toolhead position
    if (status.contains("toolhead")) {
        const auto& toolhead = status["toolhead"];

        if (toolhead.contains("position") && toolhead["position"].is_array()) {
            const auto& pos = toolhead["position"];
            // Note: Klipper can send null position values before homing or during errors
            // Store positions as centimillimeters (×100) for 0.01mm precision
            if (pos.size() >= 3 && pos[0].is_number() && pos[1].is_number() && pos[2].is_number()) {
                int new_x = helix::units::to_centimm(pos[0].get<double>());
                int new_y = helix::units::to_centimm(pos[1].get<double>());
                int new_z = helix::units::to_centimm(pos[2].get<double>());
                if (lv_subject_get_int(&position_x_) != new_x) {
                    lv_subject_set_int(&position_x_, new_x);
                }
                if (lv_subject_get_int(&position_y_) != new_y) {
                    lv_subject_set_int(&position_y_, new_y);
                }
                if (lv_subject_get_int(&position_z_) != new_z) {
                    lv_subject_set_int(&position_z_, new_z);
                }
            }
        }

        if (toolhead.contains("max_velocity") && toolhead["max_velocity"].is_number()) {
            int max_vel = static_cast<int>(toolhead["max_velocity"].get<double>());
            if (lv_subject_get_int(&max_velocity_) != max_vel) {
                lv_subject_set_int(&max_velocity_, max_vel);
            }
        }

        // Kinematic envelope — set has-bits when first values arrive so jog
        // clamping can tell "Klipper hasn't told us yet" from "limit is 0".
        if (toolhead.contains("axis_minimum") && toolhead.contains("axis_maximum") &&
            toolhead["axis_minimum"].is_array() && toolhead["axis_maximum"].is_array() &&
            toolhead["axis_minimum"].size() >= 3 && toolhead["axis_maximum"].size() >= 3) {
            const auto& mn = toolhead["axis_minimum"];
            const auto& mx = toolhead["axis_maximum"];
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

        if (toolhead.contains("homed_axes") && toolhead["homed_axes"].is_string()) {
            std::string axes = toolhead["homed_axes"].get<std::string>();
            if (strcmp(lv_subject_get_string(&homed_axes_), axes.c_str()) != 0) {
                lv_subject_copy_string(&homed_axes_, axes.c_str());
            }
            // Note: Derived homing subjects (xy_homed, z_homed, all_homed) are now
            // panel-local in ControlsPanel, which observes this homed_axes string.
        }
    }

    // Update gcode_move data (commanded position, speed/flow factors, z-offset)
    if (status.contains("gcode_move")) {
        const auto& gcode_move = status["gcode_move"];

        // Parse commanded position from gcode_move.gcode_position
        // Note: gcode_move.position is raw commanded, gcode_move.gcode_position is effective
        // (after offset adjustments). UI should display gcode_position to match Mainsail.
        if (gcode_move.contains("gcode_position") && gcode_move["gcode_position"].is_array()) {
            const auto& pos = gcode_move["gcode_position"];
            if (pos.size() >= 3 && pos[0].is_number() && pos[1].is_number() && pos[2].is_number()) {
                int new_x = helix::units::to_centimm(pos[0].get<double>());
                int new_y = helix::units::to_centimm(pos[1].get<double>());
                int new_z = helix::units::to_centimm(pos[2].get<double>());
                if (lv_subject_get_int(&gcode_position_x_) != new_x) {
                    lv_subject_set_int(&gcode_position_x_, new_x);
                }
                if (lv_subject_get_int(&gcode_position_y_) != new_y) {
                    lv_subject_set_int(&gcode_position_y_, new_y);
                }
                if (lv_subject_get_int(&gcode_position_z_) != new_z) {
                    lv_subject_set_int(&gcode_position_z_, new_z);
                }
            }
        }

        if (gcode_move.contains("speed") && gcode_move["speed"].is_number()) {
            int speed_mm_s = static_cast<int>(gcode_move["speed"].get<double>());
            if (lv_subject_get_int(&gcode_speed_) != speed_mm_s) {
                lv_subject_set_int(&gcode_speed_, speed_mm_s);
            }
        }

        if (gcode_move.contains("speed_factor") && gcode_move["speed_factor"].is_number()) {
            int factor_pct = helix::units::json_to_percent(gcode_move, "speed_factor");
            if (lv_subject_get_int(&speed_factor_) != factor_pct) {
                lv_subject_set_int(&speed_factor_, factor_pct);
            }
        }

        if (gcode_move.contains("extrude_factor") && gcode_move["extrude_factor"].is_number()) {
            int factor_pct = helix::units::json_to_percent(gcode_move, "extrude_factor");
            if (lv_subject_get_int(&flow_factor_) != factor_pct) {
                lv_subject_set_int(&flow_factor_, factor_pct);
            }
        }

        // Parse Z-offset from homing_origin[2] (baby stepping / SET_GCODE_OFFSET Z=)
        if (gcode_move.contains("homing_origin") && gcode_move["homing_origin"].is_array()) {
            const auto& origin = gcode_move["homing_origin"];
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
        if (lv_subject_get_int(&persisted_z_offset_valid_) != 1) {
            lv_subject_set_int(&persisted_z_offset_valid_, 1);
        }
    }

    // Update motion_report data (live extruder velocity)
    if (status.contains("motion_report")) {
        const auto& mr = status["motion_report"];
        if (mr.contains("live_extruder_velocity") && mr["live_extruder_velocity"].is_number()) {
            int vel_centimm = static_cast<int>(mr["live_extruder_velocity"].get<double>() * 100.0);
            if (lv_subject_get_int(&live_extruder_velocity_) != vel_centimm) {
                lv_subject_set_int(&live_extruder_velocity_, vel_centimm);
            }
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
