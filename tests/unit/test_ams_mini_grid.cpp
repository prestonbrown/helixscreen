// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_mini_grid.cpp
 * @brief The spool view's layouts: how plan_ams_mini_grid() packs units into
 * a box, how plan_ams_mini_scroll() lays out lanes that do not fit, and that
 * the widget never draws a lane below the usable minimum.
 */

#include "ui_ams_lane_spool.h"
#include "ui_ams_mini_status.h"
#include "ui_toolhead_badge.h"

#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "theme_manager.h"

#include <algorithm>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::AmsMiniFill;
using helix::ui::AmsMiniGridMetrics;
using helix::ui::AmsMiniGridPlan;
using helix::ui::plan_ams_mini_grid;

namespace {

/// The home tile's metrics at 800x480, titled (several units) or not.
AmsMiniGridMetrics metrics(bool titled, bool bypass = false) {
    AmsMiniGridMetrics m;
    m.gap = 2;
    m.group_gap = 4;
    m.group_pad = titled ? 2 : 0;
    m.title_h = titled ? 16 : 0;
    m.badge_margin = 8;
    m.text_h = 34;
    m.max_spool = 120;
    m.bypass = bypass;
    return m;
}

/// The width the plan's widest line takes, and the height all its lines take,
/// recomputed from the plan the way the widget lays it out.
struct Extent {
    int w = 0;
    int h = 0;
};
Extent extent_of(const AmsMiniGridPlan& p, const AmsMiniGridMetrics& m) {
    struct Line {
        int cells = 0, groups = 0, rows = 0;
    };
    std::vector<Line> lines;
    for (const auto& g : p.groups) {
        if (g.lanes <= 0)
            continue;
        if (lines.empty() || g.new_line)
            lines.push_back({});
        lines.back().cells += g.cols;
        lines.back().groups += 1;
        lines.back().rows = std::max(lines.back().rows, g.rows);
    }
    Extent e;
    for (const Line& l : lines) {
        int w = l.cells * p.cell_w + (l.cells - l.groups) * m.gap + l.groups * 2 * m.group_pad +
                (l.groups - 1) * m.group_gap;
        if (m.bypass)
            w += p.cell_w + 1 + 2 * m.gap;
        e.w = std::max(e.w, w);
        e.h += m.title_h + 2 * m.group_pad + l.rows * p.row_h + (l.rows - 1) * m.gap;
    }
    e.h += static_cast<int>(lines.size() - 1) * m.group_gap;
    return e;
}

} // namespace

// ============================================================================
// plan_ams_mini_grid
// ============================================================================

TEST_CASE("ams_mini grid: one unit in a wide box takes one row", "[ams_mini][grid]") {
    const AmsMiniGridMetrics m = metrics(false);
    const AmsMiniGridPlan p = plan_ams_mini_grid({4}, 466, 216, m);
    REQUIRE(p.groups.size() == 1);
    CHECK(p.groups[0].cols == 4);
    CHECK(p.groups[0].rows == 1);
    // Width-bound: four cells across 466px, gaps and rounding slack off.
    CHECK(p.cell_w == (466 - 3 * 2 - 2) / 4);
    CHECK(p.spool == p.cell_w - m.badge_margin);
}

TEST_CASE("ams_mini grid: a unit wider than a line wraps into even rows", "[ams_mini][grid]") {
    // Tall and narrow: eight lanes do better as two rows of four than one of eight.
    const AmsMiniGridPlan p = plan_ams_mini_grid({8}, 300, 300, metrics(false));
    REQUIRE(p.groups.size() == 1);
    CHECK(p.groups[0].rows == 2);
    CHECK(p.groups[0].cols == 4);
}

TEST_CASE("ams_mini grid: units stay whole and start a line when they do not fit",
          "[ams_mini][grid]") {
    // afc_direct's units in the 4x2 home tile at 800x480.
    const std::vector<int> units{4, 1, 4, 2, 4, 4};
    const AmsMiniGridMetrics m = metrics(true);
    const AmsMiniGridPlan p = plan_ams_mini_grid(units, 466, 216, m);
    REQUIRE(p.groups.size() == units.size());
    int lines = 0;
    for (size_t k = 0; k < units.size(); ++k) {
        INFO("unit " << k);
        CHECK(p.groups[k].lanes == units[k]);
        CHECK(p.groups[k].cols * p.groups[k].rows >= units[k]);
        lines += p.groups[k].new_line ? 1 : 0;
    }
    CHECK(p.groups[0].new_line);
    // Two lines beat one line of nineteen narrow cells.
    CHECK(lines == 2);
    CHECK(p.spool >= 24);
}

