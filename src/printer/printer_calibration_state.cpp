// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_calibration_state.cpp
 * @brief Calibration and configuration state management extracted from PrinterState
 *
 * Manages firmware retraction, manual probe, and motor state subjects.
 * Uses centimillimeters and microns for integer precision with floating-point mm values.
 *
 * Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_calibration_state.h"

#include "state/subject_macros.h"
#include "unit_conversions.h"

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
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterCalibrationState::update_from_status(const nlohmann::json& status) {
    // Update manual probe state (for Z-offset calibration)
    // Klipper's manual_probe object is active during PROBE_CALIBRATE and Z_ENDSTOP_CALIBRATE
    if (status.contains("manual_probe")) {
        const auto& mp = status["manual_probe"];

        if (mp.contains("is_active") && mp["is_active"].is_boolean()) {
            bool is_active = mp["is_active"].get<bool>();
            int old_active = lv_subject_get_int(&manual_probe_active_);
            int new_active = is_active ? 1 : 0;

            if (old_active != new_active) {
                lv_subject_set_int(&manual_probe_active_, new_active);
                spdlog::info("[PrinterCalibrationState] Manual probe active: {} -> {}",
                             old_active != 0, is_active);
            }
        }

        if (mp.contains("z_position") && mp["z_position"].is_number()) {
            // Store as microns (multiply by 1000) for integer subject with 0.001mm resolution
            double z_mm = mp["z_position"].get<double>();
            int z_microns = static_cast<int>(z_mm * 1000.0);
            if (lv_subject_get_int(&manual_probe_z_position_) != z_microns) {
                lv_subject_set_int(&manual_probe_z_position_, z_microns);
            }
            spdlog::trace("[PrinterCalibrationState] Manual probe Z: {:.3f}mm", z_mm);
        }
    }

    // Update idle_timeout "Printing" busy flag. Klipper reports idle_timeout.state
    // as exactly "Printing" / "Ready" / "Idle"; "Printing" is held for the entire
    // duration of any blocking operation (G28, BED_MESH_CALIBRATE, QGL,
    // PROBE_ACCURACY, long macro) or a real file print. Case-sensitive compare.
    if (status.contains("idle_timeout")) {
        const auto& it = status["idle_timeout"];

        if (it.contains("state") && it["state"].is_string()) {
            std::string it_state = it["state"].get<std::string>();
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
    if (status.contains("toolhead")) {
        const auto& toolhead = status["toolhead"];

        if (toolhead.contains("homed_axes") && toolhead["homed_axes"].is_string()) {
            std::string axes = toolhead["homed_axes"].get<std::string>();
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
    if (status.contains("stepper_enable")) {
        const auto& se = status["stepper_enable"];

        if (se.contains("steppers") && se["steppers"].is_object()) {
            bool any_enabled = false;
            for (const auto& [name, enabled] : se["steppers"].items()) {
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
    if (status.contains("firmware_retraction")) {
        const auto& fr = status["firmware_retraction"];

        if (fr.contains("retract_length") && fr["retract_length"].is_number()) {
            // Store as centimillimeters (x100) to preserve 0.01mm precision
            int centimm = helix::units::json_to_centimm(fr, "retract_length");
            if (lv_subject_get_int(&retract_length_) != centimm) {
                lv_subject_set_int(&retract_length_, centimm);
                spdlog::trace("[PrinterCalibrationState] Retract length: {:.2f}mm",
                              helix::units::from_centimm(centimm));
            }
        }

        if (fr.contains("retract_speed") && fr["retract_speed"].is_number()) {
            int speed = static_cast<int>(fr["retract_speed"].get<double>());
            if (lv_subject_get_int(&retract_speed_) != speed) {
                lv_subject_set_int(&retract_speed_, speed);
                spdlog::trace("[PrinterCalibrationState] Retract speed: {}mm/s", speed);
            }
        }

        if (fr.contains("unretract_extra_length") && fr["unretract_extra_length"].is_number()) {
            int centimm = helix::units::json_to_centimm(fr, "unretract_extra_length");
            if (lv_subject_get_int(&unretract_extra_length_) != centimm) {
                lv_subject_set_int(&unretract_extra_length_, centimm);
                spdlog::trace("[PrinterCalibrationState] Unretract extra: {:.2f}mm",
                              helix::units::from_centimm(centimm));
            }
        }

        if (fr.contains("unretract_speed") && fr["unretract_speed"].is_number()) {
            int speed = static_cast<int>(fr["unretract_speed"].get<double>());
            if (lv_subject_get_int(&unretract_speed_) != speed) {
                lv_subject_set_int(&unretract_speed_, speed);
                spdlog::trace("[PrinterCalibrationState] Unretract speed: {}mm/s", speed);
            }
        }
    }
}

} // namespace helix
