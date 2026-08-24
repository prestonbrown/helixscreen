// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#include "touch_calibration.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace helix {

bool compute_calibration(const Point screen_points[3], const Point touch_points[3],
                         TouchCalibration& out) {
    // Initialize output to invalid state
    out.valid = false;
    out.a = 1.0f;
    out.b = 0.0f;
    out.c = 0.0f;
    out.d = 0.0f;
    out.e = 1.0f;
    out.f = 0.0f;

    // Extract touch coordinates for readability
    float Xt1 = static_cast<float>(touch_points[0].x);
    float Yt1 = static_cast<float>(touch_points[0].y);
    float Xt2 = static_cast<float>(touch_points[1].x);
    float Yt2 = static_cast<float>(touch_points[1].y);
    float Xt3 = static_cast<float>(touch_points[2].x);
    float Yt3 = static_cast<float>(touch_points[2].y);

    // Extract screen coordinates for readability
    float Xs1 = static_cast<float>(screen_points[0].x);
    float Ys1 = static_cast<float>(screen_points[0].y);
    float Xs2 = static_cast<float>(screen_points[1].x);
    float Ys2 = static_cast<float>(screen_points[1].y);
    float Xs3 = static_cast<float>(screen_points[2].x);
    float Ys3 = static_cast<float>(screen_points[2].y);

    // Compute divisor (determinant) using Maxim Integrated AN5296 algorithm
    // Reference: https://pdfserv.maximintegrated.com/en/an/AN5296.pdf
    // div = (Xt1-Xt3)*(Yt2-Yt3) - (Xt2-Xt3)*(Yt1-Yt3)
    float div = (Xt1 - Xt3) * (Yt2 - Yt3) - (Xt2 - Xt3) * (Yt1 - Yt3);

    // Check for degenerate case (collinear or duplicate points)
    // Use scale-relative epsilon based on coordinate magnitudes.
    // For typical touchscreens (12-bit ADC, 0-4095 range), valid triangles
    // produce determinants >> 1000, so 0.01% of max coordinate is safe.
    float max_coord =
        std::max(std::initializer_list<float>({std::abs(Xt1), std::abs(Yt1), std::abs(Xt2),
                                               std::abs(Yt2), std::abs(Xt3), std::abs(Yt3)}));
    float epsilon = std::max(1.0f, max_coord * 0.0001f);
    if (std::abs(div) < epsilon) {
        if (is_touch_debug_enabled()) {
            spdlog::warn("[TouchDebug] compute_calibration FAILED — degenerate points");
            spdlog::warn("[TouchDebug]   touch[0]=({},{}) touch[1]=({},{}) touch[2]=({},{})",
                         touch_points[0].x, touch_points[0].y, touch_points[1].x, touch_points[1].y,
                         touch_points[2].x, touch_points[2].y);
            spdlog::warn("[TouchDebug]   determinant={:.6f} epsilon={:.6f} (too small)", div,
                         epsilon);
        }
        return false;
    }

    // Compute affine transformation coefficients
    // screen_x = a*touch_x + b*touch_y + c
    out.a = ((Xs1 - Xs3) * (Yt2 - Yt3) - (Xs2 - Xs3) * (Yt1 - Yt3)) / div;
    out.b = ((Xt1 - Xt3) * (Xs2 - Xs3) - (Xt2 - Xt3) * (Xs1 - Xs3)) / div;
    out.c = Xs1 - out.a * Xt1 - out.b * Yt1;

    // screen_y = d*touch_x + e*touch_y + f
    out.d = ((Ys1 - Ys3) * (Yt2 - Yt3) - (Ys2 - Ys3) * (Yt1 - Yt3)) / div;
    out.e = ((Xt1 - Xt3) * (Ys2 - Ys3) - (Xt2 - Xt3) * (Ys1 - Ys3)) / div;
    out.f = Ys1 - out.d * Xt1 - out.e * Yt1;

    out.valid = true;

    if (is_touch_debug_enabled()) {
        spdlog::warn("[TouchDebug] compute_calibration inputs:");
        spdlog::warn("[TouchDebug]   screen[0]=({},{}) screen[1]=({},{}) screen[2]=({},{})",
                     screen_points[0].x, screen_points[0].y, screen_points[1].x, screen_points[1].y,
                     screen_points[2].x, screen_points[2].y);
        spdlog::warn("[TouchDebug]   touch[0]=({},{}) touch[1]=({},{}) touch[2]=({},{})",
                     touch_points[0].x, touch_points[0].y, touch_points[1].x, touch_points[1].y,
                     touch_points[2].x, touch_points[2].y);
        spdlog::warn("[TouchDebug]   determinant={:.6f} epsilon={:.6f}", div, epsilon);
        spdlog::warn(
            "[TouchDebug]   coefficients: a={:.6f} b={:.6f} c={:.6f} d={:.6f} e={:.6f} f={:.6f}",
            out.a, out.b, out.c, out.d, out.e, out.f);
        spdlog::warn(
            "[TouchDebug]   transform: screen_x = {:.6f}*touch_x + {:.6f}*touch_y + {:.6f}", out.a,
            out.b, out.c);
        spdlog::warn(
            "[TouchDebug]   transform: screen_y = {:.6f}*touch_x + {:.6f}*touch_y + {:.6f}", out.d,
            out.e, out.f);
    }

    return true;
}