TEST_CASE("ams_mini grid: the bypass column comes off the lanes' width", "[ams_mini][grid]") {
    const AmsMiniGridPlan plain = plan_ams_mini_grid({8}, 466, 216, metrics(false));
    const AmsMiniGridPlan bypass = plan_ams_mini_grid({8}, 466, 216, metrics(false, true));
    CHECK(bypass.cell_w < plain.cell_w);
}

TEST_CASE("ams_mini grid: nothing to lay out plans nothing", "[ams_mini][grid]") {
    CHECK(plan_ams_mini_grid({}, 466, 216, metrics(false)).spool == 0);
    CHECK(plan_ams_mini_grid({0, 0}, 466, 216, metrics(true)).spool == 0);
    CHECK(plan_ams_mini_grid({4}, 0, 216, metrics(false)).spool == 0);
}

TEST_CASE("ams_mini grid: every plan fits its box", "[ams_mini][grid]") {
    std::mt19937 rng(20261122);
    std::uniform_int_distribution<int> unit_count(1, 8);
    std::uniform_int_distribution<int> lanes(1, 8);
    std::uniform_int_distribution<int> side(120, 900);
    for (int trial = 0; trial < 300; ++trial) {
        std::vector<int> units(unit_count(rng));
        for (int& n : units)
            n = lanes(rng);
        const int w = side(rng);
        const int h = side(rng);
        const AmsMiniGridMetrics m = metrics(units.size() >= 2, trial % 3 == 0);
        const AmsMiniGridPlan p = plan_ams_mini_grid(units, w, h, m);
        if (p.spool <= 0)
            continue;
        const Extent e = extent_of(p, m);
        INFO("trial " << trial << ": " << units.size() << " units in " << w << "x" << h << ", cell "
                      << p.cell_w << "x" << p.row_h);
        CHECK(e.w <= w);
        CHECK(e.h <= h);
        CHECK(p.spool <= m.max_spool);
        CHECK(p.spool + m.badge_margin <= p.cell_w);
        CHECK(p.spool + m.badge_margin + m.text_h <= p.row_h);
    }
}

// ============================================================================
// Grid or strip
// ============================================================================

TEST_CASE("ams_mini scroll: as many usable rows as the height holds", "[ams_mini][grid]") {
    using helix::ui::plan_ams_mini_scroll;
    const AmsMiniGridMetrics m = metrics(false);
    // 216px holds two rows of 36px spools (8 + 36 + 34 = 78 each), not three.
    const AmsMiniGridPlan p = plan_ams_mini_scroll({48}, 216, 50, 6, 36, m);
    REQUIRE(p.groups.size() == 1);
    CHECK(p.scrolls);
    CHECK(p.groups[0].rows == 2);
    CHECK(p.groups[0].cols == 24);
    CHECK(p.spool >= 36);
    CHECK(p.spool + m.badge_margin + m.text_h <= p.row_h);
    CHECK(2 * p.row_h + m.gap <= 216 - 6);
    // At least as wide as its text needs, and as its spool.
    CHECK(p.cell_w == std::max(50, p.spool + m.badge_margin));
}

TEST_CASE("ams_mini scroll: each unit keeps its own box in one line", "[ams_mini][grid]") {
    using helix::ui::plan_ams_mini_scroll;
    const AmsMiniGridPlan p = plan_ams_mini_scroll({1, 4, 4}, 216, 40, 6, 36, metrics(true));
    REQUIRE(p.groups.size() == 3);
    CHECK(p.groups[0].new_line);
    CHECK_FALSE(p.groups[1].new_line);
    CHECK_FALSE(p.groups[2].new_line);
    // A one-lane unit is one cell; four lanes in two rows are a 2x2 box.
    CHECK(p.groups[0].rows == 1);
    CHECK(p.groups[1].rows == 2);
    CHECK(p.groups[1].cols == 2);
}

TEST_CASE("ams_mini scroll: a box too short for one usable row plans nothing", "[ams_mini][grid]") {
    using helix::ui::plan_ams_mini_scroll;
    const AmsMiniGridMetrics m = metrics(false);
    // One row needs 8 + 36 + 34 = 78px plus the 6px scrollbar.
    CHECK(plan_ams_mini_scroll({8}, 84, 40, 6, 36, m).spool >= 36);
    CHECK(plan_ams_mini_scroll({8}, 83, 40, 6, 36, m).spool <= 0);
}

