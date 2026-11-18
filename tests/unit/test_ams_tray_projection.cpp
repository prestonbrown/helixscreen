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
using helix::ui::ams_stagger_label_width;
using helix::ui::ams_stagger_row;
using helix::ui::ams_stagger_row_height;
using helix::ui::ams_stepper_horizontal;
using helix::ui::AmsLabelRow;

namespace {

constexpr float PI = 3.14159265358979f;
constexpr float kFlangeRy = 0.42f * 61;

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
    CHECK(kDepthSkew * b.depth == Approx(25.364).margin(0.001));
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
    CHECK(spool_front_cy(b, kFlangeRy) == Approx(98.38).margin(0.001));
    const PointF c = spool_center(b, 58, kFlangeRy);
    check_point(c, 70.682f, 95.88f);
    // The spool's bottom lies below the front-wall top: the wall covers it.
    CHECK(c.y + kFlangeRy > b.ft);
}

TEST_CASE("tray projection: the lid clears the spools by one pixel", "[ams][tray]") {
    const TrayBox b = reference_box();
    const float lid_h = lid_height(b, kFlangeRy);
    CHECK(lid_h == Approx(24.24).margin(0.001));
    check_point(cap_point(b, lid_h, 392, PI), 392, 101);
    check_point(cap_point(b, lid_h, 392, 0), 417.364f, 86);
    const PointF crest = cap_point(b, lid_h, 392, PI / 2);
    check_point(crest, 404.682f, 69.26f);
    const PointF spool = spool_center(b, 58, kFlangeRy);
    CHECK(spool.y - kFlangeRy - crest.y == Approx(1).margin(0.001));

    // The hull's top is above the mid-depth crest: the chord slopes up to the back.
    CHECK(unit_top_y(b, lid_h, true) == Approx(68.13).margin(0.01));
    CHECK(unit_top_y(b, 0, false) == Approx(86));
}

TEST_CASE("tray projection: the cap profile runs front to back above its chord", "[ams][tray]") {
    const TrayBox b = reference_box();
    const float lid_h = lid_height(b, kFlangeRy);
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
    const float lid_h = lid_height(b, kFlangeRy);
    check_point(cap_point(b, lid_h, 10, 112 * PI / 180), 17.931f, 73.835f);
    check_point(cap_point(b, lid_h, 392, 112 * PI / 180), 399.931f, 73.835f);

    SheenSpan span{};
    SheenRow rows[kSheenMaxRows];
    const int n = sheen_rows(b, lid_h, 0.26f, span, rows);
    CHECK(span.x0 == Approx(37.931).margin(0.001));
    CHECK(span.full0 == Approx(107.931).margin(0.001));
    CHECK(span.full1 == Approx(259.931).margin(0.001));
    CHECK(span.x1 == Approx(369.931).margin(0.001));
    REQUIRE(n > 0);
    REQUIRE(n <= kSheenMaxRows);
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
    const float lid_h = lid_height(row, kFlangeRy);
    const PointF last_cap_back = cap_point(row, lid_h, slots[3] + h, 0);
    const PointF br_t = tray_faces(row).back_wall[1];
    check_point(last_cap_back, br_t.x, br_t.y);
    // A lid's right cap [x+h, x+h+S] overlaps the next lid's left end by S/2 at most.
    const float s = kDepthSkew * b.depth;
    const float overlap = (slots[0] + h + s) - (slots[1] - h);
    CHECK(overlap <= s / 2 + 0.001f);
}

TEST_CASE("tray projection: spools stand at the real units' pitch", "[ams][tray]") {
    // A 61 px spool is 17.9 px between flanges; real units space spools 1.35 widths apart.
    CHECK(spool_pitch(61) == 24);
    CHECK(spool_pitch(96) == 38);
    // Closer than a spool's own footprint (width plus a flange ellipse each side), so
    // neighbouring flanges overlap as they do in a photo.
    CHECK(spool_pitch(96) < 96 * kSpoolWidth + 2 * kDepthSkew * kSpoolFlangeRadius * 96);
}

TEST_CASE("tray projection: an end cap clears only the outer flange", "[ams][tray]") {
    const UnitShell* shell = unit_shell_by_id("ace_pro");
    REQUIRE(shell != nullptr);
    CHECK(end_cap_width(shell, 110.0f) ==
          Catch::Approx(110.0f * kSpoolWidth / 2.0f + shell->cap_clearance_px));
    CHECK(end_cap_width(shell, 55.0f) ==
          Catch::Approx(55.0f * kSpoolWidth / 2.0f + shell->cap_clearance_px / 2.0f));
    // The generic box clears a flange's depth.
    CHECK(end_cap_width(nullptr, 100.0f) ==
          Catch::Approx(100.0f * kSpoolWidth / 2.0f + kDepthSkew * kSpoolFlangeRadius * 100.0f));
}

