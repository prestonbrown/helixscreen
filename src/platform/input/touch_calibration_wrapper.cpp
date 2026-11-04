// SPDX-License-Identifier: GPL-3.0-or-later

#include "touch_calibration_wrapper.h"

#include "config.h"
#include "display_backend.h"
#include "helix_display_telemetry.h"
#include "pointer_frame_hook.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <mutex>
#include <string>

namespace helix {

namespace {

// Handle to the active calibration context for calibrated_read_cb().
//
// The read callback is a plain C function pointer and only receives lv_indev_t*,
// so it needs some way to reach the CalibrationContext. It deliberately does NOT
// use the indev's user_data: that slot lives inside the heap-allocated lv_indev_t
// struct, next to churny allocations (XML strings), and a corrupted/reused slot
// returned a non-null garbage pointer that crashed the read/disable paths
// (bundle LG9X482B, prestonbrown/helixscreen#1112). This handle lives in our own
// static storage instead. There is only ever one calibrated touch indev per
// process, and it is installed/torn down on the main (LVGL) thread, the same
// thread the read callback runs on, so the read path needs no synchronization to
// reach it. Writes are still taken under s_diag_mutex below, because a debug
// bundle reads the handle from a worker thread.
CalibrationContext* s_active_ctx = nullptr;

// Guards s_active_ctx and the diagnostics state behind it (pipeline, observed,
// range_violation_reported) plus the span-check latch below.
//
// Everything that writes those runs on the LVGL main thread, but a debug bundle
// snapshots them from the collect worker (DebugBundleCollector::collect() runs on
// an HttpExecutor lane for upload_async). Without the lock that is both a torn
// read and a read through a context the backend destructor is in the middle of
// freeing. uninstall_calibration_wrapper() clears s_active_ctx under this same
// lock, so a snapshot either finishes first or sees nothing at all.
//
// The read callback takes it once per CHANGED digitizer reading, not once per
// call, so this is a few dozen uncontended locks a second at most.
std::mutex s_diag_mutex;

/// Replace a disproved DisplaySize range with the declared one, live and stored.
///
/// The declared range is native digitizer space (lv_evdev scales into the
/// display's native resolution and rotation runs after it), so it is stored
/// stamped with capture rotation 0 and the next boot programs it at any rotation.
void correct_transposed_guess(CalibrationContext& ctx, const TouchRangeSettings& range,
                              const std::string& evidence) {
    ctx.reprogram_range(range.min_x, range.min_y, range.max_x, range.max_y);
    save_touch_range(range);
    Config::get_instance()->save();
    spdlog::warn("[TouchCal] Digitizer reached past the display on a transposed-looking ABS "
                 "range, so the range is the panel's own - re-programmed and stored X({}..{}) "
                 "Y({}..{}) ({})",
                 range.min_x, range.max_x, range.min_y, range.max_y, evidence);
}

/// Fold one CHANGED raw digitizer reading into the observed extremes, correct a
/// DisplaySize range the reading disproves, and otherwise raise telemetry the
/// first time one of them proves the configured range wrong.
///
/// Out-of-range is the only signal that fires telemetry. A compressed span is
/// reported in the debug bundle and never judged here: a user who simply never
/// touched the edges of the panel produces exactly that on healthy hardware.
void note_raw_sample(CalibrationContext& ctx, int raw_x, int raw_y) {
    std::string detail;
    TouchRangeSettings corrected;
    {
        std::lock_guard<std::mutex> lock(s_diag_mutex);
        ctx.observed.observe(raw_x, raw_y);
        TouchPipelineInfo& pipe = ctx.pipeline;

        // Judged per sample, not on the extremes, which would keep one outlier
        // voting forever.
        TouchObservedExtremes sample;
        sample.observe(raw_x, raw_y);
        if (ctx.reprogram_range && !ctx.capture_active &&
            transposed_range_guess_disproved(sample, pipe) &&
            ++ctx.transposed_guess_votes >= kTransposedGuessCorroboration) {
            detail = fmt::format("cfg_x={}..{} cfg_y={}..{} obs_x={}..{} obs_y={}..{} samples={}",
                                 pipe.min_x, pipe.max_x, pipe.min_y, pipe.max_y, ctx.observed.min_x,
                                 ctx.observed.max_x, ctx.observed.min_y, ctx.observed.max_y,
                                 ctx.observed.distinct_samples);
            corrected.valid = true;
            corrected.swap_axes = pipe.swap_axes;
            corrected.min_x = pipe.declared_min_x;
            corrected.max_x = pipe.declared_max_x;
            corrected.min_y = pipe.declared_min_y;
            corrected.max_y = pipe.declared_max_y;
            corrected.capture_rotation = 0;
            // Recorded as Stored because it now is: a recalibration that installs
            // no range of its own keeps a live stored range, and drops anything
            // else.
            pipe.source = TouchRangeSource::Stored;
            pipe.configured_valid = true;
            pipe.min_x = corrected.min_x;
            pipe.max_x = corrected.max_x;
            pipe.min_y = corrected.min_y;
            pipe.max_y = corrected.max_y;
            pipe.stored = corrected;
        } else if (!ctx.range_violation_reported) {
            const TouchRangeViolation violation = touch_range_violation(ctx.observed, pipe);
            if (!violation.any()) {
                return;
            }
            ctx.range_violation_reported = true;

            const TouchObservedExtremes seen =
                touch_observed_in_configured_axes(ctx.observed, pipe.swap_axes);
            detail =
                fmt::format("axis={} src={} swap={} cfg_x={}..{} obs_x={}..{} "
                            "cfg_y={}..{} obs_y={}..{} samples={} dev={}",
                            violation.x && violation.y ? "xy" : (violation.x ? "x" : "y"),
                            touch_range_source_name(pipe.source), pipe.swap_axes, pipe.min_x,
                            pipe.max_x, seen.min_x, seen.max_x, pipe.min_y, pipe.max_y, seen.min_y,
                            seen.max_y, ctx.observed.distinct_samples, pipe.device_name);
        } else {
            return;
        }
    }

    // Outside the lock: nothing below needs a consistent view of the
    // diagnostics, and the telemetry bridge takes TelemetryManager's own mutex.
    if (corrected.valid) {
        correct_transposed_guess(ctx, corrected, detail);
        return;
    }
    spdlog::warn("[TouchCal] Digitizer emitted a reading outside the configured evdev range - "
                 "the declared range cannot be what this panel produces ({})",
                 detail);
    helix_display_telemetry_error("display", "touch-range-declared-too-narrow", detail.c_str());
}

/// A driver point with the calibration in @p ctx applied. The point is PANEL space,
/// ahead of lv_display_rotate_point(); the affine was solved against logical,
/// post-rotation targets, so it is placed rather than applied directly.
Point calibrate_panel_point(const CalibrationContext& ctx, Point panel_point) {
    return apply_calibration_in_panel_space(ctx.calibration, panel_point,
                                            ctx.calibration.capture_rotation, ctx.screen_width,
                                            ctx.screen_height);
}

} // namespace

void calibrated_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    CalibrationContext* ctx = s_active_ctx;
    if (!ctx) {
        return;
    }

