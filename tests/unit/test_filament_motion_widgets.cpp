// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The two widgets that draw a manual Extrude / Retract: <filament_motion_glyph>
// on the filament panel and the AMS <filament_path_canvas>, both bound to the
// motion subject through bind_motion.

#include "ui_filament_motion_glyph.h"
#include "ui_filament_path_canvas.h"

#include "../test_fixtures.h"
#include "ams_types.h"
#include "display_settings_manager.h"
#include "filament_motion.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

using helix::FilamentMotion;

namespace {

constexpr const char* kSubject = "test_filament_manual_motion";

// Animations follow the global preference; the fixture forces it off.
struct AnimationsOn {
    lv_subject_t* pref = helix::DisplaySettingsManager::instance().subject_animations_enabled();
    int was = lv_subject_get_int(pref);
    AnimationsOn() {
        lv_subject_set_int(pref, 1);
    }
    ~AnimationsOn() {
        lv_subject_set_int(pref, was);
    }
};

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "filament_motion_glyph draws the loaded filament's color",
                 "[filament-chevrons][widget]") {
    lv_subject_t color;
    lv_subject_init_int(&color, static_cast<int>(helix::kNoFilamentPaint));
    lv_xml_register_subject(nullptr, "test_filament_color", &color);
    const char* attrs[] = {"bind_color", "test_filament_color", nullptr};
    auto* obj =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "filament_motion_glyph", attrs));
    REQUIRE(obj != nullptr);
    CHECK(helix::ui::filament_motion_glyph_color(obj) == helix::kNoFilamentPaint);

    lv_subject_set_int(&color, 0x12AB34);
    CHECK(helix::ui::filament_motion_glyph_color(obj) == 0x12AB34);
    lv_subject_set_int(&color, static_cast<int>(helix::kNoFilamentPaint));
    CHECK(helix::ui::filament_motion_glyph_color(obj) == helix::kNoFilamentPaint);

    lv_obj_delete(obj);
    lv_subject_deinit(&color);
}

TEST_CASE_METHOD(XMLTestFixture, "filament_motion_glyph follows bind_motion",
                 "[filament-chevrons][widget]") {
    lv_subject_t subj;
    lv_subject_init_int(&subj, 0);
    lv_xml_register_subject(nullptr, kSubject, &subj);
    const char* attrs[] = {"bind_motion", kSubject, "width", "40", "height", "80", nullptr};
    auto* glyph =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "filament_motion_glyph", attrs));
    REQUIRE(glyph != nullptr);
    process_lvgl(20);
    CHECK(helix::ui::filament_motion_glyph_motion(glyph) == FilamentMotion::Idle);
    CHECK_FALSE(helix::ui::filament_motion_glyph_animating(glyph));

    SECTION("animations on: the chevrons run only while a move does") {
        AnimationsOn on;
        lv_subject_set_int(&subj, static_cast<int>(FilamentMotion::Extrude));
        process_lvgl(20);
        CHECK(helix::ui::filament_motion_glyph_motion(glyph) == FilamentMotion::Extrude);
        CHECK(helix::ui::filament_motion_glyph_animating(glyph));
        lv_subject_set_int(&subj, static_cast<int>(FilamentMotion::Retract));
        process_lvgl(20);
        CHECK(helix::ui::filament_motion_glyph_motion(glyph) == FilamentMotion::Retract);
        CHECK(helix::ui::filament_motion_glyph_animating(glyph));
        lv_subject_set_int(&subj, static_cast<int>(FilamentMotion::Idle));
        process_lvgl(20);
        CHECK_FALSE(helix::ui::filament_motion_glyph_animating(glyph));
    }
    SECTION("animations off: the direction shows with nothing running") {
        lv_subject_set_int(&subj, static_cast<int>(FilamentMotion::Retract));
        process_lvgl(20);
        CHECK(helix::ui::filament_motion_glyph_motion(glyph) == FilamentMotion::Retract);
        CHECK_FALSE(helix::ui::filament_motion_glyph_animating(glyph));
    }

    lv_obj_delete(glyph);
    lv_subject_deinit(&subj);
}

TEST_CASE_METHOD(XMLTestFixture, "filament_path_canvas streams chevrons for a manual move",
                 "[filament-chevrons][widget]") {
    ui_filament_path_canvas_register();
    lv_subject_t subj;
    lv_subject_init_int(&subj, 0);
    lv_xml_register_subject(nullptr, kSubject, &subj);
    const char* attrs[] = {"bind_motion", kSubject, nullptr};
    auto* canvas =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "filament_path_canvas", attrs));
    REQUIRE(canvas != nullptr);
    process_lvgl(20);
    CHECK(ui_filament_path_canvas_get_manual_motion(canvas) == 0);
    CHECK_FALSE(ui_filament_path_canvas_is_animating(canvas));

    lv_subject_set_int(&subj, static_cast<int>(FilamentMotion::Extrude));
    process_lvgl(20);
    CHECK(ui_filament_path_canvas_get_manual_motion(canvas) == 1);
    CHECK(ui_filament_path_canvas_is_animating(canvas));
    CHECK_FALSE(ui_filament_path_canvas_flow_reversed(canvas));

    lv_subject_set_int(&subj, static_cast<int>(FilamentMotion::Retract));
    process_lvgl(20);
    CHECK(ui_filament_path_canvas_flow_reversed(canvas));

    // Leaving the panel stops every animation, the manual stream included.
    ui_filament_path_canvas_stop_animations(canvas);
    CHECK(ui_filament_path_canvas_get_manual_motion(canvas) == 0);
    CHECK_FALSE(ui_filament_path_canvas_is_animating(canvas));

    lv_subject_set_int(&subj, static_cast<int>(FilamentMotion::Idle));
    process_lvgl(20);
    lv_obj_delete(canvas);
    lv_subject_deinit(&subj);
}
