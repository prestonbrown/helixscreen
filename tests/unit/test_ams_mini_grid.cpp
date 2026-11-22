// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_mini_grid.cpp
 * @brief The spool view's grid: how plan_ams_mini_grid() packs units into a
 * box, when the widget draws the grid instead of the one-row strip, and that a
 * lane count no grid can hold falls back to the strip, which scrolls.
 */

#include "ui_ams_mini_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "display_numbering.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"

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

TEST_CASE("ams_mini grid: when the widget draws the grid", "[ams_mini][grid]") {
    using helix::ui::ams_mini_use_grid;
    // A row tray never draws it.
    CHECK_FALSE(ams_mini_use_grid(false, false, 80, 24, 24));
    // A fill tray draws it whenever its spool reads.
    CHECK(ams_mini_use_grid(true, false, 24, 56, 24));
    CHECK_FALSE(ams_mini_use_grid(true, false, 23, 24, 24));
    // An auto tray only for a larger spool than the strip's.
    CHECK(ams_mini_use_grid(false, true, 31, 24, 24));
    CHECK_FALSE(ams_mini_use_grid(false, true, 24, 24, 24));
    CHECK_FALSE(ams_mini_use_grid(false, true, 22, 21, 24));
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
    lv_obj_update_flag(host, LV_OBJ_FLAG_SCROLLABLE, false);
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
    return lv_obj_has_state(sc, LV_STATE_USER_1);
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
        CHECK_FALSE(lv_obj_has_flag(cell, LV_OBJ_FLAG_HIDDEN));
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
                 "ams_mini grid: more lanes than any grid holds fall back to the scrolling strip",
                 "[ams_mini][grid]") {
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution medium(disp, 800, 480);
    theme_manager_refresh_layout_constants(disp);
    ui_ams_mini_status_init();

    // 48 lanes (twelve four-lane units) in the 4x2 tile, and in a 2x2 one.
    for (const auto& [w, h] : {std::pair{466, 216}, std::pair{228, 216}}) {
        INFO(w << "x" << h);
        lv_obj_t* tray = auto_tray(test_screen(), w, h, 48);
        CHECK_FALSE(draws_grid(tray));
        lv_obj_t* sc = UITest::find_by_name(tray, "ams_spools_container");
        CHECK(lv_obj_get_scroll_right(sc) > 0);
        check_no_lane_clipped(tray, 48);
    }
    theme_manager_refresh_layout_constants(disp);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini grid: a short home tile keeps the strip",
                 "[ams_mini][grid]") {
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution medium(disp, 800, 480);
    theme_manager_refresh_layout_constants(disp);
    ui_ams_mini_status_init();

    lv_obj_t* tray = auto_tray(test_screen(), 466, 99, 4);
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
    lv_obj_update_flag(host, LV_OBJ_FLAG_SCROLLABLE, false);
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
        if (lv_obj_t* t = lv_obj_find_by_name(lv_obj_get_child(sc, i), "unit_group_title"))
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

    // The 4x2 home tile at 800x480.
    lv_obj_t* tray = bound_tray(test_screen(), 466, 216, AmsMiniFill::Auto);
    REQUIRE(draws_grid(tray));
    const auto snapshot = helix::AmsState::instance().system_info();
    std::vector<std::string> expected;
    for (size_t u = 0; u < snapshot->units.size(); ++u)
        expected.push_back(
            ams_draw::get_unit_display_name(snapshot->units[u], static_cast<int>(u)));
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
    lv_obj_t* tray = bound_tray(test_screen(), 466, 216, AmsMiniFill::On);
    REQUIRE(draws_grid(tray));
    CHECK(group_titles(tray).empty());
    check_no_lane_clipped(tray, helix::AmsState::instance().system_info()->total_slots);
    teardown_ams();
    theme_manager_refresh_layout_constants(disp);
}
