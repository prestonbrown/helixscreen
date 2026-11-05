// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "pre_print_option.h"
#include "preprint_skip_wrappers.h"
#include "subject_managed_panel.h"

#include <lvgl.h>
#include <map>
#include <string>

namespace helix {

/**
 * @brief Z-offset calibration strategy — determines gcode commands for calibration and save
 *
 * Different printers need different approaches to calibrate and persist Z-offset.
 * FIRMWARE_MANAGED: HelixScreen offers no Save step; calibration applies SET_GCODE_OFFSET and
 * the firmware owns what happens next (FlashForge, ForgeX-mod and Artillery M1 persist it; the
 * Snapmaker U1 does not, see ZOffsetTuneHint). PROBE_CALIBRATE: standard Klipper
 * PROBE_CALIBRATE -> ACCEPT -> SAVE_CONFIG. ENDSTOP: Z_ENDSTOP_CALIBRATE -> ACCEPT ->
 * Z_OFFSET_APPLY_ENDSTOP -> SAVE_CONFIG.
 */
enum class ZOffsetCalibrationStrategy {
    PROBE_CALIBRATE,  ///< Standard Klipper: PROBE_CALIBRATE -> ACCEPT -> SAVE_CONFIG
    FIRMWARE_MANAGED, ///< No HelixScreen Save step: SET_GCODE_OFFSET, then the firmware's call
    ENDSTOP ///< Endstop: Z_ENDSTOP_CALIBRATE -> ACCEPT -> Z_OFFSET_APPLY_ENDSTOP -> SAVE_CONFIG
};

/**
 * @brief What Print Tune can truthfully say about a z-offset adjustment
 *
 * Published as the `z_offset_tune_hint` subject; the values are the XML's ref_values.
 */
enum class ZOffsetTuneHint {
    SAVE_ON_CONTROLS = 0,      ///< Temporary unless the user saves it (Save Z Offset)
    AUTO_SAVED = 1,            ///< The firmware or an installed module persists it
    TEMPORARY = 2,             ///< No save path: it lasts until the print ends
    TEMPORARY_TOOL_CHANGE = 3, ///< As TEMPORARY, and a tool change re-applies the head's own
                               ///< stored offset over it
};

/**
 * @brief Pick the tune hint from what is known about persistence
 *
 * @param external_persistence an installed z-offset persistence provider is latched
 * @param db_persists the printer database does not say the firmware drops the offset
 *        (`z_offset_persists`, default true)
 * @param tool_change_resets the printer database's `tool_change_resets_z_offset`
 * @param save_enabled the user offers Z-offset saves on this printer
 *        (helix::zoffset::save_enabled()); off turns a Save step into no save path
 */
ZOffsetTuneHint resolve_z_offset_tune_hint(ZOffsetCalibrationStrategy strategy,
                                           bool external_persistence, bool db_persists,
                                           bool tool_change_resets, bool save_enabled);

/**
 * @brief The printer type and what it decides: the pre-print option set and the
 *        z-offset calibration strategy
 *
 * The type comes from the printer database (detection, wizard, printer manager);
 * the option set is the database's, with runtime options (timelapse) synthesized
 * on top and firmware-held defaults overlaid. Inputs owned by other domains
 * (probe, exclude_object, timelapse availability) are passed in. Main thread only.
 */
class PrinterProfileState {
  public:
    PrinterProfileState() = default;
    PrinterProfileState(const PrinterProfileState&) = delete;
    PrinterProfileState& operator=(const PrinterProfileState&) = delete;

    void init_subjects(bool register_xml = true);
    void deinit_subjects();

    /**
     * @brief Resolve @p type against the printer database
     *
     * @param has_probe decides the strategy when the database names none
     * @return false when the type and strategy are what they already were
     */
    bool set_printer_type(const std::string& type, bool has_probe, bool exclude_object_known,
                          bool timelapse_available);

    /**
     * @brief Synthesize runtime-dependent options into the cached option set
     *
     * Some options are driven by runtime capability discovery rather than the
     * database (the timelapse toggle appears only when the moonraker-timelapse
     * plugin is installed), and the bed_mesh option goes adaptive only when the
     * firmware has [exclude_object]. Idempotent: re-running clears previously
     * synthesized options before re-adding the ones that apply now.
     *
     * @param skip_ops Leveling steps whose skip toggle is offered now
     *        (skip_wrappers::offerable()); a database option for the same
     *        step wins over the toggle.
     */
    void apply_dynamic_options(bool exclude_object_known, bool timelapse_available,
                               const std::vector<skip_wrappers::Op>& skip_ops = {});