TEST_CASE("tray projection: a unit's width runs cap to cap plus its side face", "[ams][tray]") {
    const float s = 96;
    CHECK(unit_width(4, s) == Catch::Approx(3 * spool_pitch(s) + 2 * end_cap_width(nullptr, s) +
                                            kDepthSkew * box_depth(s)));
    CHECK(unit_width(1, s) ==
          Catch::Approx(2 * end_cap_width(nullptr, s) + kDepthSkew * box_depth(s)));
}

TEST_CASE("tray projection: the spool grows until its unit fills the width", "[ams][tray]") {
    for (int lanes : {1, 4, 6, 8}) {
        for (float width : {200.0f, 277.0f, 416.0f}) {
            CAPTURE(lanes, width);
            const int32_t size = fit_spool_size(width, lanes, 400);
            CHECK(unit_width(lanes, (float)size) <= width);
            CHECK(unit_width(lanes, (float)(size + 1)) > width);
        }
    }
    // The cap holds when the width has room for more.
    CHECK(fit_spool_size(416.0f, 4, 96) == 96);
}

TEST_CASE("tray projection: leader dots keep their spacing round a corner", "[ams][tray]") {
    const PointF pts[3] = {{10, 0}, {10, 7}, {14, 10}}; // 7 down, then 5 across the diagonal
    PointF dots[16];
    const int n = leader_dots(pts, 3, 3.0f, dots, 16);
    // At 0, 3 and 6 down the drop, then 9 and 12 along the whole run: 2 and 5 into the
    // diagonal, whose 3-4-5 slope puts them at (11.6, 8.2) and (14, 10).
    REQUIRE(n == 5);
    CHECK((dots[0].x == 10 && dots[0].y == 0));
    CHECK((dots[1].x == 10 && dots[1].y == 3));
    CHECK((dots[2].x == 10 && dots[2].y == 6));
    CHECK((dots[3].x == 12 && dots[3].y == 8));
    CHECK((dots[4].x == 14 && dots[4].y == 10));
    // The budget caps the count.
    CHECK(leader_dots(pts, 3, 3.0f, dots, 2) == 2);
}

TEST_CASE("tray projection: a cheek climbs from the front wall to the back", "[ams][tray]") {
    const UnitShell* shell = unit_shell_by_id("ace_pro");
    REQUIRE(shell != nullptr);
    TrayBox b{};
    b.fl = 0;
    b.fr = 100;
    b.ft = 50;
    b.fb = 90;
    b.depth = 100;
    b.rise = 10;
    const PointF front = cheek_point(b, *shell, 110.0f, 100, 0);
    const PointF back = cheek_point(b, *shell, 110.0f, 100, shell->cheek_n - 1);
    CHECK(front.y == Catch::Approx(50.0f));
    CHECK(back.y == Catch::Approx(50.0f - shell->back_extra_px - b.rise));
    CHECK(back.x > front.x);
}

TEST_CASE("tray projection: a lid's end comes down to the body's contour", "[ams][tray]") {
    const UnitShell* shell = unit_shell_by_id("ace_pro");
    REQUIRE(shell != nullptr);
    TrayBox b{};
    b.fr = 100;
    b.ft = 50;
    b.fb = 90;
    b.depth = 100;
    b.rise = 10;
    // Ahead of the knee the body is level, so the glass reaches the front wall's height.
    const PointF level = cheek_under(b, *shell, 110.0f, 100, proj(b, 100, 0, 20));
    CHECK(level.y == Approx(proj(b, 100, 50, 20).y));
    // Past it the glass stops on the climbing edge, well above a straight run to the floor.
    const PointF back = cheek_under(b, *shell, 110.0f, 100, proj(b, 100, 0, 100));
    CHECK(back.y == Approx(50.0f - shell->back_extra_px - b.rise));
}

