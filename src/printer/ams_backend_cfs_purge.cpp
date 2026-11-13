// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_CFS

// AmsBackendCfs purge chute: what box.cfg says about the chute, and the manual
// purge the stock dialect makes over it instead of extruding in place (#2065).

#include "ams_backend_cfs.h"
#include "toolhead_homing.h"

#include <spdlog/spdlog.h>

#include <mutex>
#include <string>

namespace helix::printer {

void AmsBackendCfs::apply_chute_config(const std::optional<ChuteWiperLine>& wiper, bool chute) {
    chute_wiper_ = wiper;
    if (wiper) {
        spdlog::info("[AMS CFS] box.cfg wiper: X {:.1f}-{:.1f} at Y {:.1f}", wiper->x_min,
                     wiper->x_max, wiper->y);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        has_chute_extrude_pos_ = chute;
    }
    spdlog::info("[AMS CFS] Purge chute position {}", chute ? "configured" : "not configured");
}

std::string AmsBackendCfs::chute_purge_gcode(int length_mm, int feedrate_mm_min) {
    // The envelope the stock operations share: state saved, part-cooling
    // suppressed, toolhead over the configured extrude position before the
    // nozzle moves, M400 so the park does not overlap the extrude, fan
    // restored before the park, then the firmware's own safe position.
    return "SAVE_GCODE_STATE NAME=helix_cfs_purge\n"
           "BOX_SAVE_FAN\n"
           "BOX_GO_TO_EXTRUDE_POS\n"
           "M83\n"
           "G1 E" +
           std::to_string(length_mm) + " F" + std::to_string(feedrate_mm_min) +
           "\nM400\n"
           "BOX_RESTORE_FAN\n"
           "BOX_MOVE_TO_SAFE_POS\n"
           "RESTORE_GCODE_STATE NAME=helix_cfs_purge";
}

std::string AmsBackendCfs::manual_purge_gcode(int length_mm, int feedrate_mm_min) const {
    // The envelope is built from the stock dialect's BOX_* commands; the Fork
    // is not known to register them.
    if (macro_variant_ == CfsMacroVariant::Fork) {
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!has_chute_extrude_pos_) {
            return {};
        }
    }
    // BOX_GO_TO_EXTRUDE_POS is an absolute XY move: unhomed, the caller's
    // in-place extrude is the only purge that cannot hit anything.
    if (!api_ || !helix::axis_is_homed(api_->printer_state(), helix::Axis::X) ||
        !helix::axis_is_homed(api_->printer_state(), helix::Axis::Y)) {
        return {};
    }
    return chute_purge_gcode(length_mm, feedrate_mm_min);
}

} // namespace helix::printer

#endif // HELIX_HAS_CFS
