// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_grid_edit_chrome_contrast.cpp
 * @brief The edit-mode selection's actions: the tile callout's rows and the
 * navbar's delete-page request
 */

#include "../test_fixtures.h"
#include "../test_helpers/grid_edit_mode_test_access.h"
#include "../test_helpers/grid_edit_scene.h"
#include "../test_helpers/scoped_pointer_indev.h"
#include "../ui_test_utils.h"
#include "grid_edit_mode.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "tap_lockout.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A PanelWidget that advertises edit-mode configuration, so the callout
/// offers its Configure row.
class ConfigurableWidget : public PanelWidget {
  public:
    void attach(lv_obj_t*, lv_obj_t*) override {}
    void detach() override {}
    const char* id() const override {
        return "temperature";
    }
    bool has_edit_configure() const override {
        return true;
    }
    /// Counts the Configure row's taps; false asks for no rebuild.
    bool on_edit_configure() override {
        ++configure_calls;
        return false;
    }
    int configure_calls = 0;
};

lv_point_t centre_of(lv_obj_t* obj) {
    lv_obj_update_layout(obj);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_point_t c{(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
    REQUIRE(lv_indev_search_obj(lv_screen_active(), &c) == obj);
    return c;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "tile callout rows: only a tap past the lockout that lifts on the row acts",
                 "[grid_edit][edit-callout]") {
    REQUIRE(lv_xml_register_component_from_file("A:ui_xml/tile_edit_callout.xml") == LV_RESULT_OK);
    GridEditScene scene(test_screen(), "test_tile_callout_rows");
    ConfigurableWidget pw;
    lv_obj_set_user_data(scene.widget, &pw);
    GridEditMode em;
    em.set_rebuild_callback([]() {});
    REQUIRE(em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex)));
    em.select_widget(scene.widget);
    em.open_tile_callout();
    REQUIRE(em.is_callout_open());
    lv_obj_t* configure = lv_obj_find_by_name(lv_screen_active(), "tile_callout_configure");
    REQUIRE(configure != nullptr);
    REQUIRE_FALSE(lv_obj_is_hidden(configure));
    process_lvgl(20); // the card's first frame starts the lockout

    helix_test::ScopedPointerIndev pointer;
    const lv_point_t c = centre_of(configure);
    // A double tap's second press, inside the lockout, lifting after it.
    pointer.press(c.x, c.y);
    lv_tick_inc(helix::ui::TapLockout::kDefaultMs);
    pointer.release(c.x, c.y);
    CHECK(pw.configure_calls == 0);
    REQUIRE(em.is_callout_open());

    // A press that slides off the row before it lifts.
    pointer.press(c.x, c.y);
    pointer.move(c.x, c.y + 200);
    pointer.release(c.x, c.y + 200);
    CHECK(pw.configure_calls == 0);
    REQUIRE(em.is_callout_open());

    // A tap.
    pointer.press(c.x, c.y);
    pointer.release(c.x, c.y);
    CHECK(pw.configure_calls == 1);
    CHECK_FALSE(em.is_callout_open());
    CHECK(em.selected_widget() == scene.widget);

    // A click with no input device behind it (helix-screen ctl) is no finger
    // under a new card, so the lockout does not hold it back.
    process_lvgl(50);
    em.open_tile_callout();
    REQUIRE(em.is_callout_open());
    lv_obj_t* remove = lv_obj_find_by_name(lv_screen_active(), "tile_callout_remove");
    REQUIRE(remove != nullptr);
    lv_obj_send_event(remove, LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(scene.config->is_placed("temperature"));

    em.exit();
    lv_obj_set_user_data(scene.widget, nullptr);
    process_lvgl(50);
    process_lvgl(50);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture, "the navbar's delete-page request acts only on a deletable page",
                 "[grid_edit][edit-chrome][2023]") {
    GridEditScene scene(test_screen(), "test_delete_page_request");
    GridEditMode em;
    int delete_page_calls = 0;
    em.set_delete_page_callback([&]() { ++delete_page_calls; });

    // Outside a session nothing is offered and a request does nothing.
    em.request_delete_page();
    CHECK(delete_page_calls == 0);

    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
    REQUIRE(helix::delete_page_offered());
    em.request_delete_page();
    CHECK(delete_page_calls == 1);

    em.exit();
    CHECK_FALSE(helix::delete_page_offered());
    em.request_delete_page();
    CHECK(delete_page_calls == 1);

    // The main page is never deletable.
    REQUIRE(scene.config->main_page_index() == 0);
    em.enter(scene.container, scene.config, 0);
    CHECK_FALSE(helix::delete_page_offered());
    em.request_delete_page();
    CHECK(delete_page_calls == 1);
    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}
