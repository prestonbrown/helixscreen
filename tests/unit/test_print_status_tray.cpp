// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The filament lane tray on Print Status: the gate that decides whether a
// printer has one, and where each layout puts it.

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_state.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "print_status_tile_layout.h"
#include "target_tool.h"

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
// The strip heights and gap measured at each portrait size.
PrintStatusTileLayout portrait(int32_t w, int32_t h, int32_t strip_h, bool tray) {
    return decide_print_status_tile_layout(w, h, 12, 48, strip_h, tray);
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
    CHECK(tray.job_h + tray.printer_h + 12 + tray.strip_h + 12 + tray.lane_tray_h + 12 <= 804);
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
    // Every size still fits its content box.
    for (const auto& [l, h] : {std::pair{tall, 1060}, {medium, 804}, {small, 610}}) {
        CHECK(l.job_h + l.printer_h + 12 + l.strip_h + 12 + l.lane_tray_h + 12 <= h);
    }
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
