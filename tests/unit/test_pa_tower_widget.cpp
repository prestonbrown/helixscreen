// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_pa_tower.h"

#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

TEST_CASE_METHOD(LVGLUITestFixture, "pa_tower: a tap reports the height under it, plate to top",
                 "[pa_tuner][pa_tower_widget]") {
    lv_obj_t* tower =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "pa_tower", nullptr));
    REQUIRE(tower != nullptr);
    lv_obj_set_size(tower, 200, 400);
    PaTowerModel m;
    m.banded = false;
    m.fill = PaTowerFill::Full;
    m.faces = {helix::pa::TowerFace::Left};
    pa_tower_set_model(tower, m);
    lv_obj_update_layout(tower);

    lv_area_t a;
    lv_obj_get_content_coords(tower, &a);
    // The plate sits near the bottom, the top of the tower near the top.
    CHECK(pa_tower_height_at(tower, a.y2) == Catch::Approx(0.0f).margin(0.01f));
    CHECK(pa_tower_height_at(tower, a.y1) == Catch::Approx(m.height_mm).margin(0.01f));
    const float low = pa_tower_height_at(tower, a.y1 + (a.y2 - a.y1) * 3 / 4);
    const float high = pa_tower_height_at(tower, a.y1 + (a.y2 - a.y1) / 4);
    CHECK(low > 0.0f);
    CHECK(high > low);
    CHECK(high < m.height_mm);

    // The banded tower maps the same way to its own height.
    PaTowerModel banded;
    banded.banded = true;
    banded.height_mm = 31.25f;
    pa_tower_set_model(tower, banded);
    CHECK(pa_tower_height_at(tower, a.y1) == Catch::Approx(31.25f).margin(0.01f));
    lv_obj_delete(tower);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "pa_tower: a drag keeps picking the height under the finger, on the face it "
                 "started on",
                 "[pa_tuner][pa_tower_widget]") {
    UITest::init(lv_screen_active());
    lv_obj_t* tower =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "pa_tower", nullptr));
    REQUIRE(tower != nullptr);
    lv_obj_set_pos(tower, 0, 0);
    lv_obj_set_size(tower, 300, 400);
    PaTowerModel m;
    m.banded = false;
    m.fill = PaTowerFill::Full;
    m.faces = {helix::pa::TowerFace::Left, helix::pa::TowerFace::Front};
    pa_tower_set_model(tower, m);
    lv_obj_update_layout(tower);
    int picks = 0;
    lv_obj_add_event_cb(
        tower, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_VALUE_CHANGED, &picks);

    lv_area_t a;
    lv_obj_get_content_coords(tower, &a);
    const int32_t h = a.y2 - a.y1;
    const int32_t left_x = a.x1 + 20;
    const int32_t front_x = a.x2 - 20;
    const int32_t low_y = a.y1 + h * 3 / 4;
    const int32_t high_y = a.y1 + h / 4;

    REQUIRE(UITest::press_at(left_x, low_y));
    CHECK(picks == 1);
    CHECK(pa_tower_tap_face(tower) == 0);
    const float low = pa_tower_tap_height(tower);
    CHECK(low == Catch::Approx(pa_tower_height_at(tower, low_y)).margin(0.01f));

    // Up the tower in small steps, drifting over the front face on the way.
    for (int32_t y = low_y - 10; y >= high_y; y -= 10) {
        REQUIRE(UITest::move_to(y < (low_y + high_y) / 2 ? front_x : left_x, y));
    }
    CHECK(picks > 2);
    const float high = pa_tower_tap_height(tower);
    CHECK(high > low);
    CHECK(pa_tower_tap_face(tower) == 0);

    const int before_release = picks;
    REQUIRE(UITest::release());
    CHECK(picks == before_release);
    CHECK(pa_tower_tap_face(tower) == 0);
    CHECK(pa_tower_tap_height(tower) == Catch::Approx(high));
    lv_obj_delete(tower);
    UITest::cleanup();
}

TEST_CASE_METHOD(LVGLUITestFixture, "pa_tower: a banded tower with no bands yet draws",
                 "[pa_tuner][pa_tower_widget]") {
    // The pick before the printer reported a band: the ruler has nothing to mark.
    lv_obj_t* tower =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "pa_tower", nullptr));
    REQUIRE(tower != nullptr);
    lv_obj_set_size(tower, 200, 400);
    for (auto fill : {PaTowerFill::Preview, PaTowerFill::Outline, PaTowerFill::Live,
                      PaTowerFill::Full, PaTowerFill::Wall}) {
        PaTowerModel m;
        m.banded = true;
        m.height_mm = 31.25f;
        m.fill = fill;
        pa_tower_set_model(tower, m);
        lv_refr_now(nullptr);
    }
    PaTowerModel faces;
    faces.banded = false;
    faces.faces = {helix::pa::TowerFace::Left, helix::pa::TowerFace::Front};
    for (auto fill : {PaTowerFill::Preview, PaTowerFill::Outline, PaTowerFill::Live,
                      PaTowerFill::Full, PaTowerFill::Wall}) {
        faces.fill = fill;
        pa_tower_set_model(tower, faces);
        lv_refr_now(nullptr);
    }
    CHECK(pa_tower_height_at(tower, 0) >= 0.0f);
    lv_obj_delete(tower);
}

TEST_CASE_METHOD(LVGLUITestFixture, "pa_tower: the finishing flourish fades out and stops",
                 "[pa_tuner][pa_tower_widget]") {
    lv_obj_t* tower =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "pa_tower", nullptr));
    REQUIRE(tower != nullptr);
    lv_obj_set_size(tower, 200, 400);
    PaTowerModel m;
    m.banded = false;
    m.fill = PaTowerFill::Full;
    m.faces = {helix::pa::TowerFace::Left};
    pa_tower_set_model(tower, m);
    auto& display = DisplaySettingsManager::instance();
    const bool animations = display.get_animations_enabled();
    display.set_animations_enabled(true);
    pa_tower_flourish(tower);
    CHECK(lv_anim_get(tower, nullptr) != nullptr);
    process_lvgl(1200);
    // Done within its 0.9 s: nothing of it still runs.
    CHECK(lv_anim_get(tower, nullptr) == nullptr);
    // A tower deleted mid-flourish takes its animation with it.
    pa_tower_flourish(tower);
    lv_obj_delete(tower);
    process_lvgl(1200);

    // With animations off there is no flourish at all.
    lv_obj_t* still =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "pa_tower", nullptr));
    display.set_animations_enabled(false);
    pa_tower_flourish(still);
    CHECK(lv_anim_get(still, nullptr) == nullptr);
    lv_obj_delete(still);
    display.set_animations_enabled(animations);
}
