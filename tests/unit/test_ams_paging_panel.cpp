// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_paging_panel.cpp
 * @brief The overview's unit view: one unit per page, stepped, swiped and kept on its unit.
 *
 * Drives the real AmsOverviewPanel against a twelve-unit fleet shaped like the OpenAMS
 * mock: units 0-9 on hub "fps", units 10-11 on hub "fps2". Unit 2 holds the loaded slot
 * and unit 6 is drying. The panel opens on its overview; tapping a unit card zooms into the
 * unit view, open on that unit's page.
 */

#include "ui_ams_slot.h"
#include "ui_bypass_spool_widget.h"
#include "ui_filament_path_canvas.h"
#include "ui_nav_manager.h"
#include "ui_panel_ams.h"
#include "ui_panel_ams_overview.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_panel_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "display_settings_manager.h"
#include "src/ui/ui_filament_path_internal.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <array>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::fpath::get_data;

namespace {

constexpr int kSlotsPerUnit = 4;
constexpr int kOwnNumberBase = 100;

struct UnitSpec {
    std::string name;
    std::string hub;
    bool absent = false;
    int slots = kSlotsPerUnit;
    bool connected = true;
    std::string extruder{}; ///< the Klipper extruder the unit's lanes feed; "" names none
};

int subject_value(lv_subject_t* subject) {
    return lv_subject_get_int(subject);
}

class PagedFleet : public AmsBackendMock {
  public:
    PagedFleet() : AmsBackendMock(48) {
        for (int u = 0; u < 12; ++u)
            specs.push_back({"unit_" + std::to_string(u), u < 10 ? "fps" : "fps2", false});
    }

    // Every unit feeds a hub, as an OpenAMS fleet's do.
    [[nodiscard]] PathTopology get_topology() const override {
        return PathTopology::HUB;
    }
    [[nodiscard]] PathTopology get_unit_topology(int) const override {
        return PathTopology::HUB;
    }

    AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        info.units.clear();
        int first_slot = 0;
        for (size_t pos = 0; pos < specs.size(); ++pos) {
            AmsUnit unit;
            // The unit's own number is not its position, as on a backend that numbers its
            // units from elsewhere: the paging screen must key the viewed unit and the
            // env chip on this one.
            unit.unit_index = kOwnNumberBase + static_cast<int>(pos);
            unit.name = specs[pos].name;
            unit.display_name = "Box " + specs[pos].name;
            unit.hub_id = specs[pos].hub;
            unit.absent = specs[pos].absent;
            unit.connected = specs[pos].connected;
            unit.slot_count = specs[pos].slots;
            unit.first_slot_global_index = first_slot;
            first_slot += specs[pos].slots;
            EnvironmentData env;
            env.temperature_c = 20.0f + static_cast<float>(pos);
            env.humidity_pct = 30.0f + static_cast<float>(pos);
            env.has_humidity = true;
            unit.environment = env;
            for (int s = 0; s < specs[pos].slots; ++s) {
                SlotInfo slot;
                slot.slot_index = s;
                slot.global_index = unit.first_slot_global_index + s;
                slot.extruder_name = specs[pos].extruder;
                unit.slots.push_back(slot);
            }
            info.units.push_back(unit);
        }
        info.total_slots = first_slot;
        info.current_slot = loaded_slot;
        info.filament_loaded = loaded_slot >= 0;
        return info;
    }

    int get_current_slot() const override {
        return loaded_slot;
    }

    DryerInfo get_dryer_info(int unit = 0) const override {
        DryerInfo d;
        d.supported = true;
        d.active = (unit == drying_unit);
        d.remaining_min = 90;
        return d;
    }

    std::vector<UnitSpec> specs;
    int loaded_slot = 2 * kSlotsPerUnit + 1; ///< a bay of unit 2
    int drying_unit = kOwnNumberBase + 6;    ///< by the unit's own number
};