TEST_CASE("tray projection: a unit's lid takes its real profile", "[ams][tray]") {
    const UnitShell* shell = unit_shell_by_id("ace_pro");
    REQUIRE(shell != nullptr);
    TrayBox b{};
    b.fr = 100;
    b.ft = 50;
    b.fb = 90;
    b.depth = 100;
    b.rise = 0;
    PointF out[17];
    shell_cap_polyline(b, *shell, 110.0f, 100, out, 17);
    CHECK(out[0].y == Approx(50.0f));
    CHECK(out[16].y == Approx(50.0f - shell->back_extra_px).margin(0.01));
    // The middle of the lid sits on the flat top, at the profile's full height.
    float crest = 0;
    for (int i = 0; i < shell->dome_n; ++i)
        crest = std::max(crest, shell->dome[i].height_px);
    CHECK(out[8].y == Approx(50.0f - crest).margin(0.01));
    CHECK(out[8].y < out[16].y);
}

TEST_CASE("tray projection: front parts anchor to the row, a lane or an end", "[ams][tray]") {
    TrayBox b{};
    b.fl = 100;
    b.fr = 300;
    b.ft = 50;
    b.fb = 90;
    float box[4];
    // At half the reference spool, every length halves.
    front_part_box(b, {FrontAnchor::Span, 4, 8, 2, 6, 0, 0}, 55.0f, 0, box);
    CHECK(box[0] == Approx(102.0f));
    CHECK(box[2] == Approx(296.0f));
    CHECK(box[1] == Approx(51.0f));
    CHECK(box[3] == Approx(53.0f));
    front_part_box(b, {FrontAnchor::Lane, -2, 2, -4, 0, 0, 0}, 55.0f, 150, box);
    CHECK(box[0] == Approx(149.0f));
    CHECK(box[2] == Approx(151.0f));
    CHECK(box[1] == Approx(48.0f));
    front_part_box(b, {FrontAnchor::Center, -2, 2, 0, 1, 0, 0}, 55.0f, 0, box);
    CHECK(box[0] == Approx(199.0f));
    front_part_box(b, {FrontAnchor::Left, 2, 6, 0, 1, 0, 0}, 55.0f, 0, box);
    CHECK(box[0] == Approx(101.0f));
    // A part wider than the row stops at the unit's end.
    front_part_box(b, {FrontAnchor::Left, 2, 900, 0, 1, 0, 0}, 55.0f, 0, box);
    CHECK(box[2] == Approx(300.0f));
    front_part_box(b, {FrontAnchor::Right, 2, 6, 0, 1, 0, 0}, 55.0f, 0, box);
    CHECK(box[0] == Approx(297.0f));
    CHECK(box[2] == Approx(299.0f));
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
    SECTION("a drawn unit: its own lids, or none without a dome") {
        CHECK(lid_mode(*helix::ui::tray::unit_shell_by_id("cfs")) == LidMode::Unit);
        CHECK(lid_mode(*helix::ui::tray::unit_shell_by_id("emu")) == LidMode::PerLane);
        CHECK(lid_mode(*helix::ui::tray::unit_shell_by_id("night_owl")) == LidMode::None);
    }
}

TEST_CASE("slot layout: a pitch cap holds up to four slots, centered", "[ams][tray]") {
    const AmsSlotLayout capped = calculate_ams_slot_layout(470, 4, 72);
    CHECK(capped.slot_width == 72);
    CHECK(capped.centering_offset == (470 - 4 * 72) / 2);
    CHECK(calculate_ams_slot_layout(470, 1, 72).centering_offset == (470 - 72) / 2);
    const AmsSlotLayout narrow = calculate_ams_slot_layout(200, 4, 72);
    CHECK(narrow.slot_width == 50);
    CHECK(narrow.centering_offset == 0);
    // Five or more overlap as before; the cap does not apply.
    CHECK(calculate_ams_slot_layout(470, 8, 72).slot_width ==
          calculate_ams_slot_layout(470, 8).slot_width);
}

TEST_CASE("stagger rows: 1.5 lines from the medium breakpoint up", "[ams][tray][stagger]") {
    CHECK(ams_stagger_row_height(22, UiBreakpoint::Medium) == 33);
    CHECK(ams_stagger_row_height(22, UiBreakpoint::Large) == 33);
    CHECK(ams_stagger_row_height(26, UiBreakpoint::XXLarge) == 39);
}

TEST_CASE("stagger rows: one line apart below the medium breakpoint", "[ams][tray][stagger]") {
    CHECK(ams_stagger_row_height(15, UiBreakpoint::Micro) == 15);
    CHECK(ams_stagger_row_height(15, UiBreakpoint::Tiny) == 15);
    CHECK(ams_stagger_row_height(18, UiBreakpoint::Small) == 18);
}