TEST_CASE("ams_mini grid: unit groups give way to one block when they cost a third",
          "[ams_mini][grid]") {
    using helix::ui::ams_mini_grid_prefers_flat;
    CHECK_FALSE(ams_mini_grid_prefers_flat(31, 36, 24));
    CHECK_FALSE(ams_mini_grid_prefers_flat(30, 45, 24));
    CHECK(ams_mini_grid_prefers_flat(29, 45, 24));
    CHECK(ams_mini_grid_prefers_flat(23, 24, 24));
}

// ============================================================================
// The widget
// ============================================================================

namespace {

/// An auto tray (the home tile) in a w x h box, fed @p lanes programmatic lanes.
lv_obj_t* auto_tray(lv_obj_t* screen, int32_t w, int32_t h, int lanes) {
    lv_obj_t* host = lv_obj_create(screen);
    lv_obj_set_size(host, w, h);
    lv_obj_set_scrollable(host, false);
    lv_obj_set_style_pad_all(host, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(host, 0, LV_PART_MAIN);
    lv_obj_t* tray = ui_ams_mini_status_create(host, h);
    REQUIRE(tray != nullptr);
    lv_obj_set_size(tray, lv_pct(100), lv_pct(100));
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(host);
    helix::ui::ams_mini_status_set_fill(tray, AmsMiniFill::Auto);
    ui_ams_mini_status_set_slot_count(tray, lanes);
    for (int i = 0; i < lanes; ++i)
        ui_ams_mini_status_set_slot_label(tray, i, "PLA", 50);
    ui_ams_mini_status_set_width(tray, w);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(host);
    ui_ams_mini_status_refresh(tray);
    lv_obj_update_layout(host);
    return tray;
}

bool draws_grid(lv_obj_t* tray) {
    lv_obj_t* sc = UITest::find_by_name(tray, "ams_spools_container");
    REQUIRE(sc != nullptr);
    return !lv_obj_is_hidden(sc) && lv_obj_has_state(sc, LV_STATE_USER_1);
}

bool scrolls_sideways(lv_obj_t* tray) {
    lv_obj_t* sc = UITest::find_by_name(tray, "ams_spools_container");
    REQUIRE(sc != nullptr);
    return !lv_obj_is_hidden(sc) && lv_obj_has_state(sc, LV_STATE_USER_2) &&
           lv_obj_get_scroll_right(sc) > 0;
}

int text_w(lv_obj_t* label) {
    lv_point_t size;
    lv_text_get_size(&size, lv_label_get_text(label),
                     lv_obj_get_style_text_font(label, LV_PART_MAIN), 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    return size.x;
}

/// Every lane is usable: a spool of at least kAmsMiniUsableSpoolPx, its
/// number whole inside its badge, its material whole inside its label.
void check_lanes_usable(lv_obj_t* tray, int lanes) {
    for (int i = 0; i < lanes; ++i) {
        INFO("lane " << i);
        lv_obj_t* cell = UITest::find_by_name(tray, ("spool_cell_" + std::to_string(i)).c_str());
        REQUIRE(cell != nullptr);
        lv_obj_t* spool = lv_obj_find_by_name(cell, "lane_spool");
        REQUIRE(spool != nullptr);
        // The lane spool widget is its graphic plus the badge's slack.
        CHECK(lv_obj_get_height(spool) >=
              helix::ui::kAmsMiniUsableSpoolPx + helix::ui::kAmsLaneSpoolBadgeMarginPx);
        lv_obj_t* badge = lv_obj_find_by_name(cell, ("spool_badge_" + std::to_string(i)).c_str());
        REQUIRE(badge != nullptr);
        lv_obj_t* number = lv_obj_find_by_name(badge, "lane_badge_label");
        REQUIRE(number != nullptr);
        CHECK(text_w(number) <= lv_obj_get_width(badge));
        lv_obj_t* mat = lv_obj_find_by_name(cell, ("spool_material_" + std::to_string(i)).c_str());
        REQUIRE(mat != nullptr);
        CHECK(text_w(mat) <= lv_obj_get_width(mat));
    }
}

/// Every lane's cell is laid out inside the tray or reachable by scrolling,
/// never cut off above or below it.
void check_no_lane_clipped(lv_obj_t* tray, int lanes) {
    lv_obj_t* sc = UITest::find_by_name(tray, "ams_spools_container");
    REQUIRE(sc != nullptr);
    lv_area_t box;
    lv_obj_get_coords(sc, &box);
    for (int i = 0; i < lanes; ++i) {
        lv_obj_t* cell = UITest::find_by_name(tray, ("spool_cell_" + std::to_string(i)).c_str());
        REQUIRE(cell != nullptr);
        CHECK_FALSE(lv_obj_is_hidden(cell));
        lv_area_t a;
        lv_obj_get_coords(cell, &a);
        INFO("lane " << i << " y " << a.y1 << ".." << a.y2 << " in " << box.y1 << ".." << box.y2);
        CHECK(a.y1 >= box.y1);
        CHECK(a.y2 <= box.y2);
    }
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini grid: a tall home tile grows its spools past the strip's cap",
                 "[ams_mini][grid]") {
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution medium(disp, 800, 480);
    theme_manager_refresh_layout_constants(disp);
    ui_ams_mini_status_init();

    // One four-lane unit in the 4x2 tile at 800x480.
    lv_obj_t* tray = auto_tray(test_screen(), 466, 216, 4);
    CHECK(draws_grid(tray));
    lv_obj_t* sc = UITest::find_by_name(tray, "ams_spools_container");
    CHECK(lv_obj_get_scroll_right(sc) <= 0);
    check_no_lane_clipped(tray, 4);
    // Larger spools than the strip's cap.
    lv_obj_t* spool = UITest::find_by_name(tray, "lane_spool");
    REQUIRE(spool != nullptr);
    CHECK(lv_obj_get_height(spool) > 56);
    theme_manager_refresh_layout_constants(disp);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini scroll: lanes that do not fit stay usable and scroll in every row",
                 "[ams_mini][grid]") {
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution medium(disp, 800, 480);
    theme_manager_refresh_layout_constants(disp);
    ui_ams_mini_status_init();

    // 48 lanes in the 4x2 and 2x2 tiles: two rows, scrolling.
    for (const auto& [w, h] : {std::pair{466, 216}, std::pair{228, 216}}) {
        INFO(w << "x" << h);
        lv_obj_t* tray = auto_tray(test_screen(), w, h, 48);
        CHECK(scrolls_sideways(tray));
        check_no_lane_clipped(tray, 48);
        check_lanes_usable(tray, 48);
        lv_obj_t* first = UITest::find_by_name(tray, "spool_cell_0");
        lv_obj_t* second_row = UITest::find_by_name(tray, "spool_cell_24");
        REQUIRE((first && second_row));
        CHECK(lv_obj_get_y(second_row) > lv_obj_get_y(first));
    }
    // Eight lanes in the one-row 2x1 tile: one usable row, scrolling.
    lv_obj_t* tray = auto_tray(test_screen(), 228, 99, 8);
    CHECK(scrolls_sideways(tray));
    check_lanes_usable(tray, 8);
    theme_manager_refresh_layout_constants(disp);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini scroll: a tile too short for one usable row shows the bars",
                 "[ams_mini][grid]") {
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution medium(disp, 800, 480);
    theme_manager_refresh_layout_constants(disp);
    ui_ams_mini_status_init();

    lv_obj_t* tray = auto_tray(test_screen(), 466, 50, 4);
    lv_obj_t* bars = UITest::find_by_name(tray, "ams_bars_container");
    REQUIRE(bars != nullptr);
    CHECK_FALSE(lv_obj_is_hidden(bars));
    CHECK_FALSE(draws_grid(tray));
    theme_manager_refresh_layout_constants(disp);
}

namespace {

/// The afc_direct mock's six units (Turtle 1, Tools, Vivid 1, EMU 1, AMS 1,
/// Ace2 1), synced into AmsState.
helix::AmsBackendMock* install_afc_direct() {
    auto& ams = helix::AmsState::instance();
    ams.init_subjects(false);
    auto mock = std::make_unique<helix::AmsBackendMock>(4);
    auto* raw = mock.get();
    raw->set_afc_direct_mode(true);
    ams.set_backend(std::move(mock));
    ams.sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();
    return raw;
}

void teardown_ams() {
    auto& ams = helix::AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
}

/// The tray bound to AmsState in a w x h box, in @p fill mode.
lv_obj_t* bound_tray(lv_obj_t* screen, int32_t w, int32_t h, AmsMiniFill fill) {
    lv_obj_t* host = lv_obj_create(screen);
    lv_obj_set_size(host, w, h);
    lv_obj_set_scrollable(host, false);
    lv_obj_set_style_pad_all(host, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(host, 0, LV_PART_MAIN);
    lv_obj_t* tray = ui_ams_mini_status_create(host, h);
    REQUIRE(tray != nullptr);
    lv_obj_set_size(tray, lv_pct(100), lv_pct(100));
    helix::ui::ams_mini_status_set_fill(tray, fill);
    ui_ams_mini_status_set_width(tray, w);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(host);
    ui_ams_mini_status_refresh(tray);
    lv_obj_update_layout(host);
    return tray;
}

/// The unit group titles the tray draws, in order.
std::vector<std::string> group_titles(lv_obj_t* tray) {
    std::vector<std::string> titles;
    lv_obj_t* sc = UITest::find_by_name(tray, "ams_spools_container");
    REQUIRE(sc != nullptr);
    for (uint32_t i = 0; i < lv_obj_get_child_count(sc); ++i) {
        lv_obj_t* t = lv_obj_find_by_name(lv_obj_get_child(sc, i), "unit_group_title");
        if (t && !lv_obj_is_hidden(t))
            titles.emplace_back(lv_label_get_text(t));
    }
    return titles;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini grid: the home tile titles each unit's group",
                 "[ams_mini][grid]") {
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution medium(disp, 800, 480);
    theme_manager_refresh_layout_constants(disp);
    ui_ams_mini_status_init();
    install_afc_direct();

    // The 2x2 home tile at 800x480: the units scroll in titled boxes.
    lv_obj_t* tray = bound_tray(test_screen(), 228, 216, AmsMiniFill::Auto);
    REQUIRE(scrolls_sideways(tray));
    const auto snapshot = helix::AmsState::instance().system_info();
    INFO("units " << snapshot->units.size() << ", groups "
                  << lv_obj_get_child_count(UITest::find_by_name(tray, "ams_spools_container")));
    std::vector<std::string> expected;
    for (size_t u = 0; u < snapshot->units.size(); ++u)
        expected.push_back(helix::ui::unit_display_name(snapshot->units[u], static_cast<int>(u)));
    CHECK(group_titles(tray) == expected);
    check_no_lane_clipped(tray, snapshot->total_slots);

    // Each lane sits in its own unit's group.
    for (size_t u = 0; u < snapshot->units.size(); ++u) {
        const auto& unit = snapshot->units[u];
        for (int s = 0; s < unit.slot_count; ++s) {
            const int slot = unit.first_slot_global_index + s;
            lv_obj_t* cell =
                UITest::find_by_name(tray, ("spool_cell_" + std::to_string(slot)).c_str());
            REQUIRE(cell != nullptr);
            lv_obj_t* box = lv_obj_get_parent(lv_obj_get_parent(cell));
            lv_obj_t* title = lv_obj_find_by_name(box, "unit_group_title");
            REQUIRE(title != nullptr);
            CHECK(std::string(lv_label_get_text(title)) == expected[u]);
        }
    }
    teardown_ams();
    theme_manager_refresh_layout_constants(disp);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini grid: a fill tray draws one untitled block",
                 "[ams_mini][grid]") {
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution medium(disp, 800, 480);
    theme_manager_refresh_layout_constants(disp);
    ui_ams_mini_status_init();
    install_afc_direct();

    // The box where the home tile titles its groups: a fill tray does not.
    lv_obj_t* tray = bound_tray(test_screen(), 228, 216, AmsMiniFill::On);
    REQUIRE(scrolls_sideways(tray));
    CHECK(group_titles(tray).empty());
    check_no_lane_clipped(tray, helix::AmsState::instance().system_info()->total_slots);
    teardown_ams();
    theme_manager_refresh_layout_constants(disp);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_tool_badge: an override wears the warning fill",
                 "[ams_mini][grid][tool_badge]") {
    lv_obj_t* badge =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_tool_badge", nullptr));
    REQUIRE(badge != nullptr);
    lv_obj_t* label = lv_obj_find_by_name(badge, "tool_badge_label");
    REQUIRE(label != nullptr);

    helix::ui::paint_tool_badge(badge, 3, false);
    CHECK(std::string(lv_label_get_text(label)) == "T3");
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(badge, LV_PART_MAIN),
                      theme_manager_get_color("text_muted")));

    helix::ui::paint_tool_badge(badge, 3, true);
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(badge, LV_PART_MAIN),
                      theme_manager_get_color("warning")));
    CHECK(lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN),
                      helix::ui::tool_badge_colors(true).text));
}
