// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_spool_vents.cpp
 * @brief The vented spool flange (include/spool_vents.h): which detail a size
 *        shows, and where the slots and vent dots open on a face of any skew.
 */

#include "spool_vents.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using helix::ui::spool_vent_coverage;
using helix::ui::spool_vent_detail;
using helix::ui::spool_vent_face_radius;
using helix::ui::SpoolVentDetail;
using helix::ui::SpoolVentFace;

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kSize = 200.0f;

SpoolVentFace face(SpoolVentDetail detail, float skew = 1.0f) {
    return SpoolVentFace{100.0f, 100.0f, kSize, skew, detail};
}

// The canvas point at @p r (fraction of size) and @p deg (0 = right, -90 = up)
// on @p f, squashed sideways by its skew.
float px(const SpoolVentFace& f, float r, float deg) {
    return f.cx + r * f.size * std::cos(deg * kPi / 180.0f) * f.skew;
}
float py(const SpoolVentFace& f, float r, float deg) {
    return f.cy + r * f.size * std::sin(deg * kPi / 180.0f);
}

} // namespace

TEST_CASE("spool_vents: detail by size, both edges inclusive", "[spool_vents]") {
    CHECK(spool_vent_detail(0) == SpoolVentDetail::None);
    CHECK(spool_vent_detail(31) == SpoolVentDetail::None);
    CHECK(spool_vent_detail(32) == SpoolVentDetail::Slots);
    CHECK(spool_vent_detail(95) == SpoolVentDetail::Slots);
    CHECK(spool_vent_detail(96) == SpoolVentDetail::SlotsAndDots);
    CHECK(spool_vent_detail(240) == SpoolVentDetail::SlotsAndDots);
    const SpoolVentFace none = face(SpoolVentDetail::None); // no detail opens nothing
    CHECK(spool_vent_coverage(none, px(none, 0.25f, -90.0f), py(none, 0.25f, -90.0f)) == 0.0f);
}

TEST_CASE("spool_vents: six slots, the first straight up", "[spool_vents]") {
    const SpoolVentFace f = face(SpoolVentDetail::Slots);
    for (float deg : {-90.0f, -30.0f, 30.0f, 90.0f, 150.0f, 210.0f})
        CHECK(spool_vent_coverage(f, px(f, 0.25f, deg), py(f, 0.25f, deg)) == 1.0f);
    CHECK(spool_vent_coverage(f, f.cx, f.cy) == 0.0f);                             // hub
    CHECK(spool_vent_coverage(f, px(f, 0.25f, 0.0f), py(f, 0.25f, 0.0f)) == 0.0f); // between
    // On a small squashed face, solid flange still stands 2 px inside the rim past every slot.
    const SpoolVentFace tray{46.0f, 46.0f, 92.0f, 0.45f, SpoolVentDetail::Slots};
    for (float deg : {-90.0f, -30.0f, 30.0f}) {
        const float rx = px(tray, 0.42f, deg) - tray.cx, ry = py(tray, 0.42f, deg) - tray.cy;
        const float in = 1.0f - 2.0f / std::hypot(rx, ry);
        CHECK(spool_vent_coverage(tray, tray.cx + rx * in, tray.cy + ry * in) == 0.0f);
    }
    CHECK(spool_vent_coverage(f, px(f, 0.10f, -90.0f), py(f, 0.10f, -90.0f)) ==
          0.0f); // inside the start
}

TEST_CASE("spool_vents: slot edges are anti-aliased", "[spool_vents]") {
    const SpoolVentFace f = face(SpoolVentDetail::Slots);
    // Walk across the top slot's long edge: x from its centre line outward.
    bool partial = false;
    for (float dx = 0.0f; dx < 0.1f * kSize; dx += 1.0f) {
        const float c = spool_vent_coverage(f, f.cx + dx + 0.5f, py(f, 0.25f, -90.0f));
        partial = partial || (c > 0.0f && c < 1.0f);
    }
    CHECK(partial);
}

TEST_CASE("spool_vents: vent dots open between the slots on large spools only", "[spool_vents]") {
    // (0.27 S, 0) sits on the dot lattice, in the dot band, clear of both
    // neighbouring slots.
    const SpoolVentFace dots = face(SpoolVentDetail::SlotsAndDots);
    CHECK(spool_vent_coverage(dots, px(dots, 0.27f, 0.0f), py(dots, 0.27f, 0.0f)) > 0.9f);

    const SpoolVentFace slots = face(SpoolVentDetail::Slots);
    CHECK(spool_vent_coverage(slots, px(slots, 0.27f, 0.0f), py(slots, 0.27f, 0.0f)) == 0.0f);

    // Lattice points past the band's outer edge or inside its inner edge stay shut.
    CHECK(spool_vent_coverage(dots, px(dots, 0.39f, 0.0f), py(dots, 0.39f, 0.0f)) == 0.0f);
    CHECK(spool_vent_coverage(dots, px(dots, 0.12f, 0.0f), py(dots, 0.12f, 0.0f)) == 0.0f);
}

TEST_CASE("spool_vents: a skewed face squashes the slots sideways", "[spool_vents]") {
    const SpoolVentFace f = face(SpoolVentDetail::Slots, 0.45f);
    CHECK(spool_vent_coverage(f, px(f, 0.25f, -30.0f), py(f, 0.25f, -30.0f)) == 1.0f);
    // Where that slot would sit on an unskewed face is solid flange here.
    const float flat_x = f.cx + 0.25f * kSize * std::cos(-30.0f * kPi / 180.0f);
    CHECK(spool_vent_coverage(f, flat_x, py(f, 0.25f, -30.0f)) == 0.0f);
    CHECK(spool_vent_face_radius(f, px(f, 0.25f, -30.0f), py(f, 0.25f, -30.0f)) ==
          Catch::Approx(0.25f * kSize).margin(0.5f));
}