Point transform_point(const TouchCalibration& cal, Point raw, int max_x, int max_y) {
    // If calibration is invalid, return input unchanged
    if (!cal.valid) {
        return raw;
    }

    // Apply affine transformation with rounding
    float raw_x = static_cast<float>(raw.x);
    float raw_y = static_cast<float>(raw.y);

    Point result;
    result.x = static_cast<int>(std::round(cal.a * raw_x + cal.b * raw_y + cal.c));
    result.y = static_cast<int>(std::round(cal.d * raw_x + cal.e * raw_y + cal.f));

    // Clamp to screen bounds if specified (prevents out-of-bounds coordinates
    // from corrupted calibration data)
    if (max_x > 0) {
        result.x = std::max(0, std::min(result.x, max_x));
    }
    if (max_y > 0) {
        result.y = std::max(0, std::min(result.y, max_y));
    }

    return result;
}

bool invert_transform_point(const TouchCalibration& cal, Point screen, Point& out_raw) {
    if (!cal.valid)
        return false;
    float det = cal.a * cal.e - cal.b * cal.d;
    if (std::abs(det) < 1e-6f)
        return false;
    float sx = static_cast<float>(screen.x) - cal.c;
    float sy = static_cast<float>(screen.y) - cal.f;
    float rx = (cal.e * sx - cal.b * sy) / det;
    float ry = (-cal.d * sx + cal.a * sy) / det;
    out_raw.x = static_cast<int>(std::round(rx));
    out_raw.y = static_cast<int>(std::round(ry));
    return true;
}

TouchCalibration platform_default_calibration() {
    TouchCalibration cal;

    // Measured on our own hardware, one unit per platform. The AD5X and AD5M Pro
    // panels are the same part: strip the Y-axis polarity difference and the two
    // fits agree to 0.19% in X scale and 1.61% in Y, with raw spans of 682.8 vs
    // 681.4 (X) and 324.6 vs 329.8 (Y). That cross-agreement is why these are
    // shipped at all — a lone fit would only describe the unit it came from.
    //
    // e is positive on the AD5X and negative on the AD5M: the digitizer reports Y
    // in opposite directions on the two models, and f absorbs the flip.
#if defined(HELIX_PLATFORM_AD5X)
    cal.valid = true;
    cal.a = 1.171731f;
    cal.b = -0.043628f;
    cal.c = -66.965828f;
    cal.d = -0.006188f;
    cal.e = 1.478954f;
    cal.f = -118.906227f;
#elif defined(HELIX_PLATFORM_AD5M)
    cal.valid = true;
    cal.a = 1.174004f;
    cal.b = 0.009220f;
    cal.c = -79.064400f;
    cal.d = -0.000000f;
    cal.e = -1.455497f;
    cal.f = 570.492126f;
#endif

    // Never hand back a default that our own validator would reject.
    if (cal.valid && !is_calibration_valid(cal)) {
        spdlog::warn("[TouchCal] Platform default failed validation — ignoring");
        cal.valid = false;
    }
    return cal;
}

bool is_calibration_valid(const TouchCalibration& cal) {
    if (!cal.valid) {
        return false;
    }

    // Check all coefficients are finite (not NaN or Infinity). No magnitude
    // bound: legitimate hardware (e.g. Mellow FLY-TFT35 with Goodix capacitive
    // touch) needs scale/offset terms in the thousands to map a compressed
    // ABS sub-range across the panel. Geometric validity is enforced by
    // validate_calibration_result() at compute time.
    if (!std::isfinite(cal.a) || !std::isfinite(cal.b) || !std::isfinite(cal.c) ||
        !std::isfinite(cal.d) || !std::isfinite(cal.e) || !std::isfinite(cal.f)) {
        return false;
    }

    return true;
}