TEST_CASE("stagger rows never overlap their own line", "[ams][tray][stagger]") {
    for (int lh = 8; lh <= 40; ++lh) {
        for (int bp = 0; bp <= 6; ++bp)
            CHECK(ams_stagger_row_height(lh, as_breakpoint(bp)) >= lh);
    }
}

TEST_CASE("stagger rows: below medium the lanes alternate between two rows",
          "[ams][tray][stagger]") {
    for (int bp = 0; bp <= 2; ++bp) {
        for (int count : {5, 7, 8}) {
            CAPTURE(bp, count);
            for (int i = 0; i < count; ++i) {
                const AmsLabelRow r = ams_stagger_row(i, count, as_breakpoint(bp));
                CHECK(r.rows == 2);
                CHECK(r.row == i % 2);
            }
        }
    }
}

TEST_CASE("stagger rows: from medium up, three rows from seven lanes, two from five",
          "[ams][tray][stagger]") {
    // High, mid, low, mid, high...
    const int three[] = {2, 1, 0, 1, 2, 1, 0, 1};
    for (int i = 0; i < 8; ++i) {
        CAPTURE(i);
        const AmsLabelRow r = ams_stagger_row(i, 8, UiBreakpoint::Medium);
        CHECK(r.rows == 3);
        CHECK(r.row == three[i]);
    }
    CHECK(ams_stagger_row(0, 5, UiBreakpoint::Large).rows == 2);
    CHECK(ams_stagger_row(0, 5, UiBreakpoint::Large).row == 1);
    CHECK(ams_stagger_row(1, 5, UiBreakpoint::Large).row == 0);
}

TEST_CASE("stagger rows: four lanes or fewer stand in one row", "[ams][tray][stagger]") {
    for (int bp = 0; bp <= 6; ++bp) {
        const AmsLabelRow r = ams_stagger_row(3, 4, as_breakpoint(bp));
        CHECK(r.rows == 1);
        CHECK(r.row == 0);
    }
}

TEST_CASE("stagger labels: below medium a label keeps to its two-lane span",
          "[ams][tray][stagger]") {
    CHECK(ams_stagger_label_width(30, UiBreakpoint::Micro, 4) == 56);
    CHECK(ams_stagger_label_width(30, UiBreakpoint::Small, 4) == 56);
    // From medium up labels are not narrowed.
    CHECK(ams_stagger_label_width(30, UiBreakpoint::Medium, 4) == 0);
    CHECK(ams_stagger_label_width(0, UiBreakpoint::Micro, 4) == 0);
}

TEST_CASE("labels over a drawn unit: the fewest rows that give every label its room",
          "[ams][tray][stagger][over-lane]") {
    using helix::ui::ams_over_lane_label_rows;
    const float x[4] = {0, 40, 80, 120};
    const int32_t fit[4] = {36, 36, 36, 36};
    CHECK(ams_over_lane_label_rows(x, fit, fit, 4, 4, -40, 160) == 1);
    const int32_t wide[4] = {60, 60, 60, 60};
    CHECK(ams_over_lane_label_rows(x, wide, wide, 4, 4, -40, 160) == 2);
    const int32_t wider[4] = {100, 100, 100, 100};
    CHECK(ams_over_lane_label_rows(x, wider, wider, 4, 4, -40, 160) == 3);
    // Whole text first: two rows show it all though one row would keep a few characters.
    CHECK(ams_over_lane_label_rows(x, wide, fit, 4, 4, -40, 160) == 2);
    // Too long for any rows whole: the fewest rows that keep a few characters.
    const int32_t huge[4] = {500, 500, 500, 500};
    CHECK(ams_over_lane_label_rows(x, huge, fit, 4, 4, -40, 160) == 1);
}