    // Call the original evdev read callback first
    if (ctx->original_read_cb) {
        ctx->original_read_cb(indev, data);
    }

    // Corroboration against a DisplaySize range counts within one press. Only
    // this thread writes the count, so the unlocked read cannot miss a vote.
    if (data->state == LV_INDEV_STATE_RELEASED && ctx->transposed_guess_votes != 0) {
        std::lock_guard<std::mutex> lock(s_diag_mutex);
        ctx->transposed_guess_votes = 0;
    }

    // Stash the pre-swap, pre-scale digitizer reading behind the coordinate we
    // just got, before the affine below rewrites it. Calibration needs this to
    // solve for the evdev range; the clamped coordinate alone cannot tell it what
    // the true range was (#1259, #1276).
    if (ctx->raw_source) {
        int rx = 0;
        int ry = 0;
        if (ctx->raw_source(rx, ry)) {
            // lv_evdev_get_last_raw() keeps serving the last reading until a new
            // event arrives, and this callback runs at the indev poll rate. Only
            // a CHANGED reading is a sample: counting calls would report hundreds
            // of them for one press and make the count worthless as the
            // confidence signal a bundle reader needs.
            //
            // Only a PRESSED reading is a sample. lv_evdev reports root (0,0) with
            // has_last_raw set on its first poll, before any EV_ABS arrives, and a
            // declared range that does not include 0 flags that as out-of-range.
            const bool changed =
                !ctx->last_raw_valid || rx != ctx->last_raw.x || ry != ctx->last_raw.y;
            ctx->last_raw = helix::Point{rx, ry};
            ctx->last_raw_valid = true;
            if (changed && data->state == LV_INDEV_STATE_PRESSED) {
                note_raw_sample(*ctx, rx, ry);
            }
        }
    }

