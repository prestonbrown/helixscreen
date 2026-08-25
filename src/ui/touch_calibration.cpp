// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#include "touch_calibration.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <mutex>

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

namespace {

/// A digitizer axis spanning fewer raw units than this cannot be a real range:
/// the decomposition landed on a near-degenerate slope, not on hardware.
constexpr int kMinRangeSpan = 8;

/// ...and one spanning more than this is not hardware either. Also keeps
/// lv_evdev's integer scale, which computes (v - min) * (out_max - out_min)
/// before dividing, clear of 32-bit overflow: 1e6 * 4096 still fits.
constexpr int kMaxRangeSpan = 1000000;

/// Below this the axis-aligned decomposition already reproduces the targets, so
/// the residual affine would be a sub-pixel no-op and is left switched off.
constexpr float kResidualIdentityPx = 0.5f;

/// Decline the decomposition once the residual exceeds this share of the shorter
/// display axis (never less than kResidualDeclineFloorPx, so a small panel still
/// gets room for tap noise).
///
/// The two stages compose exactly in the middle of the panel but not at its
/// edges: lv_evdev clamps its output before the residual affine runs, so a point
/// the shear would have pushed past an edge is clamped first and then sheared
/// back, costing up to `residual_px` of reach along that edge. That trade is
/// worth making for the sub-degree skew a laminated panel actually has, and it is
/// not worth making for a panel genuinely mounted at an angle - there the affine
/// is doing real work, it handles the shape without any clamp in front of it, and
/// leaving that case exactly as it was before is the safer answer.
constexpr float kResidualDeclineFraction = 0.10f;
constexpr float kResidualDeclineFloorPx = 12.0f;

/// Decompose one axis: `slope` pixels per raw unit and `offset` pixels at raw 0
/// describe `out_span + 1` display pixels. Recovers the raw values that land on
/// the first and last pixel. Returns false when the implied span is not a range
/// real hardware could emit.
bool decompose_axis(float slope, float offset, int out_span, int& out_min, int& out_max) {
    if (!std::isfinite(slope) || !std::isfinite(offset) || std::abs(slope) < 1e-9f) {
        return false;
    }
    const float span = static_cast<float>(out_span) / slope;
    const float min_v = -offset / slope;
    const float max_v = min_v + span;
    if (!std::isfinite(span) || !std::isfinite(min_v) || !std::isfinite(max_v)) {
        return false;
    }
    const float abs_span = std::abs(span);
    if (abs_span < static_cast<float>(kMinRangeSpan) ||
        abs_span > static_cast<float>(kMaxRangeSpan)) {
        return false;
    }
    // The raw endpoints themselves must stay inside int range with room for
    // lv_evdev's multiply. Both are bounded by the span check plus this.
    if (std::abs(min_v) > static_cast<float>(kMaxRangeSpan) ||
        std::abs(max_v) > static_cast<float>(kMaxRangeSpan)) {
        return false;
    }
    out_min = static_cast<int>(std::lround(min_v));
    out_max = static_cast<int>(std::lround(max_v));
    return out_min != out_max;
}

} // namespace