TEST_CASE("labels over a drawn unit: spans clear neighbours and the ticks rising past them",
          "[ams][tray][stagger][over-lane]") {
    using helix::ui::AmsLabelSpan;
    struct Case {
        const char* what;
        int n;
        float x[2];
        int row[2];
        int32_t need[2];
        float lo, hi;
        int32_t left[2], right[2]; // -1: not pinned
    };
    const Case cases[] = {
        {"same row: each stops a gap short of halfway",
         2,
         {0, 40},
         {0, 0},
         {100, 100},
         -1000,
         1000,
         {-1, 18},
         {18, -1}},
        // The higher label has no tick under it to clear.
        {"a higher neighbour's tick: the label stops a gap short of it",
         2,
         {0, 40},
         {0, 1},
         {100, 100},
         -1000,
         1000,
         {-1, 50},
         {36, 50}},
        {"crowded on one side, it slides to the other, its own tick still under it",
         2,
         {0, 20},
         {0, 1},
         {40, 10},
         -1000,
         1000,
         {24, -1},
         {16, -1}},
        {"never past the row's ends", 1, {10, 0}, {0, 0}, {40, 0}, 0, 100, {10, -1}, {30, -1}},
        {"centered where both sides allow",
         1,
         {0, 0},
         {0, 0},
         {30, 0},
         -1000,
         1000,
         {15, -1},
         {15, -1}},
    };
    for (const Case& t : cases) {
        CAPTURE(t.what);
        AmsLabelSpan s[2];
        helix::ui::ams_over_lane_label_spans(t.x, t.row, t.need, t.n, 4, t.lo, t.hi, s);
        for (int k = 0; k < t.n; ++k) {
            CAPTURE(k);
            if (t.left[k] >= 0)
                CHECK(s[k].left == t.left[k]);
            if (t.right[k] >= 0)
                CHECK(s[k].right == t.right[k]);
        }
        if (t.n == 2 && t.row[0] == t.row[1])
            CHECK(t.x[0] + (float)s[0].right < t.x[1] - (float)s[1].left);
        if (t.need[0] < 100)
            CHECK(s[0].left + s[0].right == t.need[0]);
    }
}

TEST_CASE("lane numbers under a drawn unit: one row, then two, then no \"+\"",
          "[ams][tray][lane-badges]") {
    using helix::ui::ams_lane_badge_layout;
    const int32_t number[4] = {16, 16, 16, 16};
    const int32_t plus[4] = {26, 26, 26, 26};
    const float roomy[4] = {0, 40, 80, 120};
    CHECK(ams_lane_badge_layout(roomy, number, plus, 4, 4).rows == 1);
    CHECK(ams_lane_badge_layout(roomy, number, plus, 4, 4).plus);
    const float close[4] = {0, 20, 40, 60};
    CHECK(ams_lane_badge_layout(close, number, plus, 4, 4).rows == 2);
    CHECK(ams_lane_badge_layout(close, number, plus, 4, 4).plus);
    const float tight[4] = {0, 12, 24, 36};
    CHECK(ams_lane_badge_layout(tight, number, plus, 4, 4).rows == 2);
    CHECK_FALSE(ams_lane_badge_layout(tight, number, plus, 4, 4).plus);
}

TEST_CASE("lane badge: on the front face, inside the lane's bay", "[ams][tray][badge]") {
    const TrayBox b = reference_box(); // front face y 101..126
    SECTION("a wide pitch puts it at the lane's front bottom-right") {
        const PointF p = lane_badge_origin(b, 100, 40, 20, 20, 2);
        CHECK(p.x == Approx(118));
        CHECK(p.y == Approx(102));
        CHECK(p.y + 20 <= b.fb);
        CHECK(p.y >= b.ft);
    }
    SECTION("a tight pitch centers it on the lane") {
        const PointF p = lane_badge_origin(b, 100, 6, 16, 16, 2);
        CHECK(p.x == Approx(92));
        CHECK(p.y == Approx(106));
    }
    SECTION("a badge taller than the face keeps to its top, never below the box") {
        TrayBox low = b;
        low.ft = low.fb - 12;
        const PointF p = lane_badge_origin(low, 100, 40, 16, 16, 2);
        CHECK(p.y == Approx(low.ft));
    }
}

TEST_CASE("env pill: stands at the box's back-right corner", "[ams][tray][env_chip]") {
    const TrayBox b = reference_box();
    CHECK(pill_left_x(b) == Approx(b.fr + kDepthSkew * b.depth));
    CHECK(pill_left_x(b) == Approx(tray_faces(b).back_wall[1].x));
}

TEST_CASE("step progress: horizontal only on a narrow portrait screen", "[ams][tray][stepper]") {
    // Landscape keeps the vertical list at every size.
    for (int bp = 0; bp <= 6; ++bp)
        CHECK_FALSE(ams_stepper_horizontal(false, as_breakpoint(bp)));
    // Portrait below medium has no height for a vertical list under the strip.
    CHECK(ams_stepper_horizontal(true, UiBreakpoint::Micro));
    CHECK(ams_stepper_horizontal(true, UiBreakpoint::Tiny));
    CHECK(ams_stepper_horizontal(true, UiBreakpoint::Small));
    CHECK_FALSE(ams_stepper_horizontal(true, UiBreakpoint::Medium));
    CHECK_FALSE(ams_stepper_horizontal(true, UiBreakpoint::Large));
}
