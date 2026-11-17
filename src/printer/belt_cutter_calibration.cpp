// SPDX-License-Identifier: GPL-3.0-or-later

#include "belt_cutter_calibration.h"

#include "printer_discovery.h"
#include "translation_loader.h"

#include <algorithm>
#include <string_view>

namespace helix::belt_cutter_cal {
namespace {

bool has_object(const PrinterDiscovery& hw, std::string_view name) {
    const auto& objects = hw.printer_objects();
    return std::find(objects.begin(), objects.end(), name) != objects.end();
}

bool has_object_with_prefix(const PrinterDiscovery& hw, std::string_view prefix) {
    const auto& objects = hw.printer_objects();
    return std::any_of(objects.begin(), objects.end(), [prefix](const std::string& name) {
        return std::string_view(name).substr(0, prefix.size()) == prefix;
    });
}

/// One firmware's way of running both routines. A null builder or gcode means
/// that firmware lacks the routine; the predicates say whether this printer's
/// configuration has the hardware for it.
struct Row {
    const char* name;
    bool (*detect)(const PrinterDiscovery& hw);
    bool (*has_belt_tensioners)(const PrinterDiscovery& hw);
    std::string (*belt_gcode)(BeltAxes axes);
    bool (*has_cutter_calibration)(const PrinterDiscovery& hw);
    const char* cutter_gcode;
};

// --- Kalico port of the Creality K2 (Jacob10383/k2-plus-custom-firmware) ------
//
// extras/belt_mdl.py loads one `[belt_mdl x]` / `[belt_mdl y]` section per
// tensioner and registers BELT_TENSION_SET AXIS=X|Y, which homes X/Y, sweeps the
// carriage to settle the belt, adjusts it to TENSION and releases the motors.
// macros.cfg wraps it: BELT_TENSION [AXES=XY] runs BELT_TENSION_SET at 140 for
// each axis named in AXES (default both).
//
// extras/motor_control.py registers CALIBRATE_CUT_POS as a Python command, so it
// is no gcode_macro: it homes XY if needed, refuses with filament loaded, finds
// the cutter hit and stores cut_pos_x under [box]. motor_control and box are the
// objects it needs.
constexpr std::string_view kBeltModulePrefix = "belt_mdl ";
constexpr const char* kKalicoBeltMacro = "BELT_TENSION";

bool kalico_k2_detect(const PrinterDiscovery& hw) {
    return hw.is_kalico() &&
           (has_object_with_prefix(hw, kBeltModulePrefix) || has_object(hw, "motor_control"));
}

bool kalico_k2_has_belt_tensioners(const PrinterDiscovery& hw) {
    return has_object_with_prefix(hw, kBeltModulePrefix) && hw.has_macro(kKalicoBeltMacro);
}

std::string kalico_k2_belt_gcode(BeltAxes axes) {
    switch (axes) {
    case BeltAxes::X:
        return "BELT_TENSION AXES=X";
    case BeltAxes::Y:
        return "BELT_TENSION AXES=Y";
    case BeltAxes::Both:
        break;
    }
    return "BELT_TENSION";
}

bool kalico_k2_has_cutter_calibration(const PrinterDiscovery& hw) {
    return has_object(hw, "motor_control") && has_object(hw, "box");
}

const Row kRows[] = {
    {"Kalico K2 port", kalico_k2_detect, kalico_k2_has_belt_tensioners, kalico_k2_belt_gcode,
     kalico_k2_has_cutter_calibration, "CALIBRATE_CUT_POS"},
};

const Row* resolve(const PrinterDiscovery& hw) {
    for (const auto& row : kRows) {
        if (row.detect(hw)) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

bool can_tension_belts(const PrinterDiscovery& hw) {
    const Row* row = resolve(hw);
    return row && row->belt_gcode && row->has_belt_tensioners(hw);
}

std::string belt_tension_gcode(const PrinterDiscovery& hw, BeltAxes axes) {
    if (!can_tension_belts(hw)) {
        return {};
    }
    return resolve(hw)->belt_gcode(axes);
}

bool can_calibrate_cutter(const PrinterDiscovery& hw) {
    const Row* row = resolve(hw);
    return row && row->cutter_gcode && row->has_cutter_calibration(hw);
}

std::string cutter_calibration_gcode(const PrinterDiscovery& hw) {
    if (!can_calibrate_cutter(hw)) {
        return {};
    }
    return resolve(hw)->cutter_gcode;
}

const char* provider_name(const PrinterDiscovery& hw) {
    const Row* row = resolve(hw);
    return row ? row->name : "";
}

const char* belt_entry_name() {
    return TR_NOOP("Auto Belt Tension");
}

const char* belt_entry_description() {
    return TR_NOOP("Let the printer's tensioners set the belts");
}

const char* cutter_entry_name() {
    return TR_NOOP("Cutter Calibration");
}

const char* cutter_entry_description() {
    return TR_NOOP("Find the filament cutter's position");
}

} // namespace helix::belt_cutter_cal
