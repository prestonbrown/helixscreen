// SPDX-License-Identifier: GPL-3.0-or-later

#include "stepper_resonance_calibration.h"

#include "printer_discovery.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace helix::stepper_resonance_cal {
namespace {

/// One way a firmware runs the calibration.
struct Row {
    Procedure procedure;
    bool (*detect)(const PrinterDiscovery& hw);
};

// --- Firmware wrapper: CALIBRATE_VFA ----------------------------------------
//
// Z-Mod's CALIBRATE_VFA T=<n> (c5pro_generic.cfg) mounts head n, zeroes the
// gcode offset, moves to the bed centre, runs the stock
// STEPPER_RESONANCE_FACTORY_CALIBRATE, parks the head and ends in SAVE_CONFIG.
// It homes as part of mounting, so nothing has to come first.
bool wrapper_detect(const PrinterDiscovery& hw) {
    return hw.has_macro("CALIBRATE_VFA");
}

std::string wrapper_build(const PrinterDiscovery& /*hw*/, int tool) {
    return fmt::format("CALIBRATE_VFA T={}", tool);
}

// --- The stepper_resonance_tester extra ---------------------------------------
//
// The klippy extra registers STEPPER_RESONANCE_SEARCH_CALIBRATE
// STEPPER=<stepper>, which measures one motor through the carriage
// accelerometer and stages its damping for SAVE_CONFIG. The stock
// printer.vibration.cfg wraps both XY motors in
// STEPPER_RESONANCE_FACTORY_CALIBRATE, whose own SAVE_CONFIG is commented out,
// so the result survives a restart only when the caller saves it.
constexpr const char* kTesterObject = "stepper_resonance_tester";
constexpr const char* kFactoryMacro = "STEPPER_RESONANCE_FACTORY_CALIBRATE";

bool tester_detect(const PrinterDiscovery& hw) {
    const auto& objects = hw.printer_objects();
    return std::find(objects.begin(), objects.end(), kTesterObject) != objects.end();
}

std::string tester_build(const PrinterDiscovery& hw, int /*tool*/) {
    const std::string calibrate = hw.has_macro(kFactoryMacro)
                                      ? std::string(kFactoryMacro)
                                      : std::string("STEPPER_RESONANCE_SEARCH_CALIBRATE "
                                                    "STEPPER=stepper_x\n"
                                                    "STEPPER_RESONANCE_SEARCH_CALIBRATE "
                                                    "STEPPER=stepper_y");
    return calibrate + "\nSAVE_CONFIG";
}

// --- Firmware wrapper: VFA_COMPENSATION ---------------------------------------
//
// OpenCreator's VFA_COMPENSATION (printer.vibration.cfg) refuses unless every
// head is docked, homes, parks the bare carriage at Z100/X130/Y130, runs
// STEPPER_RESONANCE_FACTORY_CALIBRATE and ends in SAVE_CONFIG. Sending the
// stock macro directly skips the dock check and the park. The macro name alone
// is not the routine: it drives the stepper_resonance_tester extra above.
bool compensation_detect(const PrinterDiscovery& hw) {
    return hw.has_macro("VFA_COMPENSATION") && tester_detect(hw);
}

std::string compensation_build(const PrinterDiscovery& /*hw*/, int /*tool*/) {
    return "VFA_COMPENSATION";
}

/// Most specific first: a wrapper that knows the machine outranks the bare
/// extra it drives.
const Row kRows[] = {
    {{"Z-Mod CALIBRATE_VFA", /*per_tool=*/true, /*needs_home=*/true,
      /*restarts_klipper=*/true, wrapper_build},
     wrapper_detect},
    {{"OpenCreator VFA_COMPENSATION", /*per_tool=*/false, /*needs_home=*/false,
      /*restarts_klipper=*/true, compensation_build},
     compensation_detect},
    {{"stepper_resonance_tester", /*per_tool=*/false, /*needs_home=*/true,
      /*restarts_klipper=*/true, tester_build},
     tester_detect},
};

} // namespace

Procedure resolve(const PrinterDiscovery& hw) {
    for (const auto& row : kRows) {
        if (row.detect(hw)) {
            return row.procedure;
        }
    }
    return {};
}

std::string calibrate_gcode(const PrinterDiscovery& hw, int tool) {
    const Procedure p = resolve(hw);
    if (!p.present() || (p.per_tool && tool < 0)) {
        return {};
    }
    return p.build(hw, tool);
}

const char* entry_name() {
    return TR_NOOP("Vertical Artifact Compensation");
}

const char* entry_description() {
    return TR_NOOP("Tune the motors to remove fine vertical lines");
}

} // namespace helix::stepper_resonance_cal