    /**
     * @brief Record whether an installed module persists the z-offset itself
     *
     * While set the strategy resolves to FIRMWARE_MANAGED whatever the type
     * says, and the tune hint to AUTO_SAVED (prestonbrown/helixscreen#1401).
     * The caller re-resolves the type.
     * @return true when the flag changed
     */
    bool set_external_persistence(bool persisted);
    bool external_persistence() const {
        return external_persistence_;
    }

    /// Default for the synthesized timelapse option: the global
    /// moonraker-timelapse `enabled` setting (#1094).
    void set_timelapse_default_enabled(bool enabled) {
        timelapse_default_enabled_ = enabled;
    }

    /// The database declares this printer's own timelapse option, so its firmware
    /// records timelapses and the plugin's controls do not apply.
    [[nodiscard]] bool firmware_owns_timelapse() const {
        return firmware_owns_timelapse_;
    }

    /// Merge the settings a self-storing firmware holds, keyed by option id.
    /// Merges rather than replaces: Moonraker sends deltas, so a frame mentioning
    /// one setting is silent about the rest. @return true when any changed
    bool merge_firmware_option_defaults(const std::map<std::string, bool>& defaults);

    /// Forget the stored settings, when the connected machine changes. Options
    /// they overrode get their database defaults back;
    /// the caller re-runs apply_dynamic_options(). @return true when any were held
    bool clear_firmware_option_defaults();

    const std::string& printer_type() const {
        return printer_type_;
    }
    const PrePrintOptionSet& pre_print_option_set() const {
        return pre_print_option_set_;
    }
    ZOffsetCalibrationStrategy z_offset_calibration_strategy() const {
        return z_offset_calibration_strategy_;
    }
    /// The printer database says this printer type is enclosed.
    bool db_enclosed() const {
        return db_enclosed_;
    }

    /// Whether this printer's Klipper skips dot-directories when it resolves a
    /// print (the printer database's `klipper_hides_dot_dirs`): files the app
    /// stages for printing must sit in a visible directory.
    bool klipper_hides_dot_dirs() const {
        return db_hides_dot_dirs_;
    }

    /**
     * @brief String subject updated on every change to the resolved type
     *
     * Resets to "" on deinit_subjects()/re-init, and the no-change early return
     * means a soft restart repopulates it only on the next real type change.
     * Treat it as a change signal and read the value from printer_type().
     */
    lv_subject_t* get_printer_type_subject() {
        return &printer_type_subject_;
    }
    /// 1 when HelixScreen offers Save Z Offset, 0 when the strategy has no Save step.
    lv_subject_t* get_z_offset_can_save_subject() {
        return &z_offset_can_save_;
    }
    /// Whether Z-offset saving is offered on this printer, as
    /// helix::zoffset::save_enabled() decides it; feeds the tune hint.
    /// Returns true when the published hint changed.
    bool set_z_offset_save_enabled(bool enabled);

    /// The ZOffsetTuneHint for the resolved type, as an int.
    lv_subject_t* get_z_offset_tune_hint_subject() {
        return &z_offset_tune_hint_;
    }

  private:
    friend class PrinterProfileStateTestAccess;

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    std::string printer_type_;
    PrePrintOptionSet pre_print_option_set_;
    ZOffsetCalibrationStrategy z_offset_calibration_strategy_ =
        ZOffsetCalibrationStrategy::PROBE_CALIBRATE;
    bool external_persistence_ = false;
    ZOffsetTuneHint z_offset_tune_hint_value_ = ZOffsetTuneHint::SAVE_ON_CONTROLS;
    bool z_offset_save_enabled_ = true;
    bool db_z_offset_persists_ = true;
    bool db_tool_change_resets_z_offset_ = false;
    bool db_enclosed_ = false;
    bool db_hides_dot_dirs_ = false;
    bool timelapse_default_enabled_ = false;
    bool firmware_owns_timelapse_ = false;
    /// What the firmware reports for each pre-print option it stores itself.
    /// Empty on printers whose firmware stores none.
    std::map<std::string, bool> firmware_option_defaults_;

    lv_subject_t printer_type_subject_{};
    char printer_type_subject_buf_[128];
    lv_subject_t z_offset_can_save_{};
    lv_subject_t z_offset_tune_hint_{};
};

} // namespace helix
