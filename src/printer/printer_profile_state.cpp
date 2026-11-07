// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_profile_state.h"

#include "printer_detector.h"
#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

ZOffsetTuneHint resolve_z_offset_tune_hint(ZOffsetCalibrationStrategy strategy,
                                           bool external_persistence, bool db_persists,
                                           bool tool_change_resets, bool save_enabled,
                                           bool per_head) {
    const ZOffsetTuneHint temporary =
        tool_change_resets ? ZOffsetTuneHint::TEMPORARY_TOOL_CHANGE : ZOffsetTuneHint::TEMPORARY;
    if (strategy != ZOffsetCalibrationStrategy::FIRMWARE_MANAGED) {
        return save_enabled ? ZOffsetTuneHint::SAVE_ON_CONTROLS : temporary;
    }
    if (per_head) {
        return ZOffsetTuneHint::AUTO_SAVED_PER_HEAD;
    }
    if (external_persistence || db_persists) {
        return ZOffsetTuneHint::AUTO_SAVED;
    }
    return temporary;
}

void PrinterProfileState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        return;
    }
    // Resolved printer type. Not XML-registered: the name collides with the
    // connection-transport int subject ("network"/"usb"/"bluetooth"), and no
    // XML binds it; observers attach programmatically (printer artwork).
    INIT_SUBJECT_STRING(printer_type_subject, "", subjects_, false);

    // Z-offset save visibility (1 = Save Z Offset offered, 0 = no Save step)
    INIT_SUBJECT_INT(z_offset_can_save, 1, subjects_, register_xml);
    INIT_SUBJECT_INT(z_offset_tune_hint, static_cast<int>(z_offset_tune_hint_value_), subjects_,
                     register_xml);
    INIT_SUBJECT_INT(z_offset_reapplied_per_head, z_offset_reapplied_per_head() ? 1 : 0, subjects_,
                     register_xml);
    subjects_initialized_ = true;
}

void PrinterProfileState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

bool PrinterProfileState::set_printer_type(const std::string& type, bool has_probe,
                                           bool exclude_object_known, bool timelapse_available) {
    // Determine what the z-cal strategy would be for this type so we can
    // skip redundant updates (auto-detect often confirms the saved type).
    auto new_options = PrinterDetector::get_pre_print_option_set(type);
    std::string strategy_str = PrinterDetector::get_z_offset_calibration_strategy(type);
    ZOffsetCalibrationStrategy new_strategy;
    if (strategy_str == "firmware_managed") {
        new_strategy = ZOffsetCalibrationStrategy::FIRMWARE_MANAGED;
    } else if (strategy_str == "endstop") {
        new_strategy = ZOffsetCalibrationStrategy::ENDSTOP;
    } else if (strategy_str == "probe_calibrate") {
        new_strategy = ZOffsetCalibrationStrategy::PROBE_CALIBRATE;
    } else {
        new_strategy = has_probe ? ZOffsetCalibrationStrategy::PROBE_CALIBRATE
                                 : ZOffsetCalibrationStrategy::ENDSTOP;
    }

    // An installed SET_GCODE_OFFSET wrapper persists the offset itself; the
    // type-derived strategy would fold the gcode offset into the probe and the
    // wrapper's boot gcode would re-apply it - runaway stacking
    // (prestonbrown/helixscreen#1401).
    if (external_persistence_) {
        new_strategy = ZOffsetCalibrationStrategy::FIRMWARE_MANAGED;
    }

    const bool db_persists = PrinterDetector::z_offset_persists(type);
    const bool db_tool_change_resets = PrinterDetector::tool_change_resets_z_offset(type);
    const ZOffsetTuneHint new_hint = resolve_z_offset_tune_hint(
        new_strategy, external_persistence_, db_persists, db_tool_change_resets,
        z_offset_save_enabled_, z_offset_per_head_);

    if (type == printer_type_ && new_strategy == z_offset_calibration_strategy_ &&
        new_hint == z_offset_tune_hint_value_) {
        return false;
    }

    printer_type_ = type;
    pre_print_option_set_ = new_options;
    z_offset_calibration_strategy_ = new_strategy;
    z_offset_tune_hint_value_ = new_hint;
    db_z_offset_persists_ = db_persists;
    db_tool_change_resets_z_offset_ = db_tool_change_resets;
    db_enclosed_ = PrinterDetector::is_enclosed(type);
    db_hides_dot_dirs_ = PrinterDetector::klipper_hides_dot_dirs(type);

    if (subjects_initialized_) {
        lv_subject_copy_string(&printer_type_subject_, type.c_str());
    }

    // Synthesize runtime-dependent options (timelapse) on top of the DB load.
    apply_dynamic_options(exclude_object_known, timelapse_available);

    if (subjects_initialized_) {
        lv_subject_set_int(&z_offset_can_save_,
                           new_strategy != ZOffsetCalibrationStrategy::FIRMWARE_MANAGED ? 1 : 0);
        lv_subject_set_int(&z_offset_tune_hint_, static_cast<int>(new_hint));
    }
    publish_z_offset_reapplied_per_head();
    return true;
}

