// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_tray_projection.h"

#include <algorithm>
#include <cmath>

namespace helix::ui::tray {

namespace {
constexpr float PI = 3.14159265358979f;
constexpr int HULL_SAMPLES = 181;
// The band is a 7 px stroke blurred with a 5 px Gaussian.
constexpr float SHEEN_BAND = 7.0f;
constexpr float SHEEN_SIGMA = 5.0f;
constexpr int SHEEN_REACH = 12; // rows either side of the centre
constexpr int SHEEN_MIN_OPA = 2;

float gauss_cdf(float x) {
    return 0.5f * (1.0f + std::erf(x / std::sqrt(2.0f)));
}
} // namespace

float box_depth(float spool_size) {
    return 2.0f * SPOOL_FLANGE_RADIUS * spool_size * (1.0f + BOX_DEPTH_MARGIN);
}

float box_rise(float depth) {
    return std::round(DEPTH_RISE * depth);
}

float spool_pitch(float spool_size) {
    // The spool canvas's own integer geometry: width between flanges plus a
    // flange ellipse each side.
    const int width = (int)(spool_size * SPOOL_WIDTH);
    const int flange_rx = (int)((float)(int)(spool_size * SPOOL_FLANGE_RADIUS) * DEPTH_SKEW);
    const float footprint = (float)(width + 2 * flange_rx);
    return std::round(footprint + DEPTH_SKEW * box_depth(spool_size) / 2.0f + PITCH_CLEARANCE);
}

PointF proj(const TrayBox& b, float x, float y, float z) {
    return {x + DEPTH_SKEW * z, y - b.rise * z / b.depth};
}

TrayFaces tray_faces(const TrayBox& b) {
    const PointF fl_t{b.fl, b.ft}, fr_t{b.fr, b.ft}, fl_b{b.fl, b.fb}, fr_b{b.fr, b.fb};
    const PointF bl_t = proj(b, b.fl, b.ft - b.back_extra, b.depth);
    const PointF br_t = proj(b, b.fr, b.ft - b.back_extra, b.depth);
    const PointF bl_b = proj(b, b.fl, b.fb, b.depth);
    const PointF br_b = proj(b, b.fr, b.fb, b.depth);
    return {{bl_t, br_t, br_b, bl_b},
            {fl_b, fr_b, br_b, bl_b},
            {fl_t, bl_t, bl_b, fl_b},
            {fl_t, fr_t, fr_b, fl_b},
            {fr_t, br_t, br_b, fr_b}};
}

float spool_front_cy(const TrayBox& b, float flange_ry) {
    return b.fb - flange_ry - SPOOL_FLOOR_GAP;
}

PointF spool_center(const TrayBox& b, float slot_x, float flange_ry) {
    return proj(b, slot_x, spool_front_cy(b, flange_ry), b.depth / 2.0f);
}

float lid_height(const TrayBox& b, float flange_ry) {
    return b.ft - b.back_extra / 2.0f - spool_front_cy(b, flange_ry) + flange_ry + LID_GAP;
}

PointF cap_point(const TrayBox& b, float lid_h, float x_end, float theta) {
    const float z = b.depth / 2.0f * (1.0f + std::cos(theta));
    const float h = b.back_extra * z / b.depth + lid_h * std::sin(theta);
    return proj(b, x_end, b.ft - h, z);
}

int cap_polyline(const TrayBox& b, float lid_h, float x_end, PointF* out, int n) {
    for (int i = 0; i < n; ++i) {
        const float theta = PI * (1.0f - (float)i / (float)(n - 1));
        out[i] = cap_point(b, lid_h, x_end, theta);
    }
    return n;
}

float unit_top_y(const TrayBox& b, float lid_h, bool has_lid) {
    if (!has_lid)
        return b.ft - b.back_extra - b.rise;
    // Both caps share the profile, so the hull's top is the profile's top.
    float top = b.ft;
    for (int i = 0; i < HULL_SAMPLES; ++i) {
        const float theta = PI * (float)i / (float)(HULL_SAMPLES - 1);
        top = std::min(top, cap_point(b, lid_h, b.fl, theta).y);
    }
    return top;
}

LidMode lid_mode(const AmsUnit& unit, bool has_physical_tray, bool dryer_supported) {
    if (!has_physical_tray)
        return LidMode::None;
    if (unit.environment.has_value() || dryer_supported)
        return LidMode::Unit;
    for (const auto& slot : unit.slots) {
        if (slot.environment.has_value())
            return LidMode::PerLane;
    }
    return LidMode::None;
}

float lane_lid_half_width(float slot_spacing, const TrayBox& b) {
    return slot_spacing / 2.0f - DEPTH_SKEW * b.depth / 4.0f - 1.0f;
}

int sheen_rows(const TrayBox& b, float lid_h, float alpha, SheenSpan& span, SheenRow* rows) {
    const float theta = SHEEN_THETA_DEG * PI / 180.0f;
    const PointF left = cap_point(b, lid_h, b.fl, theta);
    const PointF right = cap_point(b, lid_h, b.fr, theta);
    const float width = right.x - left.x;
    const float fade_in = std::min(70.0f, width * 0.3f);
    const float fade_out = std::min(110.0f, width * 0.4f);
    span.x0 = left.x + std::min(20.0f, width * 0.1f);
    span.x1 = right.x - std::min(30.0f, width * 0.1f);
    span.full0 = span.x0 + fade_in;
    span.full1 = span.x1 - fade_out;
    // The ramps are 0.3 and 0.4 of the lid's width inside a 0.8 span, so they
    // never cross: every band that fades in also reaches full strength.
    if (span.x1 <= span.x0)
        return 0;

    int n = 0;
    for (int k = -SHEEN_REACH; k <= SHEEN_REACH; ++k) {
        const float d = (float)k;
        const float cover = gauss_cdf((d + SHEEN_BAND / 2) / SHEEN_SIGMA) -
                            gauss_cdf((d - SHEEN_BAND / 2) / SHEEN_SIGMA);
        const long opa = std::lround(alpha * 255.0f * cover);
        if (opa < SHEEN_MIN_OPA)
            continue;
        rows[n++] = {left.y + d, (uint8_t)std::min<long>(opa, 255)};
    }
    return n;
}

} // namespace helix::ui::tray
