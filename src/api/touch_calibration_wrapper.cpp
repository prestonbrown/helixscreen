// SPDX-License-Identifier: GPL-3.0-or-later

#include "touch_calibration_wrapper.h"

#include "config.h"

#include <spdlog/spdlog.h>

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
// process, and it is installed/torn down on the main (LVGL) thread — the same
// thread the read callback runs on — so a plain pointer needs no synchronization.
CalibrationContext* s_active_ctx = nullptr;

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

    // Stash the pre-swap, pre-scale digitizer reading behind the coordinate we
    // just got, before the affine below rewrites it. Calibration needs this to
    // solve for the evdev range; the clamped coordinate alone cannot tell it what
    // the true range was (#1259, #1276).
    if (ctx->raw_source) {
        int rx = 0;
        int ry = 0;
        if (ctx->raw_source(rx, ry)) {
            ctx->last_raw = helix::Point{rx, ry};
            ctx->last_raw_valid = true;
        }
    }

    // Apply affine calibration if valid (for both PRESSED and RELEASED states)
    if (ctx->calibration.valid) {
        helix::Point raw{static_cast<int>(data->point.x), static_cast<int>(data->point.y)};
        helix::Point transformed = helix::transform_point(
            ctx->calibration, raw, ctx->screen_width - 1, ctx->screen_height - 1);
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
    if (!cfg) {
        return range;
    }

    if (!cfg->get<bool>("/input/touch_range/valid", false)) {
        return range;
    }

    range.swap_axes = cfg->get<bool>("/input/touch_range/swap_axes", false);
    range.min_x = cfg->get<int>("/input/touch_range/min_x", 0);
    range.max_x = cfg->get<int>("/input/touch_range/max_x", 0);
    range.min_y = cfg->get<int>("/input/touch_range/min_y", 0);
    range.max_y = cfg->get<int>("/input/touch_range/max_y", 0);

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
    if (!cfg) {
        spdlog::warn("[TouchCal] Config not available - touch range not saved");
        return;
    }

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
    spdlog::info("[TouchCal] Touch range saved: swap={} X({}..{}) Y({}..{})", range.swap_axes,
                 range.min_x, range.max_x, range.min_y, range.max_y);
}

TouchCalibration load_touch_calibration() {
    helix::Config* cfg = helix::Config::get_instance();
    TouchCalibration cal;

    if (!cfg) {
        spdlog::debug("[TouchCal] Config not available for calibration load");
        return cal;
    }

    cal.valid = cfg->get<bool>("/input/calibration/valid", false);
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

    s_active_ctx = &ctx;
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
    if (s_active_ctx == &ctx) {
        s_active_ctx = nullptr;
    }
}

} // namespace helix
