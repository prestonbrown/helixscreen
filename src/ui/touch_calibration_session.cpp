// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#include "touch_calibration_session.h"

#include "config.h"
#include "touch_calibration_wrapper.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {

void write_affine(Config& cfg, const TouchCalibration& cal) {
    cfg.set<bool>("/input/calibration/valid", cal.valid);
    cfg.set<double>("/input/calibration/a", static_cast<double>(cal.a));
    cfg.set<double>("/input/calibration/b", static_cast<double>(cal.b));
    cfg.set<double>("/input/calibration/c", static_cast<double>(cal.c));
    cfg.set<double>("/input/calibration/d", static_cast<double>(cal.d));
    cfg.set<double>("/input/calibration/e", static_cast<double>(cal.e));
    cfg.set<double>("/input/calibration/f", static_cast<double>(cal.f));
}

} // namespace

bool commit_calibration_result(ICalibrationSink* sink, const TouchCalibration& cal,
                               const TouchRangeFit& fit) {
    // Re-program the evdev stage first. A backend that cannot (no evdev, or a
    // degenerate range) says so, and the whole commit falls back to the affine-only
    // shape rather than persisting a range nothing honours.
    const bool range_installed =
        fit.valid && sink != nullptr &&
        sink->apply_touch_range(fit.swap_axes, fit.min_x, fit.min_y, fit.max_x, fit.max_y);

    // Exactly one of these two describes the mapping from here on.
    const TouchCalibration affine = range_installed ? fit.residual : cal;

    if (Config* cfg = Config::get_instance()) {
        TouchRangeSettings range;
        range.valid = range_installed;
        range.swap_axes = fit.swap_axes;
        range.min_x = fit.min_x;
        range.max_x = fit.max_x;
        range.min_y = fit.min_y;
        range.max_y = fit.max_y;
        save_touch_range(range);
        write_affine(*cfg, affine);
    } else {
        spdlog::error("[TouchCalSession] Config not available - calibration not persisted");
    }

    bool applied = false;
    if (sink != nullptr) {
        if (affine.valid) {
            applied = sink->apply_calibration(affine);
        } else {
            // The range carries the whole mapping. Drop whatever affine the device
            // still holds instead of leaving the pre-session one composed on top of
            // a range that no longer matches it.
            sink->clear_calibration();
            applied = true;
        }
    }

    const char* affine_kind = !affine.valid ? "none" : (range_installed ? "residual" : "full");
    spdlog::info("[TouchCalSession] Calibration committed: evdev range {}, affine {}, applied={}",
                 range_installed ? "re-programmed" : "left as the kernel declared it", affine_kind,
                 applied);
    return applied;
}

void TouchCalibrationSession::begin_capture(ICalibrationSink& sink) {
    backup_ = sink.current_calibration();
    has_backup_ = true;
    sink.disable_affine();
    spdlog::debug("[TouchCalSession] begin_capture: backup snapshotted (valid={}), affine disabled",
                  backup_.valid);
}

void TouchCalibrationSession::revert_for_retry(ICalibrationSink& sink) {
    bool reverted = has_backup_ && backup_.valid;
    if (reverted) {
        sink.apply_calibration(backup_);
    } else if (has_backup_) {
        // The session began on an uncalibrated device, so there is no backup to
        // re-apply — but the device is no longer uncalibrated: VERIFY installed
        // the candidate matrix in the stored slot. disable_affine() below only
        // suppresses it, so the next enable_affine() would install a matrix the
        // user never accepted. Drop it instead.
        sink.clear_calibration();
    }
    sink.disable_affine();
    spdlog::debug("[TouchCalSession] revert_for_retry: restored backup={}, affine disabled",
                  reverted);
}

void TouchCalibrationSession::commit() {
    has_backup_ = false;
    backup_ = {};
    spdlog::debug("[TouchCalSession] commit: backup dropped (new calibration kept)");
}

void TouchCalibrationSession::restore(ICalibrationSink& sink) {
    bool reverted = has_backup_ && backup_.valid;
    if (reverted) {
        sink.apply_calibration(backup_);
    } else if (has_backup_) {
        // No prior calibration existed, so "restore" means "make the device
        // uncalibrated again", not "leave whatever is stored". VERIFY puts the
        // candidate matrix in the stored slot so the user can test it; without
        // this the enable_affine() below would make that unaccepted matrix live
        // for the rest of the session — the same shape of bug as the original
        // #943 report, where a failed recalibration changed touch until reboot.
        sink.clear_calibration();
    }
    // Always re-enable the affine transform, even when no valid backup was held
    // (first-run wizard) or no session was active — touch must never be left in
    // the disabled state a capture session puts it in (#943). With the stored
    // calibration cleared above, "enabled" resolves to the uncalibrated
    // pass-through the device started from.
    sink.enable_affine();
    has_backup_ = false;
    backup_ = {};
    spdlog::debug("[TouchCalSession] restore: restored backup={}, affine re-enabled", reverted);
}

} // namespace helix
