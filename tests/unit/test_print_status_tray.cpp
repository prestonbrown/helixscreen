// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The filament lane tray on Print Status: the gate that decides whether a
// printer has one, and where each layout puts it.

#include "ui_ams_lane_spool.h"
#include "ui_ams_mini_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_state.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "print_status_tile_layout.h"
#include "target_tool.h"

#include <set>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("the lane tray gate opens for a filament system or several heads",
          "[print_status_tray]") {
    CHECK_FALSE(ui::has_filament_system_or_multi_tool(0, false));
    CHECK(ui::has_filament_system_or_multi_tool(4, false));
    CHECK(ui::has_filament_system_or_multi_tool(1, false));
    CHECK(ui::has_filament_system_or_multi_tool(0, true));
    CHECK(ui::has_filament_system_or_multi_tool(8, true));
}

TEST_CASE_METHOD(LVGLUITestFixture, "TargetTool publishes the lane tray gate from the lane count",
                 "[print_status_tray]") {
    AmsState::instance().init_subjects();
    // Attach again so the target follows the AmsState that now stands, as
    // the app attaches after AmsState registers.
    ui::TargetTool::instance().attach(get_printer_state());
    lv_subject_t* gate = lv_xml_get_subject(nullptr, "has_filament_system_or_multi_tool");
    REQUIRE(gate != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "target_tool_multi_head") != nullptr);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "target_tool_multi_head")) == 0);

    lv_subject_t* slots = AmsState::instance().get_slot_count_subject();
    const int original = lv_subject_get_int(slots);
    lv_subject_set_int(slots, 0);
    ui::UpdateQueue::instance().drain();
    CHECK(lv_subject_get_int(gate) == 0);
    lv_subject_set_int(slots, 4);
    ui::UpdateQueue::instance().drain();
    CHECK(lv_subject_get_int(gate) == 1);
    lv_subject_set_int(slots, 0);
    ui::UpdateQueue::instance().drain();
    CHECK(lv_subject_get_int(gate) == 0);
    lv_subject_set_int(slots, original);
    ui::UpdateQueue::instance().drain();
}

TEST_CASE("ultrawide puts the lane tray in the readouts' column only when gated",
          "[print_status_tray][ultrawide]") {
    const auto plain = decide_print_status_ultrawide_layout(1880, 400, 12);
    const auto tray = decide_print_status_ultrawide_layout(1880, 400, 12, true);
    CHECK(plain.lane_tray == PrintStatusLaneTray::None);
    CHECK(tray.lane_tray == PrintStatusLaneTray::Column);
    CHECK(tray.lane_tray_h == 0);
    // The tray takes the column as it stands: no other column moves.
    CHECK(tray.columns.readouts_w == plain.columns.readouts_w);
    CHECK(tray.columns.info_w == plain.columns.info_w);
}

namespace {
// A printer tile floor below every tile these sizes give it.
constexpr int32_t kFloor = 150;

// The strip heights and gap measured at each portrait size.
PrintStatusTileLayout portrait(int32_t w, int32_t h, int32_t strip_h, bool tray,
                               int32_t printer_floor = kFloor) {
    return decide_print_status_tile_layout(w, h, 12, 48, strip_h, tray, printer_floor);
}

bool stacks_inside(const PrintStatusTileLayout& l, int32_t content_h) {
    return l.job_h + l.printer_h + 12 + l.strip_h + 12 + l.lane_tray_h + 12 <= content_h;
}
} // namespace

TEST_CASE("a stacked panel shows the lane tray strip only when gated", "[print_status_tray]") {
    const auto plain = portrait(560, 804, 231, false);
    const auto tray = portrait(560, 804, 231, true);
    REQUIRE(plain.arrangement == PrintStatusArrangement::Stacked);
    CHECK(plain.lane_tray == PrintStatusLaneTray::None);
    CHECK(plain.lane_tray_h == 0);
    CHECK(tray.lane_tray == PrintStatusLaneTray::Strip);
    CHECK(tray.lane_tray_h > 0);
    // The tiles give up the tray's height and one gap, and nothing overflows.
    CHECK(tray.job_h + tray.printer_h + tray.lane_tray_h + 12 == plain.job_h + plain.printer_h);
    CHECK(stacks_inside(tray, 804));
}