bool PrinterProfileState::set_z_offset_save_enabled(bool enabled) {
    if (z_offset_save_enabled_ == enabled) {
        return false;
    }
    z_offset_save_enabled_ = enabled;
    const ZOffsetTuneHint new_hint = resolve_z_offset_tune_hint(
        z_offset_calibration_strategy_, external_persistence_, db_z_offset_persists_,
        db_tool_change_resets_z_offset_, enabled, z_offset_per_head_);
    if (new_hint == z_offset_tune_hint_value_) {
        return false;
    }
    z_offset_tune_hint_value_ = new_hint;
    if (subjects_initialized_) {
        lv_subject_set_int(&z_offset_tune_hint_, static_cast<int>(new_hint));
    }
    return true;
}

bool PrinterProfileState::set_external_persistence(bool persisted) {
    if (external_persistence_ == persisted) {
        return false;
    }
    external_persistence_ = persisted;
    return true;
}

bool PrinterProfileState::set_z_offset_per_head(bool per_head) {
    if (z_offset_per_head_ == per_head) {
        return false;
    }
    z_offset_per_head_ = per_head;
    publish_z_offset_reapplied_per_head();
    return true;
}

void PrinterProfileState::publish_z_offset_reapplied_per_head() {
    if (subjects_initialized_) {
        lv_subject_set_int(&z_offset_reapplied_per_head_, z_offset_reapplied_per_head() ? 1 : 0);
    }
}

bool PrinterProfileState::merge_firmware_option_defaults(
    const std::map<std::string, bool>& defaults) {
    bool changed = false;
    for (const auto& [option_id, enabled] : defaults) {
        auto it = firmware_option_defaults_.find(option_id);
        if (it == firmware_option_defaults_.end() || it->second != enabled) {
            firmware_option_defaults_[option_id] = enabled;
            changed = true;
        }
    }
    return changed;
}

bool PrinterProfileState::clear_firmware_option_defaults() {
    if (firmware_option_defaults_.empty()) {
        return false;
    }
    firmware_option_defaults_.clear();
    pre_print_option_set_ = PrinterDetector::get_pre_print_option_set(printer_type_);
    return true;
}

