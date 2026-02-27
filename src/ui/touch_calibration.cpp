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

bool is_calibration_valid(const TouchCalibration& cal) {
    if (!cal.valid) {
        return false;
    }

    // Check all coefficients are finite (not NaN or Infinity)
    if (!std::isfinite(cal.a) || !std::isfinite(cal.b) || !std::isfinite(cal.c) ||
        !std::isfinite(cal.d) || !std::isfinite(cal.e) || !std::isfinite(cal.f)) {
        return false;
    }

    // Check coefficients are within reasonable bounds
    if (std::abs(cal.a) > MAX_CALIBRATION_COEFFICIENT ||
        std::abs(cal.b) > MAX_CALIBRATION_COEFFICIENT ||
        std::abs(cal.c) > MAX_CALIBRATION_COEFFICIENT ||
        std::abs(cal.d) > MAX_CALIBRATION_COEFFICIENT ||
        std::abs(cal.e) > MAX_CALIBRATION_COEFFICIENT ||
        std::abs(cal.f) > MAX_CALIBRATION_COEFFICIENT) {
        return false;
    }

    return true;
}

bool validate_calibration_result(const TouchCalibration& cal, const Point screen_points[3],
                                 const Point touch_points[3], int screen_width, int screen_height,
                                 float max_residual) {
    if (!cal.valid) {
        return false;
    }

    // Check 1: Coefficient sanity — scaling factors beyond 10x indicate bad input
    // (e.g., touch points clustered in a tiny area). The c/f offsets can be larger
    // (up to screen dimensions), so use the general MAX_CALIBRATION_COEFFICIENT for those.
    constexpr float MAX_SCALE_COEFFICIENT = 10.0f;
    if (std::abs(cal.a) > MAX_SCALE_COEFFICIENT || std::abs(cal.b) > MAX_SCALE_COEFFICIENT ||
        std::abs(cal.d) > MAX_SCALE_COEFFICIENT || std::abs(cal.e) > MAX_SCALE_COEFFICIENT) {
        spdlog::warn("[TouchCalibration] Calibration coefficients out of range "
                     "(a={:.2f}, b={:.2f}, d={:.2f}, e={:.2f})",
                     cal.a, cal.b, cal.d, cal.e);
        return false;
    }
    if (std::abs(cal.c) > MAX_CALIBRATION_COEFFICIENT ||
        std::abs(cal.f) > MAX_CALIBRATION_COEFFICIENT) {
        spdlog::warn("[TouchCalibration] Calibration offset out of range "
                     "(c={:.2f}, f={:.2f})",
                     cal.c, cal.f);
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

    return true;
}

bool calibration_suggests_axis_swap(const Point screen_points[3], const Point touch_points[3],
                                    const TouchCalibration& original_cal) {
    if (!original_cal.valid) {
        return false;
    }

    constexpr float EPSILON = 0.001f;
    float orig_cross = std::abs(original_cal.b) + std::abs(original_cal.d);
    float orig_diag = std::abs(original_cal.a) + std::abs(original_cal.e) + EPSILON;
    float orig_ratio = orig_cross / orig_diag;

    // Swap X/Y in touch points and recompute
    Point swapped_touch[3];
    for (int i = 0; i < 3; i++) {
        swapped_touch[i] = {touch_points[i].y, touch_points[i].x};
    }

    TouchCalibration swapped_cal;
    if (!compute_calibration(screen_points, swapped_touch, swapped_cal)) {
        return false;
    }

    float swap_cross = std::abs(swapped_cal.b) + std::abs(swapped_cal.d);
    float swap_diag = std::abs(swapped_cal.a) + std::abs(swapped_cal.e) + EPSILON;
    float swap_ratio = swap_cross / swap_diag;

    spdlog::debug("[TouchCalibration] Axis swap check: orig_ratio={:.4f}, swap_ratio={:.4f}",
                  orig_ratio, swap_ratio);

    return swap_ratio < orig_ratio * 0.5f;
}

} // namespace helix