TEST_CASE("the lane tray strip shrinks with the room the tiles can spare", "[print_status_tray]") {
    const auto tall = portrait(448, 1060, 219, true);  // 480x1280
    const auto medium = portrait(560, 804, 231, true); // 600x1024
    const auto small = portrait(448, 610, 203, true);  // 480x800
    // Both tall sizes have room past the tray's natural height: it stops
    // there and the tiles keep the rest.
    CHECK(tall.lane_tray_h == medium.lane_tray_h);
    CHECK(tall.job_h > medium.job_h);
    CHECK(medium.lane_tray_h > small.lane_tray_h);
    CHECK(small.lane_tray == PrintStatusLaneTray::Strip);
    CHECK(small.lane_tray_h > 0);
    for (const auto& [l, h] : {std::pair{tall, 1060}, {medium, 804}, {small, 610}}) {
        CHECK(stacks_inside(l, h));
        // The printer tile never goes below the height its chips need.
        CHECK(l.printer_h >= kFloor);
    }
}

TEST_CASE("the lane tray takes its height from the preview before the printer picture",
          "[print_status_tray]") {
    // The preview has room for the whole tray: the printer tile keeps its height.
    const auto tall = portrait(448, 1060, 219, true);
    const auto tall_plain = portrait(448, 1060, 219, false);
    REQUIRE(tall.lane_tray == PrintStatusLaneTray::Strip);
    CHECK(tall.printer_h == tall_plain.printer_h);
    CHECK(tall.job_h == tall_plain.job_h - tall.lane_tray_h - 12);

    // A picture already at its floor gives nothing: the tray is what the
    // preview can spare, and the printer tile keeps every pixel.
    const auto plain = portrait(560, 804, 231, false);
    const auto at_floor = portrait(560, 804, 231, true, plain.printer_h);
    REQUIRE(at_floor.lane_tray == PrintStatusLaneTray::Strip);
    CHECK(at_floor.printer_h == plain.printer_h);
    CHECK(at_floor.lane_tray_h < portrait(560, 804, 231, true).lane_tray_h);

    // The preview runs out first, then the picture gives down to its floor exactly.
    const auto small = portrait(448, 610, 203, true);
    const auto small_plain = portrait(448, 610, 203, false);
    REQUIRE(small.lane_tray == PrintStatusLaneTray::Strip);
    CHECK(small.job_h < small_plain.job_h);
    CHECK(small.printer_h < small_plain.printer_h);
    CHECK(small.printer_h >= kFloor);
}

TEST_CASE("an unknown printer floor spares nothing from the picture", "[print_status_tray]") {
    const auto plain = portrait(560, 804, 231, false, 0);
    const auto tray = portrait(560, 804, 231, true, 0);
    REQUIRE(tray.lane_tray == PrintStatusLaneTray::Strip);
    CHECK(tray.printer_h == plain.printer_h);
    // 480x800: the preview alone cannot spare the tray's smallest.
    CHECK(portrait(448, 610, 203, true, 0).lane_tray == PrintStatusLaneTray::None);
}

TEST_CASE("too little room shows no lane tray rather than a cramped one", "[print_status_tray]") {
    // Just tall enough to stack, with a tall strip: the tiles have nothing to spare.
    const auto l = portrait(400, 520, 220, true);
    REQUIRE(l.arrangement == PrintStatusArrangement::Stacked);
    CHECK(l.lane_tray == PrintStatusLaneTray::None);
    CHECK(l.lane_tray_h == 0);
}

