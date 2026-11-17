// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_calibration_state.cpp
 * @brief Calibration and configuration state management (a PrinterState sub-state)
 *
 * Manages firmware retraction, manual probe, and motor state subjects.
 * Uses centimillimeters and microns for integer precision with floating-point mm values.
 */

#include "printer_calibration_state.h"

#include "json_utils.h"
#include "state/subject_macros.h"
#include "unit_conversions_json.h"

#include <spdlog/spdlog.h>

namespace helix {

void PrinterCalibrationState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterCalibrationState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterCalibrationState] Initializing subjects (register_xml={})",
                  register_xml);

    // init_subjects() may run more than once (tests re-init per case). Drop stale
    // registrations first so entries do not accumulate.
    volatile_.clear();

    // Firmware retraction settings (defaults: disabled)
    INIT_SUBJECT_INT(retract_length, 0, subjects_, register_xml);         // 0 = disabled
    INIT_SUBJECT_INT(retract_speed, 20, subjects_, register_xml);         // 20 mm/s default
    INIT_SUBJECT_INT(unretract_extra_length, 0, subjects_, register_xml); // 0mm extra
    INIT_SUBJECT_INT(unretract_speed, 10, subjects_, register_xml);       // 10 mm/s default

    // Manual probe subjects (for Z-offset calibration)
    // Klippy-volatile: delta-only fields whose stale value gates behaviour (#1129).
    INIT_SUBJECT_INT_VOLATILE(manual_probe_active, 0, subjects_, volatile_,
                              register_xml); // 0=inactive, 1=active
    INIT_SUBJECT_INT_VOLATILE(manual_probe_z_position, 0, subjects_, volatile_,
                              register_xml); // Z position in microns

    // Motor enabled state (from toolhead.homed_axes - defaults to enabled).
    // NOT Klippy-volatile: right after a Klipper shutdown the steppers are
    // affirmatively de-energized, so a stale 0 is correct, not stale. Resetting
    // it to 1 on a Klippy transition would briefly claim motors are on when they
    // are not. Self-heals on the next stepper_enable/homed_axes snapshot anyway.
    INIT_SUBJECT_INT(motors_enabled, 1, subjects_,
                     register_xml); // 1=enabled (Ready/Printing), 0=disabled (Idle)

    // idle_timeout.state == "Printing" busy flag (default: not busy)
    idle_timeout_printing_lifetime_ = std::make_shared<bool>(true);
    INIT_SUBJECT_INT_VOLATILE(idle_timeout_printing, 0, subjects_, volatile_,
                              register_xml); // 1 if idle_timeout.state == "Printing", else 0

    subjects_initialized_ = true;
    spdlog::trace("[PrinterCalibrationState] Subjects initialized successfully");
}

