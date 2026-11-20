// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_mini_status_bypass.cpp
 * @brief The mini-status strip draws the bypass past a divider, by the AMS panel's rule
 *
 * The bypass is on the strip exactly when bypass_node_visible_for() puts it on the
 * AMS panel's path, it follows the always-show setting on a virtual bypass, it takes
 * the loaded-lane outline while engaged, and on a stacked multi-unit strip it stands
 * beside every row rather than in one unit's.
 */

#include "ui_ams_mini_status.h"

#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "settings_manager.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::AmsBackendMock;
using helix::AmsState;

namespace {

/// Holds the always-show setting at @p value for the test, restoring it after.
struct AlwaysShowSetting {
    bool saved;
    explicit AlwaysShowSetting(bool value) {
        auto& settings = helix::SettingsManager::instance();
        settings.init_subjects();
        saved = settings.get_ams_always_show_bypass_spool();
        settings.set_ams_always_show_bypass_spool(value);
    }
    ~AlwaysShowSetting() {
        helix::SettingsManager::instance().set_ams_always_show_bypass_spool(saved);
    }
};

AmsBackendMock* install_mock(std::unique_ptr<AmsBackendMock> mock) {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    REQUIRE(mock->start().success());
    auto* raw = mock.get();
    ams.set_backend(std::move(mock));
    ams.sync_from_backend();
    return raw;
}

void teardown_ams() {
    auto& ams = AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
}

void settle() {
    for (int i = 0; i < 3; ++i)
        helix::ui::UpdateQueue::instance().drain();
}

/// A strip in a box @p width px wide, so layout measures what a grid cell gives it.
lv_obj_t* make_strip(lv_obj_t* screen, int width, int height = 60) {
    lv_obj_t* box = lv_obj_create(screen);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(box, height);
    REQUIRE(w != nullptr);
    ui_ams_mini_status_set_width(w, width);
    // Measure against the laid-out cell, as a host's first repaint does.
    lv_obj_update_layout(screen);
    ui_ams_mini_status_refresh(w);
    settle();
    lv_obj_update_layout(screen);
    return w;
}

bool shown(lv_obj_t* obj) {
    return obj != nullptr && !lv_obj_is_hidden(obj);
}

lv_obj_t* bypass_column(lv_obj_t* w) {
    return UITest::find_by_name(w, "ams_bypass_column");
}

lv_obj_t* bar_bg(lv_obj_t* bar) {
    REQUIRE(bar != nullptr);
    lv_obj_t* bg = lv_obj_find_by_name(bar, "bar_bg");
    REQUIRE(bg != nullptr);
    return bg;
}

lv_obj_t* lane_bar(lv_obj_t* w, int lane) {
    lv_obj_t* bars = UITest::find_by_name(w, "ams_bars_container");
    REQUIRE(bars != nullptr);
    REQUIRE(static_cast<int>(lv_obj_get_child_count(bars)) > lane);
    return lv_obj_get_child(bars, lane);
}

/// The lane bar family's loaded outline is 2px.
bool wears_loaded_outline(lv_obj_t* bar) {
    return lv_obj_get_style_border_width(bar_bg(bar), LV_PART_MAIN) == 2;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini bypass: a disengaged virtual bypass stays off the strip",
                 "[ams][mini_status][mini-bypass]") {
    AlwaysShowSetting setting(false);
    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_afc_mode(true);
    install_mock(std::move(mock));

    lv_obj_t* w = make_strip(test_screen(), 130);
    REQUIRE(shown(lane_bar(w, 0)));
    CHECK_FALSE(shown(bypass_column(w)));

    lv_obj_delete(lv_obj_get_parent(w));
    teardown_ams();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini bypass: a physical bypass stands past a divider, lane-sized, unclipped",
                 "[ams][mini_status][mini-bypass]") {
    AlwaysShowSetting setting(false);
    install_mock(std::make_unique<AmsBackendMock>(8));

    lv_obj_t* w = make_strip(test_screen(), 130);
    lv_obj_t* col = bypass_column(w);
    REQUIRE(shown(col));
    lv_obj_t* divider = lv_obj_find_by_name(col, "bypass_divider");
    REQUIRE(shown(divider));
    CHECK(lv_obj_get_width(divider) == 1);
    lv_obj_t* bar = lv_obj_find_by_name(col, "bypass_bar");
    REQUIRE(shown(bar));

    // Eight lanes narrow every bar below its cap, so the bypass and its divider
    // must come out of the lanes' share. Same width as a lane, and inside the cell.
    CHECK(lv_obj_get_width(bar_bg(bar)) == lv_obj_get_width(bar_bg(lane_bar(w, 7))));
    lv_area_t cell, b, first_lane, last_lane;
    lv_obj_get_coords(lv_obj_get_parent(w), &cell);
    lv_obj_get_coords(bar, &b);
    lv_obj_get_coords(lane_bar(w, 0), &first_lane);
    lv_obj_get_coords(lane_bar(w, 7), &last_lane);
    CHECK(first_lane.x1 >= cell.x1);
    CHECK(b.x2 <= cell.x2);
    CHECK(b.x1 > last_lane.x2);

    lv_obj_delete(lv_obj_get_parent(w));
    teardown_ams();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini bypass: a virtual bypass follows the always-show setting",
                 "[ams][mini_status][mini-bypass]") {
    AlwaysShowSetting setting(false);
    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_afc_mode(true);
    install_mock(std::move(mock));

    lv_obj_t* w = make_strip(test_screen(), 130);
    REQUIRE_FALSE(shown(bypass_column(w)));

    helix::SettingsManager::instance().set_ams_always_show_bypass_spool(true);
    settle();
    CHECK(shown(bypass_column(w)));

    helix::SettingsManager::instance().set_ams_always_show_bypass_spool(false);
    settle();
    CHECK_FALSE(shown(bypass_column(w)));

    lv_obj_delete(lv_obj_get_parent(w));
    teardown_ams();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini bypass: engaged bypass takes the loaded outline from the lanes",
                 "[ams][mini_status][mini-bypass]") {
    AlwaysShowSetting setting(false);
    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_afc_mode(true);
    auto* raw = install_mock(std::move(mock));

    lv_obj_t* w = make_strip(test_screen(), 130);
    REQUIRE_FALSE(shown(bypass_column(w)));

    raw->set_operation_delay(10);
    if (raw->is_filament_loaded()) {
        REQUIRE(raw->unload_filament(raw->get_current_slot()).success());
        REQUIRE(wait_until([&] { return !raw->is_filament_loaded(); }, 5000));
        settle();
    }
    REQUIRE(raw->enable_bypass().success());
    AmsState::instance().sync_from_backend();
    settle();

    // Engaged, a virtual bypass is on the strip whatever the setting says.
    lv_obj_t* col = bypass_column(w);
    REQUIRE(shown(col));
    CHECK(wears_loaded_outline(lv_obj_find_by_name(col, "bypass_bar")));
    for (int lane = 0; lane < 4; ++lane)
        CHECK_FALSE(wears_loaded_outline(lane_bar(w, lane)));

    lv_obj_delete(lv_obj_get_parent(w));
    teardown_ams();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini bypass: stacked units share one bypass beside every row",
                 "[ams][mini_status][mini-bypass]") {
    AlwaysShowSetting setting(true);
    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_multi_unit_mode(true);
    install_mock(std::move(mock));
    REQUIRE(AmsState::instance().system_info()->units.size() >= 2);

    lv_obj_t* w = make_strip(test_screen(), 130, 80);
    lv_obj_t* col = bypass_column(w);
    REQUIRE(shown(col));
    // Not in any unit's row: a sibling of the rows' container.
    CHECK(lv_obj_get_parent(col) == w);

    // As tall as the rows together: from the top row's top to the bottom row's bottom.
    lv_obj_t* bars = UITest::find_by_name(w, "ams_bars_container");
    REQUIRE(lv_obj_get_child_count(bars) >= 2);
    lv_area_t first_row, last_row, bypass;
    lv_obj_get_coords(lv_obj_get_child(bars, 0), &first_row);
    lv_obj_get_coords(lv_obj_get_child(bars, lv_obj_get_child_count(bars) - 1), &last_row);
    lv_obj_get_coords(bar_bg(lv_obj_find_by_name(col, "bypass_bar")), &bypass);
    CHECK(bypass.y1 == first_row.y1);
    CHECK(bypass.y2 == last_row.y2);

    lv_area_t cell;
    lv_obj_get_coords(lv_obj_get_parent(w), &cell);
    CHECK(bypass.x2 <= cell.x2);

    lv_obj_delete(lv_obj_get_parent(w));
    teardown_ams();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini bypass: the spool view ends with a bypass cell, External with no spool",
                 "[ams][mini_status][mini-bypass]") {
    AlwaysShowSetting setting(false);
    install_mock(std::make_unique<AmsBackendMock>(4));

    lv_obj_t* w = make_strip(test_screen(), 400);
    lv_obj_t* cell = UITest::find_by_name(w, "bypass_cell");
    REQUIRE(shown(cell));
    REQUIRE(shown(UITest::find_by_name(w, "bypass_divider")));
    // Past the lanes' scroller, not in it.
    lv_obj_t* spools = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(spools != nullptr);
    CHECK(lv_obj_get_parent(cell) == lv_obj_get_parent(spools));
    CHECK(lv_obj_get_index(cell) > lv_obj_get_index(spools));

    lv_obj_t* material = lv_obj_find_by_name(cell, "bypass_material");
    REQUIRE(material != nullptr);
    REQUIRE_FALSE(AmsState::instance().get_external_spool_info().has_value());
    CHECK(std::string(lv_label_get_text(material)) == lv_tr("External"));

    lv_obj_delete(lv_obj_get_parent(w));
    teardown_ams();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini bypass: the bypass appearing shrinks the lanes' spools into their cells",
                 "[ams][mini_status][mini-bypass]") {
    AlwaysShowSetting setting(false);
    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_afc_mode(true);
    install_mock(std::move(mock));

    lv_obj_t* w = make_strip(test_screen(), 230, 100);
    lv_obj_t* cell = UITest::find_by_name(w, "spool_cell_0");
    REQUIRE(cell != nullptr);
    lv_obj_t* graphic = UITest::find_by_name(cell, "spool_graphic");
    REQUIRE(graphic != nullptr);
    const int32_t before = lv_obj_get_width(graphic);

    helix::SettingsManager::instance().set_ams_always_show_bypass_spool(true);
    settle();
    lv_obj_update_layout(test_screen());
    REQUIRE(shown(UITest::find_by_name(w, "bypass_cell")));

    // The spool is rebuilt at the narrower cell's size, not left at the size it was made.
    graphic = UITest::find_by_name(cell, "spool_graphic");
    REQUIRE(graphic != nullptr);
    CHECK(lv_obj_get_width(graphic) < before);
    lv_area_t c, g;
    lv_obj_get_coords(cell, &c);
    lv_obj_get_coords(graphic, &g);
    CHECK(g.x2 <= c.x2);

    lv_obj_delete(lv_obj_get_parent(w));
    teardown_ams();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini bypass: lanes that overflow scroll while the bypass cell stays in view",
                 "[ams][mini_status][mini-bypass]") {
    AlwaysShowSetting setting(true);
    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_multi_unit_mode(true);
    install_mock(std::move(mock));

    // Six lanes in a two-column home cell: more than fit, so the lanes scroll.
    lv_obj_t* w = make_strip(test_screen(), 230, 100);
    lv_obj_t* spools = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(shown(spools));
    REQUIRE(lv_obj_get_scroll_right(spools) > 0);

    lv_obj_t* cell = UITest::find_by_name(w, "bypass_cell");
    REQUIRE(shown(cell));
    lv_area_t box, c, lanes;
    lv_obj_get_coords(lv_obj_get_parent(w), &box);
    lv_obj_get_coords(cell, &c);
    lv_obj_get_coords(spools, &lanes);
    CHECK(c.x1 >= box.x1);
    CHECK(c.x2 <= box.x2);
    CHECK(c.x1 > lanes.x2);

    lv_obj_delete(lv_obj_get_parent(w));
    teardown_ams();
}