TEST_CASE("side by side tiles never show the lane tray", "[print_status_tray]") {
    // Landscape 800x480, a landscape box with height to spare for a tray,
    // and a short portrait panel.
    for (const auto& [w, h] : {std::pair{760, 414}, {976, 520}, {288, 400}}) {
        const auto l = decide_print_status_tile_layout(w, h, 12, 48, 96, true);
        REQUIRE(l.arrangement == PrintStatusArrangement::SideBySide);
        CHECK(l.lane_tray == PrintStatusLaneTray::None);
        CHECK(l.lane_tray_h == 0);
        const auto plain = decide_print_status_tile_layout(w, h, 12, 48, 96, false);
        CHECK(l.job_h == plain.job_h);
        CHECK(l.printer_h == plain.printer_h);
    }
}

TEST_CASE("the tray draws lanes only with some lane present", "[print_status_tray][ams_mini]") {
    CHECK_FALSE(ui_ams_mini_status_shows_lanes(0, 0));
    CHECK_FALSE(ui_ams_mini_status_shows_lanes(4, 0)); // every unit absent
    CHECK(ui_ams_mini_status_shows_lanes(4, 4));
    CHECK(ui_ams_mini_status_shows_lanes(8, 4));
}

namespace {

/// The laid-out lane spools of a 4-lane tray in a `w` x `h` box.
std::vector<lv_obj_t*> tray_spools(lv_obj_t* screen, int32_t w, int32_t h, bool fill) {
    lv_obj_t* host = lv_obj_create(screen);
    lv_obj_set_size(host, w, h);
    lv_obj_set_style_pad_all(host, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(host, 0, LV_PART_MAIN);
    lv_obj_t* tray = ui_ams_mini_status_create(host, h);
    REQUIRE(tray != nullptr);
    lv_obj_set_size(tray, lv_pct(100), lv_pct(100));
    ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(host);
    ui_ams_mini_status_set_fill(tray, fill);
    ui_ams_mini_status_set_slot_count(tray, 4);
    for (int i = 0; i < 4; ++i) {
        ui_ams_mini_status_set_slot_label(tray, i, "PLA", 50);
    }
    ui_ams_mini_status_set_width(tray, w);
    ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(host);
    ui_ams_mini_status_refresh(tray);
    lv_obj_update_layout(host);
    std::vector<lv_obj_t*> spools;
    lv_obj_t* sc = lv_obj_find_by_name(tray, "ams_spools_container");
    REQUIRE(sc != nullptr);
    for (uint32_t i = 0; i < lv_obj_get_child_count(sc); ++i) {
        if (lv_obj_t* spool = lv_obj_find_by_name(lv_obj_get_child(sc, i), "lane_spool")) {
            spools.push_back(spool);
        }
    }
    return spools;
}

std::set<int32_t> rows_of(const std::vector<lv_obj_t*>& spools) {
    std::set<int32_t> ys;
    for (lv_obj_t* s : spools) {
        lv_area_t a;
        lv_obj_get_coords(s, &a);
        ys.insert(a.y1);
    }
    return ys;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a fill tray sizes its spools from its height; the home tray keeps one row",
                 "[print_status_tray][ams_mini]") {
    ui_ams_mini_status_init();
    // The ultrawide column at 1920x480.
    const auto home = tray_spools(test_screen(), 360, 388, false);
    const auto fill = tray_spools(test_screen(), 360, 388, true);
    REQUIRE(home.size() == 4);
    REQUIRE(fill.size() == 4);

    // The home tray: one row, spools at its cap however tall the box.
    CHECK(rows_of(home).size() == 1);
    for (lv_obj_t* s : home) {
        CHECK(lv_obj_get_height(s) <= 56 + ui::kAmsLaneSpoolBadgeMarginPx);
    }
    // A fill tray wraps into two rows of larger spools.
    CHECK(rows_of(fill).size() == 2);
    CHECK(lv_obj_get_height(fill[0]) > lv_obj_get_height(home[0]) + 32);
    // Its lane badges stay at the home tray's size rather than covering the spool.
    lv_obj_t* home_badge = lv_obj_find_by_name(lv_obj_get_parent(home[0]), "spool_badge_0");
    lv_obj_t* fill_badge = lv_obj_find_by_name(lv_obj_get_parent(fill[0]), "spool_badge_0");
    REQUIRE((home_badge && fill_badge));
    CHECK(lv_obj_get_width(fill_badge) == lv_obj_get_width(home_badge));
}
