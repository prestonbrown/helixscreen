// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AMS detail unit's projection, box, lid and sheen on the 800x480 numbers:
// spool 61, front face FL 10, FR 392, FT 101, FB 126, back wall 10 px taller.

#include "ui_ams_slot_layout.h"

#include "ams_tray_projection.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::tray;
using Catch::Approx;

namespace {

constexpr float PI = 3.14159265358979f;
constexpr float FLANGE_RY = 0.42f * 61;

TrayBox reference_box() {
    const float dz = box_depth(61);
    return {10, 392, 101, 126, dz, box_rise(dz), 10};
}

void check_point(PointF p, float x, float y, float eps = 0.001f) {
    CHECK(p.x == Approx(x).margin(eps));
    CHECK(p.y == Approx(y).margin(eps));
}

} // namespace

TEST_CASE("tray projection: depth, rise and the skew", "[ams][tray]") {
    const TrayBox b = reference_box();
    CHECK(b.depth == Approx(56.364).margin(0.001));
    CHECK(b.rise == 5);
    CHECK(DEPTH_SKEW * b.depth == Approx(25.364).margin(0.001));
    check_point(proj(b, 0, 0, 0), 0, 0);
    check_point(proj(b, 0, 0, b.depth), 25.364f, -5);
}

TEST_CASE("tray projection: the box faces", "[ams][tray]") {
    const TrayFaces f = tray_faces(reference_box());
    const PointF bl_t = f.back_wall[0], br_t = f.back_wall[1], br_b = f.back_wall[2],
                 bl_b = f.back_wall[3];
    check_point(bl_t, 35.364f, 86);
    check_point(br_t, 417.364f, 86);
    check_point(bl_b, 35.364f, 121);
    check_point(br_b, 417.364f, 121);
    check_point(f.right_side[0], 392, 101);
    check_point(f.right_side[1], 417.364f, 86);
    check_point(f.right_side[2], 417.364f, 121);
    check_point(f.right_side[3], 392, 126);
    // Left edge 25 tall, right edge 35; the floor's back edge 5 px above its front.
    CHECK(f.right_side[3].y - f.right_side[0].y == Approx(25));
    CHECK(f.right_side[2].y - f.right_side[1].y == Approx(35));
    CHECK(f.floor[0].y - f.floor[3].y == Approx(5));
}

TEST_CASE("tray projection: spools stand on the floor", "[ams][tray]") {
    const TrayBox b = reference_box();
    CHECK(spool_front_cy(b, FLANGE_RY) == Approx(98.38).margin(0.001));
    const PointF c = spool_center(b, 58, FLANGE_RY);
    check_point(c, 70.682f, 95.88f);
    // The spool's bottom lies below the front-wall top: the wall covers it.
    CHECK(c.y + FLANGE_RY > b.ft);
}

TEST_CASE("tray projection: the lid clears the spools by one pixel", "[ams][tray]") {
    const TrayBox b = reference_box();
    const float lid_h = lid_height(b, FLANGE_RY);
    CHECK(lid_h == Approx(24.24).margin(0.001));
    check_point(cap_point(b, lid_h, 392, PI), 392, 101);
    check_point(cap_point(b, lid_h, 392, 0), 417.364f, 86);
    const PointF crest = cap_point(b, lid_h, 392, PI / 2);
    check_point(crest, 404.682f, 69.26f);
    const PointF spool = spool_center(b, 58, FLANGE_RY);
    CHECK(spool.y - FLANGE_RY - crest.y == Approx(1).margin(0.001));

    // The hull's top is above the mid-depth crest: the chord slopes up to the back.
    CHECK(unit_top_y(b, lid_h, true) == Approx(68.13).margin(0.01));
    CHECK(unit_top_y(b, 0, false) == Approx(86));
}

TEST_CASE("tray projection: the cap profile runs front to back above its chord", "[ams][tray]") {
    const TrayBox b = reference_box();
    const float lid_h = lid_height(b, FLANGE_RY);
    PointF pts[17];
    REQUIRE(cap_polyline(b, lid_h, 392, pts, 17) == 17);
    check_point(pts[0], 392, 101);
    check_point(pts[16], 417.364f, 86);
    for (int i = 0; i < 17; ++i) {
        CAPTURE(i);
        CHECK(pts[i].x >= 392 - 0.001f);
        CHECK(pts[i].x <= 417.364f + 0.001f);
        if (i > 0)
            CHECK(pts[i].x >= pts[i - 1].x - 0.001f);
        // On or above (smaller y) the chord from fr_t to br_t.
        const float t = (pts[i].x - 392) / 25.364f;
        CHECK(pts[i].y <= 101 + t * (86 - 101) + 0.001f);
    }
}