TouchRangeFit compute_range_fit(const Point screen[3], const Point raw[3], int screen_w,
                                int screen_h) {
    TouchRangeFit fit;

    if (screen_w < 2 || screen_h < 2) {
        return fit;
    }

    // Stage 1: the honest raw -> screen affine. Everything below is a
    // re-parameterisation of this same map, so the composed result reproduces it.
    TouchCalibration full;
    if (!compute_calibration(screen, raw, full)) {
        spdlog::debug("[TouchRangeFit] raw points are degenerate - no range fit");
        return fit;
    }

    // Stage 2: are the axes transposed? lv_evdev swaps BEFORE it scales, so its
    // "x range" describes whichever raw axis feeds screen X. Screen X is driven by
    // raw Y (and screen Y by raw X) exactly when the cross term dominates in both
    // rows; one row alone is shear, not a transposition.
    fit.swap_axes = std::abs(full.b) > std::abs(full.a) && std::abs(full.d) > std::abs(full.e);

    // The coefficient lv_evdev's linear map has to reproduce on each axis, and the
    // one it cannot carry (which becomes the residual).
    const float ax = fit.swap_axes ? full.b : full.a;
    const float ay = fit.swap_axes ? full.d : full.e;
    const float cross_x = fit.swap_axes ? full.a : full.b;
    const float cross_y = fit.swap_axes ? full.e : full.d;

    if (!decompose_axis(ax, full.c, screen_w - 1, fit.min_x, fit.max_x) ||
        !decompose_axis(ay, full.f, screen_h - 1, fit.min_y, fit.max_y)) {
        spdlog::debug("[TouchRangeFit] implied ABS range is not physically plausible "
                      "(slopes x={:.6f} y={:.6f}) - no range fit",
                      ax, ay);
        return fit;
    }

    // Stage 3: what the axis-aligned map leaves on the table at each target.
    // The full affine passes exactly through all three points, so this error is
    // precisely the cross terms the decomposition dropped.
    const float min_x_f = -full.c / ax;
    const float min_y_f = -full.f / ay;
    float worst = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float rx = static_cast<float>(raw[i].x);
        const float ry = static_cast<float>(raw[i].y);
        const float u = fit.swap_axes ? ry : rx;
        const float v = fit.swap_axes ? rx : ry;
        const float dx = ax * (u - min_x_f) - static_cast<float>(screen[i].x);
        const float dy = ay * (v - min_y_f) - static_cast<float>(screen[i].y);
        worst = std::max(worst, std::sqrt(dx * dx + dy * dy));
    }
    fit.residual_px = worst;

    // Stage 4: express that leftover as an affine over the evdev output, so the
    // two stages together still reproduce `full`. Substituting the inverse of the
    // axis-aligned map into `full` collapses to this, in both swap orientations:
    //   screen_x = ev_x + (cross_x / ay) * ev_y + cross_x * min_y
    //   screen_y = (cross_y / ax) * ev_x + ev_y + cross_y * min_x
    fit.residual.a = 1.0f;
    fit.residual.b = cross_x / ay;
    fit.residual.c = cross_x * min_y_f;
    fit.residual.d = cross_y / ax;
    fit.residual.e = 1.0f;
    fit.residual.f = cross_y * min_x_f;
    // Provisionally valid so is_calibration_valid() (which short-circuits on the
    // flag) actually inspects the coefficients.
    fit.residual.valid = true;
    if (!is_calibration_valid(fit.residual)) {
        // The leftover cannot be expressed, so the range on its own would map the
        // panel wrongly. Decline the whole decomposition rather than install half
        // of a mapping.
        spdlog::debug("[TouchRangeFit] residual affine is not finite - no range fit");
        return TouchRangeFit{};
    }
    // A sub-pixel residual is an identity dressed up in rounding noise. Leaving it
    // switched off keeps the affine stage out of the pipeline entirely on the
    // square panels that are the common case.
    fit.residual.valid = fit.residual_px > kResidualIdentityPx;

    const float decline_px =
        std::max(kResidualDeclineFloorPx,
                 kResidualDeclineFraction * static_cast<float>(std::min(screen_w, screen_h)));
    if (fit.residual_px > decline_px) {
        spdlog::info("[TouchRangeFit] declining range fit: axis-aligned decomposition leaves "
                     "{:.1f}px (limit {:.1f}px) - the panel is mounted at an angle, which the "
                     "affine stage handles without a clamp in front of it",
                     fit.residual_px, decline_px);
        return TouchRangeFit{};
    }

    fit.valid = true;

    spdlog::info("[TouchRangeFit] solved evdev range: swap={} X({}..{}) Y({}..{}) "
                 "residual={:.2f}px (affine stage {})",
                 fit.swap_axes, fit.min_x, fit.max_x, fit.min_y, fit.max_y, fit.residual_px,
                 fit.residual.valid ? "kept" : "not needed");

    return fit;
}

// ============================================================================
// Touch pipeline diagnostics (prestonbrown/helixscreen#1259, #1276)
// ============================================================================