    // Apply affine calibration if valid (for both PRESSED and RELEASED states)
    if (ctx->calibration.valid) {
        helix::Point raw{static_cast<int>(data->point.x), static_cast<int>(data->point.y)};
        helix::Point transformed = calibrate_panel_point(*ctx, raw);
        data->point.x = transformed.x;
        data->point.y = transformed.y;

        if (helix::is_touch_debug_enabled() && data->state == LV_INDEV_STATE_PRESSED) {
            static int touch_debug_counter = 0;
            if (++touch_debug_counter % 50 == 1) {
                spdlog::warn("[TouchDebug] calibrated_read: raw=({},{}) -> screen=({},{}) "
                             "[sample #{}]",
                             raw.x, raw.y, transformed.x, transformed.y, touch_debug_counter);
            }
        }
    }
}

lv_point_t map_touch_point_to_screen(lv_indev_t* indev, lv_point_t driver_point) {
    Point point{static_cast<int>(driver_point.x), static_cast<int>(driver_point.y)};
    // One calibrated touch indev per process (see s_active_ctx), and a multi-touch
    // gesture only ever comes from that touch panel.
    if (const CalibrationContext* ctx = s_active_ctx) {
        point = calibrate_panel_point(*ctx, point);
    }
    const PointerXY turned = PointerFrameHook::map_panel_point(indev, PointerXY{point.x, point.y});
    lv_point_t screen{turned.x, turned.y};
    if (lv_display_t* disp = lv_indev_get_display(indev)) {
        lv_display_rotate_point(disp, &screen);
    }
    return screen;
}

void set_touch_pipeline_info(const TouchPipelineInfo& info) {
    std::lock_guard<std::mutex> lock(s_diag_mutex);
    if (!s_active_ctx) {
        spdlog::debug("[TouchCal] Pipeline info discarded - no calibration wrapper installed");
        return;
    }
    s_active_ctx->pipeline = info;
}

void set_touch_configured_range(bool swap_axes, int min_x, int min_y, int max_x, int max_y,
                                TouchRangeSource source) {
    std::lock_guard<std::mutex> lock(s_diag_mutex);
    if (!s_active_ctx) {
        return;
    }
    TouchPipelineInfo& pipeline = s_active_ctx->pipeline;
    pipeline.source = source;
    pipeline.configured_valid = true;
    pipeline.swap_axes = swap_axes;
    pipeline.min_x = min_x;
    pipeline.max_x = max_x;
    pipeline.min_y = min_y;
    pipeline.max_y = max_y;

    // Everything observed so far was measured against the OLD range, so keeping
    // it would leave a bundle claiming a violation of a range that is no longer
    // programmed.
    s_active_ctx->observed = TouchObservedExtremes{};
    s_active_ctx->range_violation_reported = false;
}

void set_touch_capture_active(bool active) {
    std::lock_guard<std::mutex> lock(s_diag_mutex);
    if (s_active_ctx) {
        s_active_ctx->capture_active = active;
    }
}

bool get_touch_range_diagnostics(TouchRangeDiagnostics& out) {
    out = TouchRangeDiagnostics{};

    std::lock_guard<std::mutex> lock(s_diag_mutex);
    CalibrationContext* ctx = s_active_ctx;
    if (!ctx) {
        out.unavailable_reason = "no-touch-backend";
        return false;
    }
    if (!ctx->raw_source) {
        // A pointer with no evdev stage behind it: SDL on the desktop, or a
        // libinput fallback on DRM. Nothing here has a meaning there.
        out.unavailable_reason = "no-raw-source";
        return false;
    }

    out.available = true;
    out.pipeline = ctx->pipeline;
    out.observed = ctx->observed;
    out.affine_valid = ctx->calibration.valid;
    out.span_check_seen = get_touch_span_check(out.span_check_x, out.span_check_y);
    return true;
}