void PrinterProfileState::apply_dynamic_options(bool exclude_object_known, bool timelapse_available,
                                                const std::vector<skip_wrappers::Op>& skip_ops) {
    // Strip any previously synthesized dynamic options before re-adding so
    // this method is idempotent and handles capability changes (e.g.
    // moonraker-timelapse plugin going from absent to present).
    pre_print_option_set_.options.erase(
        std::remove_if(pre_print_option_set_.options.begin(), pre_print_option_set_.options.end(),
                       [](const PrePrintOption& opt) {
                           return opt.id == "timelapse" || skip_wrappers::is_wrapper_option(opt);
                       }),
        pre_print_option_set_.options.end());

    // Adaptive bed mesh: a property of the SINGLE bed_mesh toggle, not a separate
    // row. When ALL hold, the bed_mesh option is relabeled "Adaptive Bed Mesh"
    // and emits its adaptive token (e.g. ADAPTIVE=1) alongside the enable param
    // when ON; otherwise it stays the plain "Auto Bed Mesh" with unchanged
    // behavior. Conditions (so it's never a silent no-op):
    //   1. the bed_mesh option is a MacroParam declaring an adaptive_param (the
    //      START_PRINT forwarding signal — the macro passes the token into
    //      BED_MESH_CALIBRATE),
    //   2. the firmware exposes [exclude_object] (adaptive maps printed objects),
    //   3. no custom calibration.bed_mesh_gcode template is in use (that path
    //      runs verbatim and ignores ADAPTIVE).
    // Recomputed each run (idempotent, non-destructive) so it tracks capability
    // changes — e.g. exclude_object only becomes known once hardware arrives.
    for (auto& opt : pre_print_option_set_.options) {
        if (opt.id != "bed_mesh") {
            continue;
        }
        const auto* mp = std::get_if<PrePrintStrategyMacroParam>(&opt.strategy);
        const bool has_adaptive_param = mp && !mp->adaptive_param.empty();
        const bool firmware_forwards = exclude_object_known;
        const bool custom_template =
            !PrinterDetector::get_bed_mesh_calibrate_gcode(printer_type_).empty();
        opt.adaptive_active = has_adaptive_param && firmware_forwards && !custom_template;
        break;
    }

    // Firmware that stores these settings itself is the authority on what each
    // toggle shows. A database default would otherwise claim a state the
    // machine does not hold, and disagree with every other client reading the
    // same printer.
    for (auto& opt : pre_print_option_set_.options) {
        auto it = firmware_option_defaults_.find(opt.id);
        if (it != firmware_option_defaults_.end()) {
            opt.default_enabled = it->second;
            opt.default_from_firmware = true;
        }
    }

    // A printer whose firmware owns timelapse declares its own option for that
    // capability in the database, and that option is the one that works: it
    // writes a firmware preference, where the plugin row writes through
    // Moonraker. Synthesising on top of it gives the user two timelapse
    // toggles, and on firmware that ships a compatibility stub for the plugin
    // API the synthesised one silently does nothing. The database wins.
    firmware_owns_timelapse_ = pre_print_option_set_.declares_capability("timelapse");

    // Timelapse: append when the moonraker-timelapse plugin reports available.
    // Strategy is RuntimeCommand with sentinel values that
    // PrintPreparationManager::start_print() recognizes (see the dispatch
    // for command_enabled / command_disabled prefixed with "timelapse:").
    // These are NOT gcode lines — start_print() routes them to
    // `api_->timelapse().set_timelapse_enabled(...)`.
    if (!firmware_owns_timelapse_ && timelapse_available) {
        PrePrintOption tl;
        tl.id = "timelapse";
        tl.label_key = "Timelapse";
        tl.category = PrePrintCategory::Monitoring;
        tl.order = 100;
        // Default reflects the global moonraker-timelapse `enabled` setting
        // (seeded at discovery via PrinterState::set_timelapse_default_enabled). The plugin
        // has no per-print concept — the toggle writes the global `enabled` at
        // print start — so defaulting to a hardcoded false silently disabled a
        // user's global timelapse on every print start (#1094).
        tl.default_enabled = timelapse_default_enabled_;
        tl.strategy_kind = PrePrintStrategyKind::RuntimeCommand;
        PrePrintStrategyRuntimeCommand cmd;
        cmd.command_enabled = "timelapse:on";
        cmd.command_disabled = "timelapse:off";
        tl.strategy = cmd;
        pre_print_option_set_.options.push_back(std::move(tl));
    }

    // Skip toggles for the leveling steps helix_skips.cfg wraps. A database
    // option for the step is the printer's own way to skip it and wins.
    for (auto op : skip_ops) {
        PrePrintOption opt = skip_wrappers::option_for(op);
        if (!pre_print_option_set_.declares_capability(opt.capability_key())) {
            pre_print_option_set_.options.push_back(std::move(opt));
        }
    }

    // Maintain the (category, order) sort guarantee from
    // parse_pre_print_option_set so renderers still see options in their
    // documented order (covers both synthesized options above).
    sort_pre_print_options(pre_print_option_set_.options);
}

} // namespace helix
