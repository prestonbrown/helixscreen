// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spool_vents.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace helix::ui {

namespace {

// Geometry as fractions of the canvas size, measured on the unskewed face.
constexpr float kSlotInner = 0.165f;        // slot centre line starts clear of the hub
constexpr float kSlotOuter = 0.335f;        // and ends inside the flange rim
constexpr float kSlotHalfWidth = 0.05f;     // wide enough to read at 32 px
constexpr float kDotPitch = 0.03f;          // square lattice centred on the face
constexpr float kDotRadius = 0.007f;        // about 1.5 px across at 96 px
constexpr float kDotBandInner = 0.15f;      // dots stay off the hub boss
constexpr float kDotBandOuter = 0.39f;      // and off the flange rim
constexpr float kDotSlotClearance = 0.025f; // a solid web between a dot and a slot

constexpr int kSlotCount = 6;
constexpr float kPi = 3.14159265358979f;

struct Dir {
    float u;
    float v;
};

// Unit directions of the slots, the first straight up (v grows downward).
const std::array<Dir, kSlotCount>& slot_dirs() {
    static const std::array<Dir, kSlotCount> dirs = [] {
        std::array<Dir, kSlotCount> d{};
        for (int k = 0; k < kSlotCount; ++k) {
            const float a = -kPi / 2.0f + 2.0f * kPi * static_cast<float>(k) / kSlotCount;
            d[k] = {std::cos(a), std::sin(a)};
        }
        return d;
    }();
    return dirs;
}

// Signed distance from (u, v) to the edge of a disc or stadium whose centre
// line ends at (qu, qv), returned in screen pixels: the face distance scaled
// by how much the skew shortens the edge normal on screen.
float screen_signed_distance(float u, float v, float qu, float qv, float radius, float skew) {
    const float du = u - qu;
    const float dv = v - qv;
    const float dist = std::sqrt(du * du + dv * dv);
    if (dist <= 0.0f)
        return -radius;
    const float nu = du / dist * skew;
    const float nv = dv / dist;
    return (dist - radius) * std::sqrt(nu * nu + nv * nv);
}

// The slot nearest (u, v): every slot starts at the same radius and they are
// evenly spaced, so it is the one whose direction (u, v) leans toward most.
const Dir& nearest_slot(float u, float v) {
    const auto& dirs = slot_dirs();
    const Dir* best = &dirs[0];
    float lean = u * best->u + v * best->v;
    for (const Dir& dir : dirs) {
        const float l = u * dir.u + v * dir.v;
        if (l > lean) {
            lean = l;
            best = &dir;
        }
    }
    return *best;
}

// Nearest point on a slot's centre line to (u, v), in face units.
Dir nearest_on_slot(const Dir& dir, float u, float v, float size) {
    const float t = std::clamp(u * dir.u + v * dir.v, kSlotInner * size, kSlotOuter * size);
    return {t * dir.u, t * dir.v};
}

float coverage_from(float signed_distance) {
    return std::clamp(0.5f - signed_distance, 0.0f, 1.0f);
}

float slot_coverage(float u, float v, float size, float skew) {
    const Dir q = nearest_on_slot(nearest_slot(u, v), u, v, size);
    return coverage_from(screen_signed_distance(u, v, q.u, q.v, kSlotHalfWidth * size, skew));
}

bool dot_site_open(float gu, float gv, float size) {
    const float r = std::sqrt(gu * gu + gv * gv);
    if (r <= kDotBandInner * size || r >= kDotBandOuter * size)
        return false;
    const float keep_out = (kSlotHalfWidth + kDotRadius + kDotSlotClearance) * size;
    const Dir q = nearest_on_slot(nearest_slot(gu, gv), gu, gv, size);
    const float du = gu - q.u;
    const float dv = gv - q.v;
    return du * du + dv * dv >= keep_out * keep_out;
}

float dot_coverage(float u, float v, float size, float skew) {
    const float pitch = kDotPitch * size;
    const float gu = std::round(u / pitch) * pitch;
    const float gv = std::round(v / pitch) * pitch;
    if (!dot_site_open(gu, gv, size))
        return 0.0f;
    return coverage_from(screen_signed_distance(u, v, gu, gv, kDotRadius * size, skew));
}

} // namespace

SpoolVentDetail spool_vent_detail(int32_t size_px) {
    if (size_px >= kSpoolVentDotsMinPx)
        return SpoolVentDetail::SlotsAndDots;
    if (size_px >= kSpoolVentSlotsMinPx)
        return SpoolVentDetail::Slots;
    return SpoolVentDetail::None;
}

float spool_vent_coverage(const SpoolVentFace& face, float x, float y) {
    if (face.detail == SpoolVentDetail::None || face.size <= 0.0f || face.skew <= 0.0f)
        return 0.0f;
    const float u = (x - face.cx) / face.skew;
    const float v = y - face.cy;
    // Nothing opens outside the ring the slots and dots occupy, widened by the
    // anti-aliased edge (one screen pixel, up to 1 / skew face units).
    const float edge = 1.0f / face.skew;
    const float inner = std::max(0.0f, (kSlotInner - kSlotHalfWidth) * face.size - edge);
    const float outer =
        std::max(kSlotOuter + kSlotHalfWidth, kDotBandOuter + kDotRadius) * face.size + edge;
    const float r2 = u * u + v * v;
    if (r2 < inner * inner || r2 > outer * outer)
        return 0.0f;
    float cover = slot_coverage(u, v, face.size, face.skew);
    if (face.detail == SpoolVentDetail::SlotsAndDots && cover < 1.0f)
        cover = std::max(cover, dot_coverage(u, v, face.size, face.skew));
    return cover;
}

float spool_vent_face_radius(const SpoolVentFace& face, float x, float y) {
    const float u = face.skew > 0.0f ? (x - face.cx) / face.skew : 0.0f;
    const float v = y - face.cy;
    return std::sqrt(u * u + v * v);
}

} // namespace helix::ui