TEST_CASE("tray projection: one diffuse sheen band", "[ams][tray]") {
    const TrayBox b = reference_box();
    const float lid_h = lid_height(b, FLANGE_RY);
    check_point(cap_point(b, lid_h, 10, 112 * PI / 180), 17.931f, 73.835f);
    check_point(cap_point(b, lid_h, 392, 112 * PI / 180), 399.931f, 73.835f);

    SheenSpan span{};
    SheenRow rows[SHEEN_MAX_ROWS];
    const int n = sheen_rows(b, lid_h, 0.26f, span, rows);
    CHECK(span.x0 == Approx(37.931).margin(0.001));
    CHECK(span.full0 == Approx(107.931).margin(0.001));
    CHECK(span.full1 == Approx(259.931).margin(0.001));
    CHECK(span.x1 == Approx(369.931).margin(0.001));
    REQUIRE(n > 0);
    REQUIRE(n <= SHEEN_MAX_ROWS);
    auto opa_at = [&](int k) {
        for (int i = 0; i < n; ++i)
            if (std::fabs(rows[i].y - (73.835f + k)) < 0.01f)
                return (int)rows[i].opa;
        return -1;
    };
    CHECK(opa_at(0) == 34);
    for (int k : {3, -3})
        CHECK(opa_at(k) == 29);
    for (int k : {6, -6})
        CHECK(opa_at(k) == 19);
    for (int k : {10, -10})
        CHECK(opa_at(k) == 6);
    for (int k : {12, -12})
        CHECK(opa_at(k) == 3);
    for (int i = 0; i < n; ++i)
        CHECK(rows[i].opa >= 2);

    // Light theme: a stronger band.
    const int ln = sheen_rows(b, lid_h, 0.60f, span, rows);
    REQUIRE(ln > 0);
    auto light_at = [&](int k) {
        for (int i = 0; i < ln; ++i)
            if (std::fabs(rows[i].y - (73.835f + k)) < 0.01f)
                return (int)rows[i].opa;
        return -1;
    };
    CHECK(light_at(0) == 79);
    CHECK(light_at(6) == 43);

    // A short lid still fades in and out inside its own width.
    TrayBox short_box = b;
    short_box.fr = 150;
    REQUIRE(sheen_rows(short_box, lid_h, 0.26f, span, rows) > 0);
    CHECK(span.x0 < span.full0);
    CHECK(span.full0 <= span.full1);
    CHECK(span.full1 < span.x1);
    // A lid with no width has no band.
    short_box.fr = short_box.fl;
    CHECK(sheen_rows(short_box, lid_h, 0.26f, span, rows) == 0);
}

TEST_CASE("tray projection: per-lane lids meet the box corners", "[ams][tray]") {
    const TrayBox b = reference_box();
    CHECK(lane_lid_half_width(97, b) == Approx(97.0f / 2 - 25.364f / 4 - 1).margin(0.001));
    CHECK(lane_lid_half_width(72, b) == Approx(28.659).margin(0.001));

    // A row at pitch 72 whose box is derived from its lids.
    const float h = lane_lid_half_width(72, b);
    const float slots[4] = {58, 130, 202, 274};
    TrayBox row = b;
    row.fl = slots[0] - h;
    row.fr = slots[3] + h;
    CHECK(slots[0] - h == Approx(row.fl));
    const float lid_h = lid_height(row, FLANGE_RY);
    const PointF last_cap_back = cap_point(row, lid_h, slots[3] + h, 0);
    const PointF br_t = tray_faces(row).back_wall[1];
    check_point(last_cap_back, br_t.x, br_t.y);
    // A lid's right cap [x+h, x+h+S] overlaps the next lid's left end by S/2 at most.
    const float s = DEPTH_SKEW * b.depth;
    const float overlap = (slots[0] + h + s) - (slots[1] - h);
    CHECK(overlap <= s / 2 + 0.001f);
}

TEST_CASE("tray projection: slot pitch from the spool size", "[ams][tray]") {
    // Footprint 21 + 2 * 11, half the 25.4 px cap tuck, 16 px clear.
    CHECK(spool_pitch(61) == 72);
}

TEST_CASE("tray projection: any climate data gets glass", "[ams][tray]") {
    helix::AmsUnit unit;
    unit.slots.resize(4);
    SECTION("unit environment, tray") {
        unit.environment = helix::EnvironmentData{};
        CHECK(lid_mode(unit, true, false) == LidMode::Unit);
    }
    SECTION("unit environment, no tray") {
        unit.environment = helix::EnvironmentData{};
        CHECK(lid_mode(unit, false, false) == LidMode::None);
    }
    SECTION("dryer only, tray") {
        CHECK(lid_mode(unit, true, true) == LidMode::Unit);
    }
    SECTION("per-slot environment only, tray") {
        unit.slots[2].environment = helix::EnvironmentData{};
        CHECK(lid_mode(unit, true, false) == LidMode::PerLane);
    }
    SECTION("no climate data") {
        CHECK(lid_mode(unit, true, false) == LidMode::None);
    }
}

TEST_CASE("slot layout: a pitch cap holds up to four slots at the left", "[ams][tray]") {
    const AmsSlotLayout capped = calculate_ams_slot_layout(470, 4, 72);
    CHECK(capped.slot_width == 72);
    CHECK(capped.centering_offset == 0);
    const AmsSlotLayout narrow = calculate_ams_slot_layout(200, 4, 72);
    CHECK(narrow.slot_width == 50);
    // Five or more overlap as before; the cap does not apply.
    CHECK(calculate_ams_slot_layout(470, 8, 72).slot_width ==
          calculate_ams_slot_layout(470, 8).slot_width);
}
