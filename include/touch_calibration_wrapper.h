// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "touch_calibration.h"

#include <functional>
#include <lvgl.h>

namespace helix {

/// Context for the calibrated touch read callback wrapper.
/// Owned by the display backend as a value member; the calibrated read callback
/// reaches it through a file-scope handle (NOT the indev's user_data — see
/// install_calibration_wrapper). Lifetime must match the backend object.
struct CalibrationContext {
    TouchCalibration calibration;
    lv_indev_read_cb_t original_read_cb = nullptr;
    int screen_width = 800;
    int screen_height = 480;

    /// Optional source of the PRE-SWAP, PRE-SCALE digitizer reading behind the
    /// coordinate the chained read callback just produced.
    ///
    /// The evdev stage clamps to the display, so by the time a coordinate reaches
    /// here a wrong declared ABS range has already destroyed the information a
    /// calibration would need to correct it (#1259, #1276). The backends populate
    /// this with a shim over lv_evdev_get_last_raw(); tests populate it with a
    /// stub. It is a std::function rather than a direct call because lv_evdev is
    /// compiled out of desktop and test builds, so this translation unit must not
    /// name any lv_evdev_* symbol.
    ///
    /// Returns false when no raw reading is available, which is the normal state
    /// on every non-evdev backend. Everything downstream must degrade to the
    /// affine-only path in that case.
    std::function<bool(int&, int&)> raw_source;

    /// Raw reading stashed by the last calibrated_read_cb() call, valid only when
    /// last_raw_valid. Read back through get_last_raw_touch().
    Point last_raw{};
    bool last_raw_valid = false;

    /// What the backend actually programmed into lv_evdev. Filled through
    /// set_touch_pipeline_info(); default-constructed (known == false) on a
    /// backend that never recorded it.
    ///
    /// Guarded by the wrapper's diagnostics mutex - a debug bundle snapshots it
    /// from the collect worker while the LVGL main thread owns everything else
    /// here. Reach it through get_touch_range_diagnostics(), never directly.
    TouchPipelineInfo pipeline;

    /// Extremes of the raw digitizer reading over this run. Same mutex, same
    /// reason.
    TouchObservedExtremes observed;

    /// Latch so an out-of-range digitizer raises telemetry once, not once per
    /// touch. Same mutex.
    bool range_violation_reported = false;
};

/// Read callback wrapper that applies affine touch calibration.
/// Chains to original_read_cb first, then transforms coordinates.
void calibrated_read_cb(lv_indev_t* indev, lv_indev_data_t* data);

/// Load stored touch calibration coefficients from Config.
/// Returns TouchCalibration with valid=false if none stored or invalid.
TouchCalibration load_touch_calibration();

/// Read back the raw digitizer coordinate behind the most recent calibrated read.
/// Returns false when the active backend has no raw source (desktop/SDL) or no
/// touch has been seen yet, in which case `out` is untouched.
bool get_last_raw_touch(Point& out);

/// Record what the display backend actually programmed into lv_evdev.
///
/// Call it AFTER install_calibration_wrapper() - it writes into the active
/// calibration context and is a no-op when none is installed. Everything it
/// carries is already in hand at the point the backend resolves the range; a
/// bundle cannot re-derive any of it after the fact.
void set_touch_pipeline_info(const TouchPipelineInfo& info);

/// Update just the configured range after a calibration re-programs lv_evdev at
/// runtime (DisplayBackend::apply_touch_range). Also clears the observed extremes
/// and the telemetry latch: everything seen so far was measured against the range
/// that just went away, so keeping it would leave a bundle reporting a violation
/// of a range no longer in effect. No-op when no wrapper is installed.
void set_touch_configured_range(bool swap_axes, int min_x, int min_y, int max_x, int max_y,
                                TouchRangeSource source);

/// Snapshot everything a debug bundle needs about the touch pipeline.
///
/// Returns false, with `out.unavailable_reason` set and nothing else filled in,
/// when there is no calibration wrapper installed or the active one has no raw
/// digitizer source. That is the normal state on desktop/SDL and on a libinput
/// pointer, and every consumer must degrade to "unknown" rather than reporting
/// zeroes.
///
/// Safe to call from any thread: it takes the same mutex the read callback and
/// uninstall_calibration_wrapper() use, so it can neither tear a value nor reach
/// through a context that is being destroyed.
bool get_touch_range_diagnostics(TouchRangeDiagnostics& out);

/// Load the stored evdev ABS range from Config.
/// Returns valid=false when nothing is stored, which means "keep the kernel range".
TouchRangeSettings load_touch_range();

/// Persist an evdev ABS range solved by calibration (does not call Config::save()).
/// Writing an invalid range clears the stored one, so a later calibration that
/// could not solve the range never leaves a stale one behind to double up with a
/// full-pipeline affine.
void save_touch_range(const TouchRangeSettings& range);

/// Install the calibration read callback wrapper on an input device.
/// Sets up ctx with the calibration data, chains to the existing read callback,
/// and records ctx as the process-wide active calibration context (see the .cpp
/// for why user_data is deliberately not used). Safe to call even when cal.valid
/// is false (becomes a passthrough). There is only one calibrated touch indev
/// per process.
void install_calibration_wrapper(lv_indev_t* indev, CalibrationContext& ctx,
                                 const TouchCalibration& cal, int screen_w, int screen_h);

/// Tear down the wrapper installed by install_calibration_wrapper(): silence the
/// calibrated read callback on `indev` and drop the active-context handle if it
/// still refers to `ctx`. Call from the backend destructor before ctx is
/// destroyed — the indev outlives the backend (indevs are freed only at
/// lv_deinit), so its read callback must stop reaching a freed ctx.
void uninstall_calibration_wrapper(lv_indev_t* indev, CalibrationContext& ctx);

} // namespace helix
