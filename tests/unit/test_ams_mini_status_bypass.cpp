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
#include "ui_bypass_spool_widget.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/mock_bypass.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_mini_test_helpers.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "settings_manager.h"
#include "system_settings_manager.h"
#include "test_helpers/scoped_language.h"

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
    if (ams.get_external_spool_info().has_value()) {
        ams.clear_external_spool_info();
        for (int i = 0; i < 3; ++i)
            helix::ui::UpdateQueue::instance().drain();
    }
    ams.set_moonraker_api(nullptr);
    ams.clear_backends();
    ams.deinit_subjects();
}

void settle() {
    for (int i = 0; i < 3; ++i)
        helix::ui::UpdateQueue::instance().drain();
}

/// A strip in a box @p width px wide, so layout measures what a grid cell gives it.
lv_obj_t* make_strip(lv_obj_t* screen, int width, int height = 100) {
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

/// A PLA spool put on the bypass through a mock Moonraker, as assigning one
/// from the AMS panel does. teardown_ams() clears it and detaches the mock.
struct BypassSpool {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;
    BypassSpool() : api(client, get_printer_state()) {
        AmsState::instance().set_moonraker_api(&api);
        helix::SlotInfo spool;
        spool.color_rgb = 0x2080FF;
        spool.material = "PLA";
        AmsState::instance().set_external_spool_info(spool);
        settle();
        REQUIRE(AmsState::instance().get_external_spool_info().has_value());
    }
};

/// Engage the bypass: empty the toolhead, then switch over.
void engage_bypass(AmsBackendMock* raw) {
    REQUIRE(helix::test::unload_for_bypass(*raw));
    REQUIRE(raw->enable_bypass().success());
    AmsState::instance().sync_from_backend();
    settle();
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
    lv_obj_t* bar = ams_mini_test::lane_bar_for_slot(w, lane);
    REQUIRE(bar != nullptr);
    return bar;
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

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "ams_mini bypass: an unused bypass with no spool stays off the strip in both views",
    "[ams][mini_status][mini-bypass]") {
    AlwaysShowSetting setting(true);
    install_mock(std::make_unique<AmsBackendMock>(4));
    REQUIRE(helix::ui::bypass_node_visible_for(AmsState::instance().get_backend()));
    REQUIRE_FALSE(AmsState::instance().get_external_spool_info().has_value());

    lv_obj_t* bars = make_strip(test_screen(), 130);
    REQUIRE(shown(lane_bar(bars, 0)));
    CHECK_FALSE(shown(bypass_column(bars)));
    lv_obj_t* spools = make_strip(test_screen(), 400);
    CHECK_FALSE(shown(UITest::find_by_name(spools, "bypass_cell")));

    // A spool on it gives the strip something to say.
    BypassSpool bypass_spool;
    lv_obj_update_layout(test_screen());
    CHECK(shown(bypass_column(bars)));
    CHECK(shown(UITest::find_by_name(spools, "bypass_cell")));

    // Taking the spool off leaves it unused again.
    AmsState::instance().clear_external_spool_info();
    settle();
    lv_obj_update_layout(test_screen());
    REQUIRE_FALSE(AmsState::instance().get_external_spool_info().has_value());
    CHECK_FALSE(shown(bypass_column(bars)));
    CHECK_FALSE(shown(UITest::find_by_name(spools, "bypass_cell")));

    lv_obj_delete(lv_obj_get_parent(bars));
    lv_obj_delete(lv_obj_get_parent(spools));
    teardown_ams();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini bypass: a physical bypass stands past a divider, lane-sized, unclipped",
                 "[ams][mini_status][mini-bypass]") {
    AlwaysShowSetting setting(false);
    install_mock(std::make_unique<AmsBackendMock>(8));
    BypassSpool bypass_spool;

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
                 "ams_mini bypass: a virtual bypass with a spool follows the always-show setting",
                 "[ams][mini_status][mini-bypass]") {
    AlwaysShowSetting setting(false);
    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_afc_mode(true);
    install_mock(std::move(mock));
    BypassSpool bypass_spool;

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

    engage_bypass(raw);

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
    BypassSpool bypass_spool;
    REQUIRE(AmsState::instance().system_info()->units.size() >= 2);

    lv_obj_t* w = make_strip(test_screen(), 80, 180);
    lv_obj_t* col = bypass_column(w);
    REQUIRE(shown(col));
    // Not in any unit's line: a sibling of the lines' container.
    CHECK(lv_obj_get_parent(col) == w);

    // As tall as the lines together: from the first lane's bar top to the last
    // lane's bar bottom, on a strip whose units take two lines.
    const int last_lane = AmsState::instance().system_info()->total_slots - 1;
    lv_area_t first_bar, last_bar, bypass;
    lv_obj_get_coords(bar_bg(lane_bar(w, 0)), &first_bar);
    lv_obj_get_coords(bar_bg(lane_bar(w, last_lane)), &last_bar);
    REQUIRE(last_bar.y1 > first_bar.y2);
    // The units sit in boxes, whose padding and border each line adds.
    lv_obj_t* box = lv_obj_get_parent(lv_obj_get_parent(lane_bar(w, 0)));
    REQUIRE(lv_obj_get_style_border_width(box, LV_PART_MAIN) > 0);
    lv_obj_get_coords(bar_bg(lv_obj_find_by_name(col, "bypass_bar")), &bypass);
    CHECK(bypass.y1 == first_bar.y1);
    CHECK(bypass.y2 == last_bar.y2);

    lv_area_t cell;
    lv_obj_get_coords(lv_obj_get_parent(w), &cell);
    CHECK(bypass.x2 <= cell.x2);

    lv_obj_delete(lv_obj_get_parent(w));
    teardown_ams();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini bypass: the spool view ends with a bypass cell naming its spool",
                 "[ams][mini_status][mini-bypass]") {
    AlwaysShowSetting setting(false);
    install_mock(std::make_unique<AmsBackendMock>(4));
    BypassSpool bypass_spool;

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
    CHECK(std::string(lv_label_get_text(material)) == "PLA");

    lv_obj_delete(lv_obj_get_parent(w));
    teardown_ams();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini bypass: an engaged bypass with no spool reads External, in any language",
                 "[ams][mini_status][mini-bypass][i18n]") {
    AlwaysShowSetting setting(false);
    auto* raw = install_mock(std::make_unique<AmsBackendMock>(4));
    REQUIRE_FALSE(AmsState::instance().get_external_spool_info().has_value());

    lv_obj_t* w = make_strip(test_screen(), 400);
    engage_bypass(raw);
    lv_obj_t* material = UITest::find_by_name(w, "bypass_material");
    REQUIRE(material != nullptr);
    REQUIRE(std::string(lv_label_get_text(material)) == "External");

    lv_subject_t* language = helix::SystemSettingsManager::instance().subject_language();
    const int32_t saved = lv_subject_get_int(language);
    {
        helix::ScopedLanguage ru("ru");
        REQUIRE(std::string(lv_tr("External")) != "External");
        lv_subject_set_int(language, helix::SystemSettingsManager::language_code_to_index("ru"));
        settle();
        CHECK(std::string(lv_label_get_text(material)) == lv_tr("External"));
    }
    lv_subject_set_int(language, saved);
    settle();

    lv_obj_delete(lv_obj_get_parent(w));
    teardown_ams();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini bypass: the bypass appearing shrinks the lanes' spools into their cells",
                 "[ams][mini_status][mini-bypass]") {
    AlwaysShowSetting setting(false);
    install_mock(std::make_unique<AmsBackendMock>(4));

    lv_obj_t* w = make_strip(test_screen(), 230, 100);
    lv_obj_t* cell = UITest::find_by_name(w, "spool_cell_0");
    REQUIRE(cell != nullptr);
    lv_obj_t* graphic = UITest::find_by_name(cell, "spool_graphic");
    REQUIRE(graphic != nullptr);
    const int32_t before = lv_obj_get_width(graphic);

    REQUIRE_FALSE(shown(UITest::find_by_name(w, "bypass_cell")));
    BypassSpool bypass_spool;
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
    BypassSpool bypass_spool;

    // Six lanes in a short two-column home cell: more than fit, so the lanes
    // scroll, and the row is short enough that a spool reaches its bottom edge.
    lv_obj_t* w = make_strip(test_screen(), 230, 100);
    // Fill the cell's height, as the home grid's XML host does.
    lv_obj_set_height(w, lv_pct(100));
    lv_obj_update_layout(test_screen());
    ui_ams_mini_status_refresh(w);
    settle();
    lv_obj_update_layout(test_screen());
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

    // The scrollbar runs along the scroller's bottom: every lane's spool and badge
    // stand above it.
    const int32_t bar_top = lanes.y2 + 1 - lv_obj_get_style_width(spools, LV_PART_SCROLLBAR) -
                            lv_obj_get_style_pad_bottom(spools, LV_PART_SCROLLBAR);
    REQUIRE(lv_obj_get_height(spools) > 40);
    for (int lane = 0; lane < 6; ++lane) {
        INFO("lane " << lane);
        lv_obj_t* lane_cell =
            UITest::find_by_name(spools, ("spool_cell_" + std::to_string(lane)).c_str());
        REQUIRE(lane_cell != nullptr);
        lv_obj_t* badge =
            UITest::find_by_name(lane_cell, ("spool_badge_" + std::to_string(lane)).c_str());
        lv_obj_t* graphic = UITest::find_by_name(lane_cell, "spool_graphic");
        REQUIRE(badge != nullptr);
        REQUIRE(graphic != nullptr);
        lv_area_t a;
        lv_obj_get_coords(badge, &a);
        CHECK(a.y2 < bar_top);
        lv_obj_get_coords(graphic, &a);
        CHECK(a.y2 < bar_top);
    }

    lv_obj_delete(lv_obj_get_parent(w));
    teardown_ams();
}
