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

#include <cstdlib>

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

/// What the glyph draws down the middle of its filament column.
struct ColumnShot {
    int matching = 0;  ///< opaque pixels within a few shades of the expected color
    int different = 0; ///< opaque pixels clearly another color
};

ColumnShot shoot_column(lv_obj_t* glyph, uint32_t expected) {
    ColumnShot shot;
    lv_draw_buf_t* snap = lv_snapshot_take(glyph, LV_COLOR_FORMAT_ARGB8888);
    REQUIRE(snap != nullptr);
    const uint32_t x = snap->header.w / 2;
    for (uint32_t y = 2; y < snap->header.h / 3; ++y) {
        const uint8_t* p = snap->data + y * snap->header.stride + x * 4;
        if (p[3] < 200)
            continue;
        const int dist = std::abs(int(p[2]) - int((expected >> 16) & 0xFF)) +
                         std::abs(int(p[1]) - int((expected >> 8) & 0xFF)) +
                         std::abs(int(p[0]) - int(expected & 0xFF));
        if (dist < 30)
            ++shot.matching;
        else if (dist > 120)
            ++shot.different;
    }
    lv_draw_buf_destroy(snap);
    return shot;
}

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

TEST_CASE_METHOD(XMLTestFixture, "filament_motion_glyph paints the column in the known color",
                 "[filament-chevrons][widget]") {
    AnimationsOn animations;
    constexpr uint32_t kFilament = 0x12AB34;
    constexpr uint32_t kNeutral = 0xC020C0;
    const char* attrs[] = {"width", "40", "height", "120", nullptr};
    auto* glyph =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "filament_motion_glyph", attrs));
    REQUIRE(glyph != nullptr);
    lv_obj_set_style_bg_color(glyph, lv_color_hex(kNeutral), LV_PART_MAIN);
    lv_obj_set_style_border_color(glyph, lv_color_hex(0x000000), LV_PART_MAIN);
    process_lvgl(20);

    // No known color: the column keeps its neutral style.
    CHECK(shoot_column(glyph, kNeutral).matching > 0);

    helix::ui::filament_motion_glyph_set_color(glyph, kFilament);
    process_lvgl(20);
    const ColumnShot idle = shoot_column(glyph, kFilament);
    CHECK(idle.matching > 0);
    CHECK(shoot_column(glyph, kNeutral).matching == 0);

    // A theme accent equal to the filament must not hide the chevrons.
    lv_obj_set_style_line_color(glyph, lv_color_hex(kFilament), LV_PART_MAIN);
    helix::ui::filament_motion_glyph_set_motion(glyph, FilamentMotion::Extrude);
    process_lvgl(20);
    CHECK(shoot_column(glyph, kFilament).different > idle.different);

    lv_obj_delete(glyph);
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