namespace {

/// Order a configured [min,max] pair.
///
/// min > max is legal and inverts the axis: lv_evdev's scale simply gets a
/// negative denominator, which is exactly what a panel wired upside down
/// calibrates to. Comparing without ordering first would report every reading on
/// such a panel as out of range.
void order_range(int& lo, int& hi) {
    if (lo > hi) {
        std::swap(lo, hi);
    }
}

/// Whether one axis' observed extremes escape its configured range.
bool axis_escapes(int observed_min, int observed_max, int configured_min, int configured_max) {
    order_range(configured_min, configured_max);

    // A zero-span configured range is not a range: lv_evdev skips the scale but
    // still clamps, so the whole panel collapses onto one coordinate. That is
    // broken, but it is not something a reading can fall outside of, and saying
    // otherwise would flag every device that hits it.
    if (configured_min == configured_max) {
        return false;
    }
    return observed_min < configured_min || observed_max > configured_max;
}

} // namespace

const char* touch_range_source_name(TouchRangeSource source) {
    switch (source) {
    case TouchRangeSource::Declared:
        return "declared";
    case TouchRangeSource::Stored:
        return "stored";
    case TouchRangeSource::Environment:
        return "environment";
    case TouchRangeSource::None:
        break;
    }
    return "none";
}

void TouchObservedExtremes::observe(int x, int y) {
    if (distinct_samples == 0) {
        min_x = max_x = x;
        min_y = max_y = y;
    } else {
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    }
    distinct_samples++;
}

TouchObservedExtremes touch_observed_in_configured_axes(const TouchObservedExtremes& raw,
                                                        bool swap_axes) {
    if (!swap_axes) {
        return raw;
    }
    TouchObservedExtremes out = raw;
    out.min_x = raw.min_y;
    out.max_x = raw.max_y;
    out.min_y = raw.min_x;
    out.max_y = raw.max_x;
    return out;
}

TouchRangeViolation touch_range_violation(const TouchObservedExtremes& observed,
                                          const TouchPipelineInfo& configured) {
    TouchRangeViolation violation;
    if (!configured.configured_valid || observed.distinct_samples == 0) {
        return violation;
    }

    const TouchObservedExtremes seen =
        touch_observed_in_configured_axes(observed, configured.swap_axes);
    violation.x = axis_escapes(seen.min_x, seen.max_x, configured.min_x, configured.max_x);
    violation.y = axis_escapes(seen.min_y, seen.max_y, configured.min_y, configured.max_y);
    return violation;
}

double touch_axis_span_ratio(int observed_min, int observed_max, int configured_min,
                             int configured_max) {
    order_range(configured_min, configured_max);
    const int configured_span = configured_max - configured_min;
    if (configured_span <= 0) {
        return 0.0;
    }
    order_range(observed_min, observed_max);
    return static_cast<double>(observed_max - observed_min) / static_cast<double>(configured_span);
}

namespace {

// Last completed calibration's #943 span check. Lives here rather than with the
// calibration wrapper so the panel that produces it does not have to link the
// wrapper (which the ESP32 cut excludes), and so the pure-helper translation unit
// stays the one place the touch diagnostics vocabulary is defined.
//
// The mutex is for the debug bundle, which snapshots this from a worker thread
// while the calibration panel writes it on the LVGL main thread.
std::mutex s_span_check_mutex;
bool s_span_check_seen = false;
double s_span_check_x = 0.0;
double s_span_check_y = 0.0;

} // namespace

void record_touch_span_check(double ratio_x, double ratio_y) {
    std::lock_guard<std::mutex> lock(s_span_check_mutex);
    s_span_check_seen = true;
    s_span_check_x = ratio_x;
    s_span_check_y = ratio_y;
}

bool get_touch_span_check(double& ratio_x, double& ratio_y) {
    std::lock_guard<std::mutex> lock(s_span_check_mutex);
    if (!s_span_check_seen) {
        return false;
    }
    ratio_x = s_span_check_x;
    ratio_y = s_span_check_y;
    return true;
}

void clear_touch_span_check() {
    std::lock_guard<std::mutex> lock(s_span_check_mutex);
    s_span_check_seen = false;
    s_span_check_x = 0.0;
    s_span_check_y = 0.0;
}

} // namespace helix
