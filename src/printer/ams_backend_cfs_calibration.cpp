// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_CFS

// AmsBackendCfs calibration commands for the K2 and the send they share with the
// K1 cutter sweep. A K2 has none of the K1's BOX_* calibration commands; its
// cutter and belt buttons send the printer's own CALIBRATE_CUT_POS and
// BELT_TENSION macros, and exist only when the printer defines them; the belt
// button also needs the K2's motorized tensioners (#2059).

#include "ams_backend_cfs.h"
#include "macro_param_cache.h"
#include "moonraker_error.h"
#include "printer_discovery.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace helix::printer {

namespace {
constexpr const char* kK2CutterMacro = "CALIBRATE_CUT_POS";
constexpr const char* kK2BeltTensionMacro = "BELT_TENSION";
// Klipper object prefix of the K2's motorized belt tensioners (mdlx, mdly).
constexpr std::string_view kK2BeltMotorPrefix = "belt_mdl ";
} // namespace

void AmsBackendCfs::set_discovery(const helix::PrinterDiscovery& discovery) {
    const auto& objects = discovery.printer_objects();
    has_belt_motors_ = std::any_of(objects.begin(), objects.end(), [](const std::string& name) {
        return name.rfind(kK2BeltMotorPrefix, 0) == 0;
    });
    spdlog::debug("[AMS CFS] Motorized belt tensioners: {}", has_belt_motors_);
}

std::string AmsBackendCfs::cutter_calibration_gcode() const {
    if (macro_variant_ == CfsMacroVariant::K1) {
        return "BOX_FIND_CUT_POS";
    }
    if (helix::MacroParamCache::instance().has_macro(kK2CutterMacro)) {
        return kK2CutterMacro;
    }
    return {};
}

bool AmsBackendCfs::cutter_calibration_takes_a_minute() const {
    return macro_variant_ == CfsMacroVariant::K1;
}

bool AmsBackendCfs::can_tension_belts() const {
    return macro_variant_ != CfsMacroVariant::K1 && has_belt_motors_ &&
           helix::MacroParamCache::instance().has_macro(kK2BeltTensionMacro);
}

AmsError AmsBackendCfs::run_calibration_command(
    const std::string& gcode, std::function<void(bool ok, const std::string& detail)> on_done) {
    if (auto e = refuse_motion_if_printing(); !e.success()) {
        return e;
    }
    if (calibration_in_flight_) {
        return AmsErrorHelper::busy("calibration");
    }
    auto token = lifetime_.token();
    auto on_complete = [this, on_done]() {
        calibration_in_flight_ = false;
        if (on_done) {
            on_done(true, {});
        }
    };
    auto on_error = [this, token, on_done](const MoonrakerError& err) {
        token.defer("AmsBackendCfs::calibration_err", [this, on_done, err]() {
            calibration_in_flight_ = false;
            if (on_done) {
                on_done(false, err.message);
            }
        });
    };
    // Held before the send: a completion that answers synchronously must find
    // the lock taken so that it can release it.
    calibration_in_flight_ = true;
    const AmsError err = execute_gcode(gcode, std::move(on_complete), std::move(on_error));
    if (!err.success()) {
        calibration_in_flight_ = false;
    }
    spdlog::info("[AMS CFS] Calibration {} {}", gcode, err.success() ? "sent" : "refused");
    return err;
}

AmsError
AmsBackendCfs::tension_belts(std::function<void(bool ok, const std::string& detail)> on_result) {
    if (!can_tension_belts()) {
        return AmsErrorHelper::not_supported("Belt tensioning");
    }
    return run_calibration_command(kK2BeltTensionMacro, std::move(on_result));
}

} // namespace helix::printer

#endif // HELIX_HAS_CFS