bool get_last_raw_touch(Point& out) {
    CalibrationContext* ctx = s_active_ctx;
    if (!ctx || !ctx->last_raw_valid) {
        return false;
    }
    out = ctx->last_raw;
    return true;
}

TouchRangeSettings load_touch_range() {
    TouchRangeSettings range;
    helix::Config* cfg = helix::Config::get_instance();

    if (!cfg->get<bool>("/input/touch_range/valid", false)) {
        return range;
    }

    range.swap_axes = cfg->get<bool>("/input/touch_range/swap_axes", false);
    range.min_x = cfg->get<int>("/input/touch_range/min_x", 0);
    range.max_x = cfg->get<int>("/input/touch_range/max_x", 0);
    range.min_y = cfg->get<int>("/input/touch_range/min_y", 0);
    range.max_y = cfg->get<int>("/input/touch_range/max_y", 0);
    // A range saved without its own stamp was saved by the same commit that
    // stamped the calibration record.
    range.capture_rotation =
        cfg->get<int>("/input/touch_range/rotation",
                      cfg->get<int>("/input/calibration/rotation", range.capture_rotation));

    // min == max is not a passthrough in lv_evdev: the scale is skipped but the
    // clamp is not, so every coordinate would collapse onto one pixel. Refuse it.
    if (range.min_x == range.max_x || range.min_y == range.max_y) {
        spdlog::warn("[TouchCal] Stored touch range is degenerate X({}..{}) Y({}..{}) - ignoring",
                     range.min_x, range.max_x, range.min_y, range.max_y);
        return TouchRangeSettings{};
    }

    range.valid = true;
    return range;
}

void save_touch_range(const TouchRangeSettings& range) {
    helix::Config* cfg = helix::Config::get_instance();

    cfg->set<bool>("/input/touch_range/valid", range.valid);
    if (!range.valid) {
        // Leave the numbers alone but make sure nothing reads them. A calibration
        // that could not solve the range stores a full-pipeline affine instead,
        // and an old range surviving alongside it would apply both.
        spdlog::info("[TouchCal] Stored touch range cleared (no range fit this run)");
        return;
    }

    cfg->set<bool>("/input/touch_range/swap_axes", range.swap_axes);
    cfg->set<int>("/input/touch_range/min_x", range.min_x);
    cfg->set<int>("/input/touch_range/max_x", range.max_x);
    cfg->set<int>("/input/touch_range/min_y", range.min_y);
    cfg->set<int>("/input/touch_range/max_y", range.max_y);
    cfg->set<int>("/input/touch_range/rotation", range.capture_rotation);
    spdlog::info("[TouchCal] Touch range saved: swap={} X({}..{}) Y({}..{}) rotation={}",
                 range.swap_axes, range.min_x, range.max_x, range.min_y, range.max_y,
                 range.capture_rotation);
}