bool detect_and_correct_axis_swap(TouchCalibration& cal, const Point screen_points[3],
                                  Point touch_points[3]) {
    // Compute cross-coupling ratio: off-diagonal vs diagonal dominance
    // For a well-aligned screen: a,e are large (scaling), b,d are ~0 (no cross-coupling)
    // For swapped axes: b,d are large, a,e may be small or the matrix is chaotic
    float diagonal = std::abs(cal.a) + std::abs(cal.e);
    float off_diagonal = std::abs(cal.b) + std::abs(cal.d);

    // Avoid division by zero
    if (diagonal < 0.001f) {
        diagonal = 0.001f;
    }

    float cross_coupling_ratio = off_diagonal / diagonal;

    if (is_touch_debug_enabled()) {
        spdlog::warn("[TouchDebug] axis_swap check: diagonal={:.4f} off_diagonal={:.4f} "
                     "ratio={:.4f} (threshold=0.3)",
                     diagonal, off_diagonal, cross_coupling_ratio);
    }

    // Only consider swap if cross-coupling is significant
    if (cross_coupling_ratio <= 0.3f) {
        return false;
    }

    spdlog::info("[TouchCalibration] High cross-coupling detected (ratio={:.2f}, "
                 "a={:.3f} b={:.3f} d={:.3f} e={:.3f}), testing axis swap",
                 cross_coupling_ratio, cal.a, cal.b, cal.d, cal.e);

    // Try swapping X/Y in touch points and recomputing
    Point swapped_points[3];
    for (int i = 0; i < 3; i++) {
        swapped_points[i] = {touch_points[i].y, touch_points[i].x};
    }

    TouchCalibration swapped_cal;
    if (!compute_calibration(screen_points, swapped_points, swapped_cal)) {
        spdlog::debug("[TouchCalibration] Axis-swapped calibration failed (degenerate)");
        return false;
    }

    // Check if swapped version has better (lower) cross-coupling
    float swapped_diagonal = std::abs(swapped_cal.a) + std::abs(swapped_cal.e);
    float swapped_off_diagonal = std::abs(swapped_cal.b) + std::abs(swapped_cal.d);
    if (swapped_diagonal < 0.001f) {
        swapped_diagonal = 0.001f;
    }
    float swapped_ratio = swapped_off_diagonal / swapped_diagonal;

    if (is_touch_debug_enabled()) {
        spdlog::warn("[TouchDebug] axis_swap test: swapped_diagonal={:.4f} "
                     "swapped_off_diagonal={:.4f} swapped_ratio={:.4f}",
                     swapped_diagonal, swapped_off_diagonal, swapped_ratio);
    }

    if (swapped_ratio >= cross_coupling_ratio) {
        spdlog::debug("[TouchCalibration] Swap did not improve cross-coupling "
                      "(original={:.2f}, swapped={:.2f}), keeping original",
                      cross_coupling_ratio, swapped_ratio);
        return false;
    }

    spdlog::info("[TouchCalibration] Axis swap corrected cross-coupling "
                 "(ratio {:.2f} -> {:.2f}, a={:.3f} b={:.3f} d={:.3f} e={:.3f})",
                 cross_coupling_ratio, swapped_ratio, swapped_cal.a, swapped_cal.b, swapped_cal.d,
                 swapped_cal.e);

    // Apply the swap: update touch points in-place and use the swapped calibration
    for (int i = 0; i < 3; i++) {
        touch_points[i] = swapped_points[i];
    }
    swapped_cal.axes_swapped = true;
    cal = swapped_cal;
    return true;
}