void PrinterCalibrationState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterCalibrationState] Deinitializing subjects");
    volatile_.clear();
    // Before the subjects are freed: pending observer guards must see the
    // subject as dead so their removal skips the freed nodes.
    if (idle_timeout_printing_lifetime_) {
        *idle_timeout_printing_lifetime_ = false;
    }
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterCalibrationState::update_from_status(const nlohmann::json& status) {
    // Update manual probe state (for Z-offset calibration)
    // Klipper's manual_probe object is active during PROBE_CALIBRATE and Z_ENDSTOP_CALIBRATE
    if (const auto* manual_probe = json_util::find_member(status, "manual_probe")) {
        const auto& mp = *manual_probe;

        if (const auto active = json_util::read_field<bool>(mp, "is_active")) {
            bool is_active = *active;
            int old_active = lv_subject_get_int(&manual_probe_active_);
            int new_active = is_active ? 1 : 0;

            if (old_active != new_active) {
                lv_subject_set_int(&manual_probe_active_, new_active);
                spdlog::info("[PrinterCalibrationState] Manual probe active: {} -> {}",
                             old_active != 0, is_active);
            }
        }

        if (const auto z_position = json_util::read_field<double>(mp, "z_position")) {
            // Store as microns (multiply by 1000) for integer subject with 0.001mm resolution
            double z_mm = *z_position;
            int z_microns = static_cast<int>(z_mm * 1000.0);
            lv_subject_set_int(&manual_probe_z_position_, z_microns);
            spdlog::trace("[PrinterCalibrationState] Manual probe Z: {:.3f}mm", z_mm);
        }
    }

    // Update idle_timeout "Printing" busy flag. Klipper reports idle_timeout.state
    // as exactly "Printing" / "Ready" / "Idle"; "Printing" is held for the entire
    // duration of any blocking operation (G28, BED_MESH_CALIBRATE, QGL,
    // PROBE_ACCURACY, long macro) or a real file print. Case-sensitive compare.
    if (const auto* idle_timeout = json_util::find_member(status, "idle_timeout")) {
        if (const auto state = json_util::read_field<std::string>(*idle_timeout, "state")) {
            const std::string& it_state = *state;
            int new_printing = (it_state == "Printing") ? 1 : 0;
            int old_printing = lv_subject_get_int(&idle_timeout_printing_);

            // Feed the debounce on every report, not only on the edge: it needs
            // the repeats to stay idempotent, and it is what the blocking-op
            // guard reads (the raw subject stays the literal Klipper state for
            // display consumers).
            idle_timeout_busy_.set_printing(new_printing != 0);

            if (old_printing != new_printing) {
                lv_subject_set_int(&idle_timeout_printing_, new_printing);
                spdlog::debug("[PrinterCalibrationState] idle_timeout printing: {} -> {} "
                              "(state='{}')",
                              old_printing != 0, new_printing != 0, it_state);
            }
        }
    }

    // Update motor enabled state from toolhead.homed_axes (primary) and stepper_enable (fallback)
    // M84 clears homed_axes on all Klipper printers, making it the most reliable indicator.
    // stepper_enable.steppers is more precise but not reported by all firmware (e.g. AD5M).
    if (const auto* toolhead = json_util::find_member(status, "toolhead")) {
        if (const auto homed_axes = json_util::read_field<std::string>(*toolhead, "homed_axes")) {
            const std::string& axes = *homed_axes;
            int new_enabled = axes.empty() ? 0 : 1;
            int old_enabled = lv_subject_get_int(&motors_enabled_);

            if (old_enabled != new_enabled) {
                lv_subject_set_int(&motors_enabled_, new_enabled);
                spdlog::trace("[PrinterCalibrationState] Motors {}: homed_axes='{}'",
                              new_enabled ? "enabled" : "disabled", axes);
            }
        }
    }

    // Fallback: stepper_enable.steppers (not reported by all firmware)
    if (const auto* stepper_enable = json_util::find_member(status, "stepper_enable")) {
        if (const auto* steppers = json_util::find_object(*stepper_enable, "steppers")) {
            bool any_enabled = false;
            for (const auto& [name, enabled] : steppers->items()) {
                if (enabled.is_boolean() && enabled.get<bool>()) {
                    any_enabled = true;
                    break;
                }
            }

            int new_enabled = any_enabled ? 1 : 0;
            int old_enabled = lv_subject_get_int(&motors_enabled_);

            if (old_enabled != new_enabled) {
                lv_subject_set_int(&motors_enabled_, new_enabled);
                spdlog::trace("[PrinterCalibrationState] Motors {}: stepper_enable update",
                              new_enabled ? "enabled" : "disabled");
            }
        }
    }

    // Parse firmware_retraction settings (G10/G11 retraction parameters)
    if (const auto* firmware_retraction = json_util::find_member(status, "firmware_retraction")) {
        const auto& fr = *firmware_retraction;

        if (const auto retract_length = json_util::read_field<double>(fr, "retract_length")) {
            // Store as centimillimeters (x100) to preserve 0.01mm precision
            int centimm = helix::units::to_centimm(*retract_length);
            if (lv_subject_get_int(&retract_length_) != centimm) {
                lv_subject_set_int(&retract_length_, centimm);
                spdlog::trace("[PrinterCalibrationState] Retract length: {:.2f}mm",
                              helix::units::from_centimm(centimm));
            }
        }

        if (const auto retract_speed = json_util::read_field<double>(fr, "retract_speed")) {
            int speed = static_cast<int>(*retract_speed);
            if (lv_subject_get_int(&retract_speed_) != speed) {
                lv_subject_set_int(&retract_speed_, speed);
                spdlog::trace("[PrinterCalibrationState] Retract speed: {}mm/s", speed);
            }
        }

        if (const auto extra = json_util::read_field<double>(fr, "unretract_extra_length")) {
            int centimm = helix::units::to_centimm(*extra);
            if (lv_subject_get_int(&unretract_extra_length_) != centimm) {
                lv_subject_set_int(&unretract_extra_length_, centimm);
                spdlog::trace("[PrinterCalibrationState] Unretract extra: {:.2f}mm",
                              helix::units::from_centimm(centimm));
            }
        }

        if (const auto unretract_speed = json_util::read_field<double>(fr, "unretract_speed")) {
            int speed = static_cast<int>(*unretract_speed);
            if (lv_subject_get_int(&unretract_speed_) != speed) {
                lv_subject_set_int(&unretract_speed_, speed);
                spdlog::trace("[PrinterCalibrationState] Unretract speed: {}mm/s", speed);
            }
        }
    }
}

} // namespace helix