TouchCalibration load_touch_calibration() {
    helix::Config* cfg = helix::Config::get_instance();
    TouchCalibration cal;

    cal.valid = cfg->get<bool>("/input/calibration/valid", false);
    // Every save writes the rotation key (touch_calibration_session.cpp#write_affine),
    // so a record without one states no basis. Zero reproduces what such a record did
    // when it was written - the matrix was handed panel-space points directly - and it
    // is the exact answer for as long as nothing rotates underneath it. Taking the
    // rotation in effect NOW instead would re-place a working matrix into a basis it
    // was never solved in, a quarter turn out for anyone who rotated the display after
    // calibrating.
    const bool has_capture_rotation = cfg->exists("/input/calibration/rotation");
    cal.capture_rotation = cfg->get<int>("/input/calibration/rotation", cal.capture_rotation);
    if (!cal.valid) {
        // A stored evdev range IS a stored user calibration, even when it left no
        // affine behind - the common outcome on a panel square to the display, where
        // the range carries the whole mapping and the residual is an identity. The
        // platform default is a whole second mapping; composing it on top of a range
        // the user just measured would undo the calibration they ran (#1259, #1276).
        if (load_touch_range().valid) {
            spdlog::info("[TouchCal] Stored touch range in effect with no residual affine - "
                         "platform default not applied");
            return cal;
        }

        // Fall back to the panel default this package was built for, so touch is
        // usable on first boot instead of only after the wizard's tap routine.
        // A stored user calibration always wins: we only get here when there is none.
        TouchCalibration platform_cal = platform_default_calibration();
        if (platform_cal.valid) {
            spdlog::info("[TouchCal] Using platform default calibration "
                         "(no stored calibration yet)");
            return platform_cal;
        }
        spdlog::debug("[TouchCal] No valid calibration in config");
        return cal;
    }

    cal.a = static_cast<float>(cfg->get<double>("/input/calibration/a", 1.0));
    cal.b = static_cast<float>(cfg->get<double>("/input/calibration/b", 0.0));
    cal.c = static_cast<float>(cfg->get<double>("/input/calibration/c", 0.0));
    cal.d = static_cast<float>(cfg->get<double>("/input/calibration/d", 0.0));
    cal.e = static_cast<float>(cfg->get<double>("/input/calibration/e", 1.0));
    cal.f = static_cast<float>(cfg->get<double>("/input/calibration/f", 0.0));

    if (!helix::is_calibration_valid(cal)) {
        spdlog::warn("[TouchCal] Stored calibration failed validation");
        cal.valid = false;
        return cal;
    }

    // A rotated display turns the zero above from a faithful reading into a guess,
    // and the record's real basis is unrecoverable. Drop it and let the user pay one
    // recalibration instead of owning a screen they cannot navigate to fix. Asked of
    // the display rather than `/display/rotate` for the reasons on
    // display_rotation_degrees(); both backends call this from create_input_pointer(),
    // after DisplayManager has applied rotation.
    const int applied_rotation = display_rotation_degrees();
    if (helix::should_invalidate_unstamped_calibration(has_capture_rotation, applied_rotation)) {
        spdlog::warn("[TouchCal] Discarding a stored calibration that records no capture "
                     "rotation on a {}°-rotated display - recalibration required "
                     "(prestonbrown/helixscreen#1394)",
                     applied_rotation);
        cal.valid = false;
        cfg->set<bool>("/input/calibration/valid", false);
        cfg->save();
        return cal;
    }

    spdlog::info("[TouchCal] Loaded calibration: a={:.4f} b={:.4f} c={:.4f} d={:.4f} "
                 "e={:.4f} f={:.4f}",
                 cal.a, cal.b, cal.c, cal.d, cal.e, cal.f);

    return cal;
}

void install_calibration_wrapper(lv_indev_t* indev, CalibrationContext& ctx,
                                 const TouchCalibration& cal, int screen_w, int screen_h) {
    ctx.calibration = cal;
    ctx.original_read_cb = lv_indev_get_read_cb(indev);
    ctx.screen_width = screen_w;
    ctx.screen_height = screen_h;

    {
        std::lock_guard<std::mutex> lock(s_diag_mutex);
        s_active_ctx = &ctx;
    }
    // A fresh device session. A span check latched by a previous one describes a
    // calibration that is no longer the one in effect.
    clear_touch_span_check();
    lv_indev_set_read_cb(indev, helix::calibrated_read_cb);
}

void uninstall_calibration_wrapper(lv_indev_t* indev, CalibrationContext& ctx) {
    // Silence our callback on this indev so it can't fire against a freed ctx
    // between backend destruction and lv_deinit (prestonbrown/helixscreen#1102).
    // Only touch the read_cb if it's still ours — a newer backend may have taken
    // over the (different) live indev.
    if (indev && lv_indev_get_read_cb(indev) == helix::calibrated_read_cb) {
        lv_indev_set_read_cb(indev, nullptr);
    }
    // Under the diagnostics lock so a debug bundle collecting on a worker thread
    // cannot be halfway through reading ctx when the backend destructor frees it.
    std::lock_guard<std::mutex> lock(s_diag_mutex);
    if (s_active_ctx == &ctx) {
        s_active_ctx = nullptr;
    }
}

} // namespace helix