class PagingFixture : public LVGLUITestFixture {
  public:
    PagingFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);
        helix::ui::destroy_static_panels();

        lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = lv_obj_create(test_screen());
        panels[static_cast<int>(PanelId::Controls)] = lv_obj_create(test_screen());
        NavigationManager::instance().set_panels(panels);
        UITest::init(test_screen());
        install();
    }

    ~PagingFixture() override {
        UITest::cleanup();
        helix::ui::destroy_static_panels();
        drain();
        AmsState::instance().set_backend(nullptr);
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    /// A fresh fleet; @p tweak edits it before the first sync.
    template <typename F> PagedFleet* install(F&& tweak) {
        auto mock = std::make_unique<PagedFleet>();
        tweak(*mock);
        REQUIRE(mock->start().success());
        auto* raw = mock.get();
        AmsState::instance().set_backend(std::move(mock));
        AmsState::instance().init_subjects(true);
        AmsState::instance().sync_from_backend();
        return raw;
    }
    PagedFleet* install() {
        return install([](PagedFleet&) {});
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Open the panel: the overview.
    void open() {
        navigate_to_ams_panel();
        drain();
        process_lvgl(80);
    }

    /// Tap the unit card of the unit at @p unit_pos in AmsSystemInfo::units.
    void enter(int unit_pos) {
        const auto cards = AmsPanelTestAccess::unit_cards(panel());
        const auto units = AmsPanelTestAccess::unit_card_units(panel());
        for (size_t i = 0; i < cards.size(); ++i) {
            if (units[i] == unit_pos) {
                tap(cards[i]);
                return;
            }
        }
        FAIL("no unit card for unit " << unit_pos);
    }

    /// Open the panel and zoom into the unit at @p unit_pos.
    void open_at(int unit_pos) {
        open();
        REQUIRE_FALSE(AmsPanelTestAccess::in_unit_view(panel()));
        enter(unit_pos);
        REQUIRE(AmsPanelTestAccess::in_unit_view(panel()));
    }

    void close() {
        NavigationManager::instance().go_back();
        drain();
        process_lvgl(50);
    }

    AmsOverviewPanel& panel() {
        auto* p = helix::lazy_global_if_exists<AmsOverviewPanel>();
        REQUIRE(p != nullptr);
        return *p;
    }

    lv_obj_t* root() {
        return panel().get_panel();
    }

    lv_obj_t* named(const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(root(), name);
        REQUIRE(obj != nullptr);
        return obj;
    }

    static int subject(lv_subject_t* s) {
        return lv_subject_get_int(s);
    }
    static int page_count() {
        return subject(AmsState::instance().get_ams_page_count_subject());
    }
    static int page_current() {
        return subject(AmsState::instance().get_ams_page_current_subject());
    }
    static std::string unit_name() {
        return lv_subject_get_string(AmsState::instance().get_ams_page_unit_name_subject());
    }

    /// The unit view's env chip; the overview's cards carry chips of the same name.
    lv_obj_t* view_chip() {
        lv_obj_t* chip = lv_obj_find_by_name(named("unit_detail_container"), "env_indicator");
        REQUIRE(chip != nullptr);
        return chip;
    }

    bool hidden(const char* name) {
        return lv_obj_has_flag(named(name), LV_OBJ_FLAG_HIDDEN);
    }

    void tap(lv_obj_t* obj) {
        lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
        drain();
        process_lvgl(30);
    }

    /// Step to @p page by the next/previous arrows.
    void go_to(int page) {
        for (int guard = 0; page_current() < page && guard < 200; ++guard)
            tap(named("page_next_button"));
        for (int guard = 0; page_current() > page && guard < 200; ++guard)
            tap(named("page_prev_button"));
        REQUIRE(page_current() == page);
    }

    /// A point inside the left column that is neither an arrow nor a spool.
    lv_point_t empty_spot() {
        lv_area_t c;
        lv_obj_get_coords(named("detail_path_container"), &c);
        return {(c.x1 + c.x2) / 2 + 20, (c.y1 + c.y2) / 2 + 40};
    }

    /// One swipe: down at @p from, one jump of @p dx pixels, up.
    void swipe(lv_point_t from, int32_t dx) {
        REQUIRE(UITest::press_at(from.x, from.y));
        REQUIRE(UITest::move_to(from.x + dx, from.y));
        REQUIRE(UITest::release());
        drain();
        process_lvgl(50);
    }

    /// Whether a slot context menu is on screen.
    bool slot_menu_open() {
        lv_obj_t* backdrop = lv_obj_find_by_name(test_screen(), "context_backdrop");
        return backdrop != nullptr && !lv_obj_has_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    }

    /// The canvas's hub box.
    lv_area_t hub_box() {
        lv_area_t box;
        REQUIRE(ui_filament_path_canvas_get_hub_box(named("detail_path_canvas"), &box));
        return box;
    }

    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(PagingFixture, "Unit view: the panel opens on the overview",
                 "[ams][pages][panel][overview]") {
    open();
    REQUIRE(helix::nav::is_showing(root()));

    CHECK_FALSE(AmsPanelTestAccess::in_unit_view(panel()));
    CHECK(subject(AmsState::instance().get_ams_unit_view_active_subject()) == 0);
    CHECK(page_count() == 0);
    CHECK(AmsPanelTestAccess::unit_cards(panel()).size() == 12);
    CHECK_FALSE(lv_obj_has_flag(named("unit_cards_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(named("system_path_area"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(named("unit_detail_container"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(named("header_title"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(named("header_backend_suffix"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(named("header_title_unit_view"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(PagingFixture, "Unit view: tapping a card opens that unit's page",
                 "[ams][pages][panel][overview]") {
    // The cards run in nozzle order, so units feeding one extruder sit together there, and the
    // pages keep the backend's order when no unit names a hub: the two lists disagree.
    install([](PagedFleet& f) {
        f.specs = {{"unit_0", "", false, kSlotsPerUnit, true, "e0"},
                   {"unit_1", "", false, kSlotsPerUnit, true, "e1"},
                   {"unit_2", "", false, kSlotsPerUnit, true, "e0"},
                   {"unit_3", "", false, kSlotsPerUnit, true, "e1"}};
        f.loaded_slot = -1;
    });
    open();
    const auto card_units = AmsPanelTestAccess::unit_card_units(panel());
    REQUIRE(card_units.size() == 4);

    // The page each unit has once the view is open.
    std::vector<int> page_of(4, -1);
    for (int unit = 0; unit < 4; ++unit) {
        enter(unit);
        REQUIRE(AmsPanelTestAccess::in_unit_view(panel()));
        page_of[unit] = page_current();
        tap(named("back_button"));
        REQUIRE_FALSE(AmsPanelTestAccess::in_unit_view(panel()));
    }
    // At least one unit sits on a card position different from its page.
    bool differs = false;
    for (size_t card = 0; card < card_units.size(); ++card)
        differs = differs || page_of[card_units[card]] != static_cast<int>(card);
    REQUIRE(differs);

    // Each card opens its own unit, whichever card position and page that is.
    const auto cards = AmsPanelTestAccess::unit_cards(panel());
    for (size_t card = 0; card < cards.size(); ++card) {
        CAPTURE(card);
        const int unit = card_units[card];
        tap(cards[card]);
        REQUIRE(AmsPanelTestAccess::in_unit_view(panel()));
        CHECK(page_current() == page_of[unit]);
        CHECK(AmsPanelTestAccess::shown_unit_position(panel()) == unit);
        CHECK(unit_name() == "Box unit " + std::to_string(unit));
        tap(named("back_button"));
    }
}

TEST_CASE_METHOD(PagingFixture, "Unit view: a card of the fleet opens its own page",
                 "[ams][pages][panel][overview]") {
    open_at(2);
    CHECK(page_count() == 12);
    CHECK(page_current() == 2);
    CHECK(unit_name() == "Box unit 2");
    CHECK(AmsPanelTestAccess::shown_unit_position(panel()) == 2);
    CHECK(subject(AmsState::instance().get_ams_unit_view_active_subject()) == 1);

    // The overview's elements give way to the unit view, and the header names the unit.
    CHECK(lv_obj_has_flag(named("unit_cards_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(named("system_path_area"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(named("unit_detail_container"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(named("header_title"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(named("header_backend_suffix"), LV_OBJ_FLAG_HIDDEN));

    SECTION("another card opens its page") {
        tap(named("back_button"));
        enter(10);
        CHECK(page_current() == 10);
        CHECK(unit_name() == "Box unit 10");
    }
}

TEST_CASE_METHOD(PagingFixture, "Unit view: the arrows step one page and stop at the ends",
                 "[ams][pages][panel]") {
    open_at(2);
    REQUIRE(page_current() == 2);

    tap(named("page_next_button"));
    CHECK(page_current() == 3);
    CHECK(unit_name() == "Box unit 3");
    tap(named("page_next_button"));
    CHECK(page_current() == 4);
    tap(named("page_prev_button"));
    CHECK(page_current() == 3);

    SECTION("the first page has no previous arrow, and the last no next arrow") {
        go_to(0);
        CHECK(subject(AmsState::instance().get_ams_page_has_prev_subject()) == 0);
        CHECK(subject(AmsState::instance().get_ams_page_has_next_subject()) == 1);
        CHECK(hidden("page_prev_button"));
        CHECK_FALSE(hidden("page_next_button"));
        // A step that is not available changes nothing.
        CHECK_FALSE(panel().prev_page());
        CHECK(page_current() == 0);

        go_to(11);
        CHECK(subject(AmsState::instance().get_ams_page_has_prev_subject()) == 1);
        CHECK(subject(AmsState::instance().get_ams_page_has_next_subject()) == 0);
        CHECK_FALSE(hidden("page_prev_button"));
        CHECK(hidden("page_next_button"));
        CHECK_FALSE(panel().next_page());
        CHECK(page_current() == 11);
    }

    SECTION("a middle page has both") {
        CHECK_FALSE(hidden("page_prev_button"));
        CHECK_FALSE(hidden("page_next_button"));
    }
}

TEST_CASE_METHOD(PagingFixture, "Unit view: one page shows no arrows and no dots",
                 "[ams][pages][panel]") {
    install([](PagedFleet& f) {
        f.specs.resize(2);
        f.specs[1].absent = true;
        f.loaded_slot = 1;
    });
    open_at(0);

    CHECK(page_count() == 1);
    CHECK(page_current() == 0);
    CHECK(subject(AmsState::instance().get_ams_page_has_prev_subject()) == 0);
    CHECK(subject(AmsState::instance().get_ams_page_has_next_subject()) == 0);
    CHECK(hidden("page_prev_button"));
    CHECK(hidden("page_next_button"));
    CHECK(hidden("page_dots"));
}

TEST_CASE_METHOD(PagingFixture, "Unit view: the arrows are 48 px touch targets",
                 "[ams][pages][panel]") {
    open_at(2);
    lv_obj_update_layout(root());
    for (const char* name : {"page_prev_button", "page_next_button"}) {
        CAPTURE(name);
        CHECK(lv_obj_get_width(named(name)) >= 48);
        CHECK(lv_obj_get_height(named(name)) >= 48);
    }

    // Level with the hub box, at the left and right edges of the path area.
    const lv_area_t hub = hub_box();
    lv_area_t prev;
    lv_area_t next;
    lv_area_t path;
    lv_obj_get_coords(named("page_prev_button"), &prev);
    lv_obj_get_coords(named("page_next_button"), &next);
    lv_obj_get_coords(named("detail_path_container"), &path);
    const int32_t hub_mid = (hub.y1 + hub.y2) / 2;
    CHECK(std::abs((prev.y1 + prev.y2) / 2 - hub_mid) <= 1);
    CHECK(std::abs((next.y1 + next.y2) / 2 - hub_mid) <= 1);
    CHECK(prev.x1 == path.x1);
    CHECK(next.x2 == path.x2);
    CHECK(prev.x2 < hub.x1);
    CHECK(next.x1 > hub.x2);
}

TEST_CASE_METHOD(PagingFixture, "Unit view: one dot per page, the current one in the accent",
                 "[ams][pages][panel]") {
    open_at(2);
    lv_obj_t* dots = named("page_dots");
    REQUIRE(lv_obj_get_child_count(dots) == 12);

    const lv_color_t accent = theme_manager_get_color("primary");
    const lv_color_t idle = theme_manager_get_color("text");
    auto dot_color = [&](uint32_t i) {
        return lv_obj_get_style_bg_color(lv_obj_get_child(dots, i), LV_PART_MAIN);
    };

    for (uint32_t i = 0; i < 12; ++i) {
        CAPTURE(i);
        CHECK(lv_color_eq(dot_color(i), i == 2 ? accent : idle));
        // Every dot the same size.
        CHECK(lv_obj_get_width(lv_obj_get_child(dots, i)) ==
              lv_obj_get_width(lv_obj_get_child(dots, 0)));
    }

    tap(named("page_next_button"));
    for (uint32_t i = 0; i < 12; ++i) {
        CAPTURE(i);
        CHECK(lv_color_eq(dot_color(i), i == 3 ? accent : idle));
    }
}

TEST_CASE_METHOD(PagingFixture, "Unit view: the dot pitch closes to a floor, then the row scrolls",
                 "[ams][pages][panel]") {
    open_at(2);
    lv_obj_t* dots = named("page_dots");
    lv_obj_update_layout(root());
    const int32_t nominal = theme_manager_get_spacing("space_sm");
    const int32_t floor_gap = theme_manager_get_spacing("space_xxs");
    const int32_t dot = theme_manager_get_spacing("ams_page_dot_size");
    const int32_t avail = lv_obj_get_content_width(dots);

    // Twelve dots fit at the nominal pitch here.
    REQUIRE(12 * dot + 11 * nominal <= avail);
    CHECK(lv_obj_get_style_pad_column(dots, LV_PART_MAIN) == nominal);

    // Enough units to need less than the nominal gap, then more than the floor allows.
    const int closing = (avail - 2 * dot) / (dot + nominal) + 3;
    REQUIRE(closing * dot + (closing - 1) * floor_gap <= avail);
    install([&](PagedFleet& f) {
        f.specs.clear();
        for (int u = 0; u < closing; ++u)
            f.specs.push_back({"unit_" + std::to_string(u), "", false});
        f.loaded_slot = -1;
    });
    close();
    open_at(0);
    REQUIRE(page_count() == closing);
    process_lvgl(60);
    const int32_t gap = lv_obj_get_style_pad_column(dots, LV_PART_MAIN);
    CHECK(gap < nominal);
    CHECK(gap >= floor_gap);
    CHECK(lv_obj_get_scroll_right(dots) <= 0);

    const int past = avail / (dot + floor_gap) + 6;
    install([&](PagedFleet& f) {
        f.specs.clear();
        for (int u = 0; u < past; ++u)
            f.specs.push_back({"unit_" + std::to_string(u), "", false});
        f.loaded_slot = -1;
    });
    close();
    open_at(0);
    REQUIRE(page_count() == past);
    process_lvgl(60);
    CHECK(lv_obj_get_style_pad_column(dots, LV_PART_MAIN) == floor_gap);
    // The row overflows, and the last page's dot is brought into view when it is current.
    CHECK(lv_obj_get_scroll_right(dots) > 0);
    go_to(past - 1);
    process_lvgl(60);
    lv_area_t row;
    lv_area_t last_dot;
    lv_obj_get_coords(dots, &row);
    lv_obj_get_coords(lv_obj_get_child(dots, past - 1), &last_dot);
    CHECK(last_dot.x2 <= row.x2);
}

TEST_CASE_METHOD(PagingFixture, "Unit view: a horizontal swipe pages, left for next",
                 "[ams][pages][panel][swipe]") {
    open_at(2);
    REQUIRE(page_current() == 2);
    const lv_point_t spot = empty_spot();

    swipe(spot, -150);
    CHECK(page_current() == 3);
    swipe(spot, +150);
    CHECK(page_current() == 2);
    swipe(spot, +150);
    CHECK(page_current() == 1);

    SECTION("a vertical drag pages nowhere") {
        REQUIRE(UITest::press_at(spot.x, spot.y));
        REQUIRE(UITest::move_to(spot.x, spot.y + 120));
        REQUIRE(UITest::release());
        drain();
        CHECK(page_current() == 1);
    }

    SECTION("a swipe toward a hidden arrow is ignored") {
        go_to(0);
        swipe(spot, +150);
        CHECK(page_current() == 0);
        go_to(11);
        swipe(spot, -150);
        CHECK(page_current() == 11);
    }
}

TEST_CASE_METHOD(PagingFixture, "Unit view: a swipe that starts on a spool is not a tap on it",
                 "[ams][pages][panel][swipe]") {
    open_at(2);
    auto spool_center = [&] {
        lv_obj_t* slot = lv_obj_get_child(named("slot_grid"), 0);
        REQUIRE(slot != nullptr);
        lv_area_t c;
        lv_obj_get_coords(slot, &c);
        return lv_point_t{(c.x1 + c.x2) / 2, (c.y1 + c.y2) / 2};
    };

    // A tap there opens the slot menu: the known positive for the checks below.
    lv_point_t on_spool = spool_center();
    REQUIRE(UITest::press_at(on_spool.x, on_spool.y));
    REQUIRE(UITest::release());
    drain();
    process_lvgl(60);
    REQUIRE(slot_menu_open());
    close();
    open_at(2);
    REQUIRE_FALSE(slot_menu_open());

    SECTION("a swipe that pages") {
        const int before = page_current();
        swipe(on_spool, -150);
        process_lvgl(60);
        CHECK(page_current() == before + 1);
        CHECK_FALSE(slot_menu_open());
    }

    SECTION("a swipe that goes nowhere still does not tap the spool it began on") {
        // The page does not change, so the spool it began on is still under the finger
        // when it lifts: the release must not count as a tap.
        go_to(11);
        on_spool = spool_center();
        swipe(on_spool, -150);
        process_lvgl(60);
        CHECK(page_current() == 11);
        CHECK_FALSE(slot_menu_open());
    }
}

TEST_CASE_METHOD(PagingFixture, "Unit view: the hub stays put from page to page of a hub",
                 "[ams][pages][panel]") {
    open_at(2);
    go_to(0);
    const lv_area_t first = hub_box();
    go_to(5);
    const lv_area_t middle = hub_box();
    go_to(9);
    const lv_area_t last = hub_box();

    // The stubs' entries widen the hub the same on every page, so the box does not move.
    CHECK(lv_area_get_width(&first) == lv_area_get_width(&middle));
    CHECK(lv_area_get_width(&middle) == lv_area_get_width(&last));
    CHECK(first.x1 == middle.x1);
    CHECK(middle.x1 == last.x1);
    CHECK(first.y1 == middle.y1);
    CHECK(middle.y1 == last.y1);
}

TEST_CASE_METHOD(PagingFixture, "Unit view: the canvas is told the units on other pages",
                 "[ams][pages][panel][stubs]") {
    open_at(2);
    lv_obj_t* canvas = named("detail_path_canvas");
    const auto* data = get_data(canvas);
    REQUIRE(data != nullptr);

    // Unit 2: units 0 and 1 before it, 3-9 after it, and unit 6 among them is drying.
    CHECK(data->offpage_before == 2);
    CHECK(data->offpage_after == 7);
    CHECK_FALSE(data->offpage_before_drying);
    CHECK(data->offpage_after_drying);

    go_to(0);
    CHECK(data->offpage_before == 0);
    CHECK(data->offpage_after == 9);
    go_to(9);
    CHECK(data->offpage_before == 9);
    CHECK(data->offpage_after == 0);
    CHECK(data->offpage_before_drying);
    CHECK_FALSE(data->offpage_after_drying);

    // The second hub is its own group: nothing on the first hub's side.
    go_to(10);
    CHECK(data->offpage_before == 0);
    CHECK(data->offpage_after == 1);
    go_to(11);
    CHECK(data->offpage_before == 1);
    CHECK(data->offpage_after == 0);

    SECTION("a dryer starting elsewhere reaches the stubs") {
        go_to(2);
        auto* fleet = static_cast<PagedFleet*>(AmsState::instance().get_backend());
        fleet->drying_unit = kOwnNumberBase + 1;
        AmsState::instance().sync_from_backend();
        drain();
        CHECK(data->offpage_before_drying);
        CHECK_FALSE(data->offpage_after_drying);
        CHECK(page_current() == 2);
    }
}

TEST_CASE_METHOD(PagingFixture, "Unit view: the env chip shows the unit on screen",
                 "[ams][pages][panel][env]") {
    install([](PagedFleet& f) { f.loaded_slot = 10 * kSlotsPerUnit + 1; });
    open_at(10);
    REQUIRE(page_current() == 10);

    lv_subject_t* temp = lv_xml_get_subject(nullptr, "ams_env_ind_detail_temp_text");
    REQUIRE(temp != nullptr);
    CHECK(std::string(lv_subject_get_string(temp)) == "30\xC2\xB0"
                                                      "C");
    lv_subject_t* humidity = lv_xml_get_subject(nullptr, "ams_env_ind_detail_humidity_text");
    REQUIRE(humidity != nullptr);
    CHECK(std::string(lv_subject_get_string(humidity)) == "40%");
    // Tapping the chip opens the environment of this unit.
    CHECK(reinterpret_cast<intptr_t>(lv_obj_get_user_data(view_chip())) == kOwnNumberBase + 10);

    go_to(0);
    CHECK(std::string(lv_subject_get_string(temp)) == "20\xC2\xB0"
                                                      "C");
    CHECK(reinterpret_cast<intptr_t>(lv_obj_get_user_data(view_chip())) == kOwnNumberBase);
}

TEST_CASE_METHOD(PagingFixture, "Unit view: a disconnected unit's page says so in the header",
                 "[ams][pages][panel]") {
    install([](PagedFleet& f) { f.specs[3].connected = false; });
    open_at(2);
    REQUIRE(page_current() == 2);
    lv_obj_t* chip = named("detail_disconnected_chip");
    CHECK(lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN));

    // The flag is the viewed unit's, and it follows the page.
    go_to(3);
    CHECK_FALSE(lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN));
    go_to(4);
    CHECK(lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(PagingFixture, "Unit view: a change in the system keeps the shown unit",
                 "[ams][pages][panel][structure]") {
    open_at(2);
    go_to(4);
    REQUIRE(unit_name() == "Box unit 4");
    auto* fleet = static_cast<PagedFleet*>(AmsState::instance().get_backend());

    SECTION("a unit appearing late does not move the page") {
        fleet->specs.push_back({"unit_new", "fps2", false});
        lv_subject_t* version = AmsState::instance().get_slots_version_subject();
        lv_subject_set_int(version, lv_subject_get_int(version) + 1);
        drain();
        process_lvgl(30);
        CHECK(page_count() == 13);
        CHECK(page_current() == 4);
        CHECK(unit_name() == "Box unit 4");
    }

    SECTION("a unit inserted ahead of the shown one moves its page, not the screen") {
        fleet->specs.insert(fleet->specs.begin(), {"unit_first", "fps", false});
        AmsPanelTestAccess::resync(panel());
        CHECK(page_count() == 13);
        CHECK(page_current() == 5);
        CHECK(unit_name() == "Box unit 4");
    }

    SECTION("the shown unit going absent clamps to a page that remains") {
        fleet->specs[4].absent = true;
        AmsPanelTestAccess::resync(panel());
        CHECK(page_count() == 11);
        CHECK(page_current() == 4);
        CHECK(unit_name() != "Box unit 4");
        CHECK(AmsPanelTestAccess::shown_unit_position(panel()) == 5);
    }

    SECTION("the last unit going absent while shown clamps to the new last page") {
        go_to(11);
        fleet->specs[11].absent = true;
        AmsPanelTestAccess::resync(panel());
        CHECK(page_count() == 11);
        CHECK(page_current() == 10);
        CHECK(unit_name() == "Box unit 10");
    }

    SECTION("a refresh that changes nothing leaves the page alone") {
        fleet->loaded_slot = 9 * kSlotsPerUnit;
        AmsState::instance().sync_from_backend();
        drain();
        process_lvgl(30);
        // Filament loading elsewhere does not carry the screen along.
        CHECK(page_current() == 4);
        CHECK(unit_name() == "Box unit 4");
    }
}

TEST_CASE_METHOD(PagingFixture, "Unit view: Back returns to the overview, and Back again leaves",
                 "[ams][pages][panel][overview]") {
    install([](PagedFleet& f) { f.specs[2].connected = false; });
    open_at(2);
    lv_obj_t* panel_root = root();
    REQUIRE(helix::nav::is_showing(panel_root));
    // The unit on screen is the disconnected one.
    REQUIRE(subject(lv_xml_get_subject(nullptr, "ams_viewed_unit_disconnected")) == 1);

    tap(named("back_button"));
    CHECK(helix::nav::is_showing(panel_root));
    CHECK_FALSE(AmsPanelTestAccess::in_unit_view(panel()));
    CHECK(subject(AmsState::instance().get_ams_unit_view_active_subject()) == 0);
    CHECK_FALSE(lv_obj_has_flag(named("unit_cards_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(named("system_path_area"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(named("unit_detail_container"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(named("header_title"), LV_OBJ_FLAG_HIDDEN));
    CHECK(page_count() == 0);
    CHECK(AmsPanelTestAccess::pages(panel()).empty());
    // No unit is viewed any more.
    CHECK(subject(lv_xml_get_subject(nullptr, "ams_viewed_unit_disconnected")) == 0);

    tap(named("back_button"));
    CHECK_FALSE(helix::nav::is_showing(panel_root));
    CHECK_FALSE(AmsPanelTestAccess::is_open(panel()));
}

TEST_CASE_METHOD(PagingFixture, "Unit view: reopening the panel shows the overview",
                 "[ams][pages][panel][overview]") {
    open_at(2);
    go_to(7);
    close();
    open();
    CHECK(helix::nav::is_showing(root()));
    CHECK_FALSE(AmsPanelTestAccess::in_unit_view(panel()));
    CHECK(subject(AmsState::instance().get_ams_unit_view_active_subject()) == 0);
    CHECK_FALSE(lv_obj_has_flag(named("unit_cards_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(named("unit_detail_container"), LV_OBJ_FLAG_HIDDEN));

    // A card opens its own page again, not the one left behind.
    enter(2);
    CHECK(page_current() == 2);
}

TEST_CASE_METHOD(PagingFixture, "Unit view: a swipe over the overview pages nothing",
                 "[ams][pages][panel][overview][swipe]") {
    open();
    REQUIRE(lv_obj_get_scroll_right(named("unit_cards_row")) > 0);

    lv_area_t path;
    lv_obj_get_coords(named("system_path_area"), &path);
    const lv_point_t on_path = {(path.x1 + path.x2) / 2, (path.y1 + path.y2) / 2};
    swipe(on_path, -150);
    swipe(on_path, +150);
    CHECK_FALSE(AmsPanelTestAccess::in_unit_view(panel()));
    CHECK(page_count() == 0);

    // The cards row still scrolls sideways under a drag, and that pages nothing either.
    lv_obj_t* row = named("unit_cards_row");
    lv_area_t r;
    lv_obj_get_coords(row, &r);
    const lv_point_t on_row = {(r.x1 + r.x2) / 2, (r.y1 + r.y2) / 2};
    const int32_t before = lv_obj_get_scroll_x(row);
    swipe(on_row, -150);
    CHECK(lv_obj_get_scroll_x(row) > before);
    CHECK_FALSE(AmsPanelTestAccess::in_unit_view(panel()));
    CHECK(page_count() == 0);
}

TEST_CASE_METHOD(PagingFixture, "Unit view: coming back from an overlay keeps the page",
                 "[ams][pages][panel]") {
    open_at(2);
    go_to(7);
    // An overlay covers the panel and goes away; the panel never closed.
    panel().on_deactivate(DeactivateReason::NavigateAway);
    panel().on_activate();
    CHECK(AmsPanelTestAccess::in_unit_view(panel()));
    CHECK(page_current() == 7);
    CHECK(unit_name() == "Box unit 7");
}

TEST_CASE_METHOD(PagingFixture, "Unit view: a single-unit system still opens the AMS panel",
                 "[ams][pages][panel]") {
    auto mock = std::make_unique<AmsBackendMock>(4);
    REQUIRE(mock->start().success());
    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();

    open();
    AmsPanel* single = get_existing_ams_panel();
    REQUIRE(single != nullptr);
    CHECK(helix::nav::is_showing(single->get_panel()));
    CHECK(helix::lazy_global_if_exists<AmsOverviewPanel>() == nullptr);
}

TEST_CASE_METHOD(PagingFixture, "Unit view: with no room beside the hub the arrows sit below it",
                 "[ams][pages][panel]") {
    open_at(2);
    lv_obj_t* path = named("detail_path_container");
    lv_area_t wide_hub = hub_box();
    lv_area_t prev;
    lv_obj_get_coords(named("page_prev_button"), &prev);
    // Room beside the hub: level with it.
    CHECK(std::abs((prev.y1 + prev.y2) / 2 - (wide_hub.y1 + wide_hub.y2) / 2) <= 1);
    CHECK(get_data(named("detail_path_canvas"))->edge_reserve > 0);

    // A canvas hardly wider than the hub leaves no room for an arrow at either side.
    const int32_t hub_w = lv_area_get_width(&wide_hub);
    lv_obj_set_width(path, hub_w + 20);
    lv_obj_update_layout(root());
    process_lvgl(50);

    const lv_area_t hub = hub_box();
    lv_obj_get_coords(named("page_prev_button"), &prev);
    lv_area_t next;
    lv_obj_get_coords(named("page_next_button"), &next);
    CHECK(prev.y1 >= hub.y2);
    CHECK(next.y1 >= hub.y2);
    // Still at the canvas edges, and nothing stands in the way of the stubs' labels any more.
    lv_area_t canvas;
    lv_obj_get_coords(named("detail_path_canvas"), &canvas);
    CHECK(prev.x1 == canvas.x1);
    CHECK(next.x2 == canvas.x2);
    CHECK(get_data(named("detail_path_canvas"))->edge_reserve == 0);
}

TEST_CASE_METHOD(PagingFixture, "Unit view: the hub holds still on a unit with fewer lanes",
                 "[ams][pages][panel]") {
    install([](PagedFleet& f) {
        // A one-bay unit first, then four-bay units, all on one hub.
        f.specs = {{"unit_0", "fps", false, 1},
                   {"unit_1", "fps", false, kSlotsPerUnit},
                   {"unit_2", "fps", false, kSlotsPerUnit}};
        f.loaded_slot = -1;
    });
    open_at(0);
    lv_obj_t* canvas = named("detail_path_canvas");
    REQUIRE(get_data(canvas)->fixed_hub_lanes == kSlotsPerUnit);
    // At the lane pitch of a four-bay unit's slot row.
    CHECK(get_data(canvas)->fixed_hub_pitch > 0);

    const lv_area_t first = hub_box();
    go_to(1);
    const lv_area_t second = hub_box();
    // The hub's center line is the widget's, whichever unit stands over it.
    lv_area_t c;
    lv_obj_get_coords(canvas, &c);
    CHECK(std::abs((first.x1 + first.x2) / 2 - (c.x1 + c.x2) / 2) <= 1);
    CHECK(std::abs((second.x1 + second.x2) / 2 - (c.x1 + c.x2) / 2) <= 1);
    CHECK(first.y1 == second.y1);
    // And the box itself is the widest unit's, not the one-bay unit's.
    CHECK(first.x1 == second.x1);
    CHECK(first.x2 == second.x2);
    CHECK(first.y2 == second.y2);
}

TEST_CASE_METHOD(PagingFixture,
                 "Unit view: a small screen's header names the unit instead of the screen",
                 "[ams][pages][panel]") {
    open_at(2);
    lv_subject_t* bp = lv_xml_get_subject(nullptr, "ui_breakpoint");
    REQUIRE(bp != nullptr);
    const int original = lv_subject_get_int(bp);
    lv_obj_t* title = named("header_title_unit_view");
    lv_obj_t* colon = named("header_colon");
    lv_obj_t* logo = named("detail_logo");
    lv_obj_t* name = named("detail_unit_name");

    for (int breakpoint = 0; breakpoint <= 4; breakpoint++) {
        CAPTURE(breakpoint);
        lv_subject_set_int(bp, breakpoint);
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
        const bool small = breakpoint < 2;
        // Below breakpoint 2 the title and the colon give way; the logo and name never do.
        CHECK(lv_obj_has_flag(title, LV_OBJ_FLAG_HIDDEN) == small);
        CHECK(lv_obj_has_flag(colon, LV_OBJ_FLAG_HIDDEN) == small);
        CHECK_FALSE(lv_obj_has_flag(logo, LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(lv_obj_has_flag(name, LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(lv_obj_has_flag(named("header_backend_suffix"), LV_OBJ_FLAG_HIDDEN));
        // The overview's own title is not in play.
        CHECK(lv_obj_has_flag(named("header_title"), LV_OBJ_FLAG_HIDDEN));
    }
    lv_subject_set_int(bp, original);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLTestFixture, "AmsState: the page subjects hold a page inside its count",
                 "[ams][pages][subjects]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);

    auto values = [&] {
        return std::array<int, 4>{subject_value(ams.get_ams_page_count_subject()),
                                  subject_value(ams.get_ams_page_current_subject()),
                                  subject_value(ams.get_ams_page_has_prev_subject()),
                                  subject_value(ams.get_ams_page_has_next_subject())};
    };

    ams.set_unit_page(5, 2);
    CHECK(values() == std::array<int, 4>{5, 2, 1, 1});
    ams.set_unit_page(5, 0);
    CHECK(values() == std::array<int, 4>{5, 0, 0, 1});
    ams.set_unit_page(5, 4);
    CHECK(values() == std::array<int, 4>{5, 4, 1, 0});
    // A page outside the count lands on the nearest one.
    ams.set_unit_page(5, 9);
    CHECK(values() == std::array<int, 4>{5, 4, 1, 0});
    ams.set_unit_page(5, -3);
    CHECK(values() == std::array<int, 4>{5, 0, 0, 1});
    // One page, or none: no neighbor either way.
    ams.set_unit_page(1, 0);
    CHECK(values() == std::array<int, 4>{1, 0, 0, 0});
    ams.set_unit_page(0, 3);
    CHECK(values() == std::array<int, 4>{0, 0, 0, 0});

    ams.set_unit_page_header("Box 7", nullptr);
    CHECK(std::string(lv_subject_get_string(ams.get_ams_page_unit_name_subject())) == "Box 7");
    ams.set_unit_page(0, 0);
    ams.set_unit_page_header("", nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Bypass spool follows AmsState: hidden with the node, painted with the spool",
                 "[ams][bypass][spool_widget]") {
    class BypassMock : public AmsBackendMock {
      public:
        BypassMock() : AmsBackendMock(4) {}
        AmsSystemInfo get_system_info() const override {
            AmsSystemInfo info = AmsBackendMock::get_system_info();
            info.supports_bypass = true;
            return info;
        }
    };

    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto mock = std::make_unique<BypassMock>();
    REQUIRE(mock->start().success());
    ams.set_backend(std::move(mock));
    ams.sync_from_backend();

    helix::ui::BypassSpoolWidgets w =
        helix::ui::bypass_spool_create(lv_screen_active(), nullptr, nullptr);
    REQUIRE(w.valid());

    SlotInfo spool;
    spool.color_rgb = 0x3366CC;
    spool.material = "PETG";
    ams.set_external_spool_info_in_memory(spool);
    helix::ui::bypass_spool_sync_from_state(w);
    REQUIRE(helix::ui::bypass_node_visible_for(ams.get_backend()));
    CHECK_FALSE(lv_obj_has_flag(w.box, LV_OBJ_FLAG_HIDDEN));
    CHECK(w.cached_color_rgb == 0x3366CC);
    CHECK(w.cached_has_spool);
    CHECK(std::string(w.cached_material) == "PETG");

    // The node leaves the path: the widget goes with it.
    ams.set_backend(nullptr);
    helix::ui::bypass_spool_sync_from_state(w);
    CHECK(lv_obj_has_flag(w.box, LV_OBJ_FLAG_HIDDEN));

    helix::ui::bypass_spool_destroy(w);
}