bool validate_calibration_result(const TouchCalibration& cal, const Point screen_points[3],
                                 const Point touch_points[3], int screen_width, int screen_height,
                                 float max_residual) {
    if (!cal.valid) {
        return false;
    }

    if (is_touch_debug_enabled()) {
        spdlog::warn("[TouchDebug] validate_calibration_result:");
        spdlog::warn(
            "[TouchDebug]   coefficients: a={:.6f} b={:.6f} c={:.6f} d={:.6f} e={:.6f} f={:.6f}",
            cal.a, cal.b, cal.c, cal.d, cal.e, cal.f);
        spdlog::warn("[TouchDebug]   screen {}x{}, max_residual={:.1f}px", screen_width,
                     screen_height, max_residual);
    }

    // Coefficient magnitudes are intentionally not bounded. Both scales
    // (a, b, d, e) and offsets (c, f) can legitimately grow large when a
    // touch controller's config restricts output to a narrow sub-range of
    // its declared ABS axes — e.g. Mellow FLY-TFT35 with Goodix capacitive
    // touch reports X≈0..50 and Y≈280..320 across the full 480x320 panel,
    // requiring a≈8.4, e≈13.2, f≈-4000. NaN/Inf is filtered upstream by
    // is_calibration_valid(); ill-conditioned matrices are caught by the
    // input-side touch-span check below plus the residual + center checks.

    // Check 1: Touch points must span enough range on each axis that the
    // resulting affine is meaningful. Three calibration taps jittered within
    // a few sensor units don't define a real transform — the matrix is
    // mathematically exact but wildly noise-sensitive (a single 1-unit touch
    // jitter at runtime produces hundreds of pixels of pointer motion).
    constexpr int MIN_TOUCH_AXIS_SPAN = 5;
    int tx_min = std::min({touch_points[0].x, touch_points[1].x, touch_points[2].x});
    int tx_max = std::max({touch_points[0].x, touch_points[1].x, touch_points[2].x});
    int ty_min = std::min({touch_points[0].y, touch_points[1].y, touch_points[2].y});
    int ty_max = std::max({touch_points[0].y, touch_points[1].y, touch_points[2].y});
    if ((tx_max - tx_min) < MIN_TOUCH_AXIS_SPAN || (ty_max - ty_min) < MIN_TOUCH_AXIS_SPAN) {
        spdlog::warn("[TouchCalibration] Touch points too clustered "
                     "(x_span={}, y_span={}, min={})",
                     tx_max - tx_min, ty_max - ty_min, MIN_TOUCH_AXIS_SPAN);
        return false;
    }

    // Check 2: Back-transform residuals (numerical stability guard)
    // A 3-point affine is solved exactly, so residuals at calibration points are
    // mathematically ~0. This check catches NaN/Inf propagation or floating-point
    // corruption rather than geometric errors.
    for (int i = 0; i < 3; i++) {
        Point transformed = transform_point(cal, touch_points[i]);
        float dx = static_cast<float>(transformed.x - screen_points[i].x);
        float dy = static_cast<float>(transformed.y - screen_points[i].y);
        float residual = std::sqrt(dx * dx + dy * dy);

        if (is_touch_debug_enabled()) {
            spdlog::warn("[TouchDebug]   back-transform[{}]: touch({},{}) -> ({},{}) "
                         "expected({},{}) residual={:.2f}px {}",
                         i, touch_points[i].x, touch_points[i].y, transformed.x, transformed.y,
                         screen_points[i].x, screen_points[i].y, residual,
                         residual > max_residual ? "FAIL" : "OK");
        }

        if (residual > max_residual) {
            spdlog::warn("[TouchCalibration] Back-transform residual {:.1f}px at point {} "
                         "(expected ({},{}), got ({},{}))",
                         residual, i, screen_points[i].x, screen_points[i].y, transformed.x,
                         transformed.y);
            return false;
        }
    }

    // Check 3: Center of touch range should map to somewhere near the screen
    int center_x = (touch_points[0].x + touch_points[1].x + touch_points[2].x) / 3;
    int center_y = (touch_points[0].y + touch_points[1].y + touch_points[2].y) / 3;
    Point center_transformed = transform_point(cal, {center_x, center_y});

    if (is_touch_debug_enabled()) {
        spdlog::warn(
            "[TouchDebug]   center: touch_avg({},{}) -> screen({},{}) bounds=[{},{}]-[{},{}]",
            center_x, center_y, center_transformed.x, center_transformed.y, -screen_width / 2,
            -screen_height / 2, screen_width + screen_width / 2, screen_height + screen_height / 2);
    }

    int margin_x = screen_width / 2;
    int margin_y = screen_height / 2;
    if (center_transformed.x < -margin_x || center_transformed.x > screen_width + margin_x ||
        center_transformed.y < -margin_y || center_transformed.y > screen_height + margin_y) {
        spdlog::warn("[TouchCalibration] Center of touch range ({},{}) maps to ({},{}), "
                     "which is far off-screen ({}x{})",
                     center_x, center_y, center_transformed.x, center_transformed.y, screen_width,
                     screen_height);
        return false;
    }

    if (is_touch_debug_enabled()) {
        spdlog::warn("[TouchDebug] validation PASSED");
    }

    return true;
}

} // namespace helix
