// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#pragma once

#include "touch_calibration.h"

namespace helix {

/// The four touch-calibration operations an interactive calibration session
/// performs on the live input device. DisplayManager implements this; the
/// indirection exists so TouchCalibrationSession's backup/restore logic can be
/// unit-tested against a fake sink without a real display backend.
struct ICalibrationSink {
    virtual ~ICalibrationSink() = default;

    /// The calibration currently stored for the touch device (the affine that
    /// would be active if the affine transform were enabled).
    virtual TouchCalibration current_calibration() const = 0;

    /// Install a calibration as the active affine. Returns false if rejected
    /// (e.g. invalid coefficients).
    virtual bool apply_calibration(const TouchCalibration& cal) = 0;

    /// Disable the affine transform so the device reports pre-affine
    /// coordinates (used while capturing calibration points).
    virtual void disable_affine() = 0;

    /// Re-enable the affine transform using the device's stored calibration.
    virtual void enable_affine() = 0;

    /// Re-program the evdev stage's ABS range and axis swap.
    ///
    /// Defaulted to a refusal rather than pure-virtual: only a real evdev backend
    /// can do this, and every fake and every non-evdev display legitimately
    /// cannot. A false return means the caller must keep the affine-only result.
    ///
    /// @return true if the device accepted the new range
    virtual bool apply_touch_range(bool swap_axes, int min_x, int min_y, int max_x, int max_y) {
        (void)swap_axes;
        (void)min_x;
        (void)min_y;
        (void)max_x;
        (void)max_y;
        return false;
    }

    /// Discard the device's stored calibration, leaving it uncalibrated.
    ///
    /// Distinct from disable_affine(), which only suppresses the stored matrix:
    /// this drops it, so a later enable_affine() cannot bring it back. Needed
    /// because a session that began on an uncalibrated device still has
    /// something to undo — VERIFY installs the candidate matrix on the device so
    /// the user can test it, and apply_calibration() rejects the invalid
    /// "no calibration" value, so there is no way to put the original state back
    /// by applying it.
    virtual void clear_calibration() = 0;
};

/// Persist and install the result of an accepted three-point calibration.
///
/// A calibration now has two possible shapes, and this writes exactly ONE of
/// them so the two can never stack:
///
///  - A range fit was solved: the evdev ABS range and axis swap go to
///    `/input/touch_range/`, and only the leftover rotation/shear goes to
///    `/input/calibration/` (usually nothing at all, on a panel square to the
///    display). The range is re-programmed on the live device first, then the
///    residual affine installed on top.
///  - No range fit (no raw digitizer readings, or an implausible decomposition):
///    `cal` goes to `/input/calibration/` exactly as it did before #1259, and
///    the stored range is cleared.
///
/// Clearing the unused side is the part that matters: a stored range left behind
/// next to a full-pipeline affine would apply both stages and double the mapping.
///
/// Does NOT call Config::save() - the caller decides when to flush.
///
/// @param sink Live input device to install the result on, or null when there is
///             none (display not up, or a backend with no touch). A null sink
///             persists the affine-only shape, which is what an install that
///             cannot re-program its evdev stage has to fall back to anyway.
/// @param cal The affine over the CURRENT evdev range (TouchCalibrationPanel::get_calibration)
/// @param fit The range decomposition of that same mapping (TouchCalibrationPanel::get_range_fit)
/// @return true if the live device accepted the new mapping
bool commit_calibration_result(ICalibrationSink* sink, const TouchCalibration& cal,
                               const TouchRangeFit& fit);

/// Shared backup/disable/restore bookkeeping for an interactive touch
/// calibration session, used by both the first-run wizard
/// (WizardTouchCalibrationStep) and the Settings recalibration overlay
/// (TouchCalibrationOverlay).
///
/// Centralizing it guarantees the one invariant that matters: however a session
/// ends — accepted, cancelled, timed out, or dismissed by navigating away — the
/// affine transform is re-enabled, so an aborted recalibration never leaves
/// touch uncalibrated until the next reboot (prestonbrown/helixscreen#943). The
/// Settings overlay previously re-enabled the affine only in cleanup(), which
/// the navigation stack never calls on a plain dismiss, so a failed recalibrate
/// left the panel's touch input disabled until a restart.
class TouchCalibrationSession {
  public:
    /// Snapshot the calibration active now and disable the affine transform so
    /// the session can capture raw (pre-affine) coordinates. Re-snapshots on
    /// every call: a session always begins from a clean baseline.
    void begin_capture(ICalibrationSink& sink);

    /// Revert to the snapshot (if a valid one is held) and disable the affine
    /// transform again, ready for another capture attempt (retry / timeout /
    /// fast-revert).
    void revert_for_retry(ICalibrationSink& sink);

    /// The user accepted/persisted a new calibration: drop the snapshot so a
    /// later restore() will not revert their new calibration.
    void commit();

    /// Re-enable the affine transform, reverting to the snapshot if one is still
    /// held. This is the teardown guarantee — idempotent and safe to call when
    /// no session is active.
    void restore(ICalibrationSink& sink);

    bool has_backup() const {
        return has_backup_;
    }

    /// The calibration snapshotted at the last begin_capture() — valid=false
    /// once committed, restored, or when no session ever began. Read-only
    /// access for callers that need the pre-session MAPPING without touching
    /// the device: capture markers (#1082) are drawn where a press lands under
    /// this calibration, because raw capture space is not screen space on
    /// over-reporting digitizers (Qidi Q2, #943). Mutating the device still
    /// goes through the ICalibrationSink.
    const TouchCalibration& backup() const {
        return backup_;
    }

  private:
    TouchCalibration backup_{};
    bool has_backup_ = false;
};

} // namespace helix
