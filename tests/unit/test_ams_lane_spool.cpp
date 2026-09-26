// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Rendering contract of the ams_lane_spool widget: the spool graphic, the
// empty-lane placeholder, the fill level and the error dot all render from
// AmsState's per-slot subjects. The lane classification itself is AmsState's
// classify_lane() (test_ams_lane_state*.cpp); these tests pin what the widget
// does with each classification.

#include "ui_ams_lane_spool.h"
#include "ui_ams_slot.h"
#include "ui_spool_canvas.h"

#include "../test_fixtures.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_lane_state.h"
#include "ams_state.h"
#include "ams_types.h"
#include "config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "ui/ams_drawing_utils.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

lv_obj_t* make_spool(lv_obj_t* parent, int slot_index) {
    const std::string idx = std::to_string(slot_index);
    const char* attrs[] = {"slot_index", idx.c_str(), nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_lane_spool", attrs));
}

lv_obj_t* part(lv_obj_t* root, const char* name) {
    lv_obj_t* o = lv_obj_find_by_name(root, name);
    REQUIRE(o != nullptr);
    return o;
}

/// The error dot is the one root child that is neither the spool graphic nor
/// the empty placeholder (it is unnamed by the drawing utils).
lv_obj_t* find_error_dot(lv_obj_t* root) {
    lv_obj_t* graphic = lv_obj_find_by_name(root, "spool_graphic");
    lv_obj_t* placeholder = lv_obj_find_by_name(root, "empty_placeholder");
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* child = lv_obj_get_child(root, i);
        if (child && child != graphic && child != placeholder) {
            return child;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "ams_lane_spool: an Empty lane hides the spool, shows the placeholder",
                 "[ams][lane_spool]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Empty));

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);

    CHECK(lv_obj_has_flag(part(spool, "spool_graphic"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(part(spool, "empty_placeholder"), LV_OBJ_FLAG_HIDDEN));
    lv_obj_delete(spool);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: a Ghosted lane keeps the spool, dimmed",
                 "[ams][lane_spool]") {
    // Both halves asserted: the spool stays visible (assigned, not present)
    // AND it renders at the ghost opacity, not full strength.
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Ghosted));

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);

    lv_obj_t* graphic = part(spool, "spool_graphic");
    CHECK_FALSE(lv_obj_has_flag(graphic, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_get_style_opa(graphic, LV_PART_MAIN) == ams_draw::GHOST_OPA);
    CHECK(lv_obj_has_flag(part(spool, "empty_placeholder"), LV_OBJ_FLAG_HIDDEN));
    lv_obj_delete(spool);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: a Present lane renders full strength",
                 "[ams][lane_spool]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Present));

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);

    lv_obj_t* graphic = part(spool, "spool_graphic");
    CHECK_FALSE(lv_obj_has_flag(graphic, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_get_style_opa(graphic, LV_PART_MAIN) == LV_OPA_COVER);
    lv_obj_delete(spool);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: fill renders from the subject, -1 means no data",
                 "[ams][lane_spool]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_t* fill = AmsState::instance().get_slot_fill_subject(0);
    REQUIRE(fill != nullptr);
    lv_subject_set_int(fill, 50);

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    CHECK(helix::ui::ams_lane_spool_get_fill_level(spool) == Catch::Approx(0.50f));

    // A later change flows through the observer (deferred, #82).
    lv_subject_set_int(fill, 25);
    process_lvgl(50);
    CHECK(helix::ui::ams_lane_spool_get_fill_level(spool) == Catch::Approx(0.25f));

    // A no-data frame must not blank a lane that already rendered a value.
    lv_subject_set_int(fill, -1);
    process_lvgl(50);
    CHECK(helix::ui::ams_lane_spool_get_fill_level(spool) == Catch::Approx(0.25f));

    // Fill renders RAW in the spool family: 0 paints an empty spool, not the
    // bar family's visible-sliver floor (an empty spool is still a drawn spool).
    lv_subject_set_int(fill, 0);
    process_lvgl(50);
    CHECK(helix::ui::ams_lane_spool_get_fill_level(spool) == Catch::Approx(0.0f));

    lv_obj_delete(spool);
    lv_subject_set_int(fill, -1);
}

TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: color renders from the subject",
                 "[ams][lane_spool]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_color_subject(0), 0xFF0000);

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);

    lv_obj_t* canvas = part(spool, "spool_graphic");
    REQUIRE(lv_obj_check_type(canvas, &lv_canvas_class)); // 3D style default
    CHECK(lv_color_eq(ui_spool_canvas_get_color(canvas), lv_color_hex(0xFF0000)));

    lv_subject_set_int(AmsState::instance().get_slot_color_subject(0), 0x0000FF);
    process_lvgl(50);
    CHECK(lv_color_eq(ui_spool_canvas_get_color(canvas), lv_color_hex(0x0000FF)));
    lv_obj_delete(spool);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "ams_lane_spool: has_error drives the error dot color and visibility",
                 "[ams][lane_spool]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_error_severity_subject(0),
                       static_cast<int>(SlotError::Severity::ERROR));
    lv_subject_set_int(AmsState::instance().get_slot_has_error_subject(0), 1);

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);

    lv_obj_t* dot = find_error_dot(spool);
    REQUIRE(dot != nullptr);
    CHECK_FALSE(lv_obj_has_flag(dot, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(dot, LV_PART_MAIN),
                      ams_draw::severity_color(SlotError::Severity::ERROR)));

    // Clearing has_error hides the dot again.
    lv_subject_set_int(AmsState::instance().get_slot_has_error_subject(0), 0);
    process_lvgl(50);
    CHECK(lv_obj_has_flag(dot, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(spool);
    lv_subject_set_int(AmsState::instance().get_slot_error_severity_subject(0),
                       static_cast<int>(SlotError::Severity::INFO));
}

TEST_CASE_METHOD(XMLTestFixture, "ams_slot embeds ams_lane_spool and forwards a creation-time fill",
                 "[ams][lane_spool]") {
    ui_ams_slot_register();
    AmsState::instance().init_subjects(true);
    // The per-slot fill subject outlives tests; -1 is "no data", which leaves
    // the creation-time attribute standing.
    lv_subject_set_int(AmsState::instance().get_slot_fill_subject(0), -1);

    const char* attrs[] = {"slot_index", "0", "fill_level", "0.25", nullptr};
    auto* slot = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_slot", attrs));
    REQUIRE(slot != nullptr);
    process_lvgl(20);

    // The embedded widget exists inside the slot's spool_container...
    lv_obj_t* spool_container = UITest::find_by_name(slot, "spool_container");
    REQUIRE(spool_container != nullptr);
    lv_obj_t* embedded = lv_obj_find_by_name(spool_container, "lane_spool");
    REQUIRE(embedded != nullptr);

    // ...and the slot's fill_level attribute reached it (a later per-slot fill
    // subject value overrides it, pinned by the fill test above).
    CHECK(helix::ui::ams_lane_spool_get_fill_level(embedded) == Catch::Approx(0.25f));
    CHECK(ui_ams_slot_get_fill_level(slot) == Catch::Approx(0.25f));

    lv_obj_delete(slot);
}

// The flat style's three layers: the filament ring carries the lane color, the
// outer flange is that color darkened, the hub sits on top.
TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: flat style renders concentric rings",
                 "[ams][lane_spool]") {
    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "flat");
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_subject_set_int(AmsState::instance().get_slot_lane_state_subject(0),
                       static_cast<int>(helix::ui::LaneState::Present));
    lv_subject_set_int(AmsState::instance().get_slot_color_subject(0), 0xFF0000);

    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);

    lv_obj_t* ring = part(spool, "spool_graphic");
    CHECK_FALSE(lv_obj_check_type(ring, &lv_canvas_class)); // rings, not the 3D canvas
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(ring, LV_PART_MAIN), lv_color_hex(0xFF0000)));
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(part(spool, "spool_outer"), LV_PART_MAIN),
                      ams_draw::darken_color(lv_color_hex(0xFF0000), 50)));
    CHECK(part(spool, "spool_hub") != nullptr);
    CHECK(lv_obj_has_flag(part(spool, "empty_placeholder"), LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(spool);
    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "3d");
}

// A size change or a spool-style flip rebuilds the visual layers; the same
// size in the same style must not (the strip calls this every rebuild).
TEST_CASE_METHOD(XMLTestFixture, "ams_lane_spool: set_size rebuilds only on a real change",
                 "[ams][lane_spool]") {
    ui_ams_lane_spool_register();
    AmsState::instance().init_subjects(true);
    lv_obj_t* spool = make_spool(test_screen(), 0);
    REQUIRE(spool != nullptr);
    process_lvgl(20);
    lv_obj_t* g1 = part(spool, "spool_graphic");
    // The canvas' max width is the size it was built at; step off it by a
    // delta rather than a constant so the case cannot collide with the
    // theme's default.
    const int32_t size1 = lv_obj_get_style_max_width(g1, LV_PART_MAIN);
    const int32_t size2 = size1 + 16;

    helix::ui::ams_lane_spool_set_size(spool, size2);
    process_lvgl(20);
    lv_obj_t* g2 = part(spool, "spool_graphic");
    CHECK(g2 != g1); // resized -> rebuilt
    CHECK(lv_obj_get_style_max_width(g2, LV_PART_MAIN) == size2);

    helix::ui::ams_lane_spool_set_size(spool, size2);
    process_lvgl(20);
    CHECK(part(spool, "spool_graphic") == g2); // same size + style -> no rebuild

    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "flat");
    helix::ui::ams_lane_spool_set_size(spool, size2);
    process_lvgl(20);
    lv_obj_t* g3 = part(spool, "spool_graphic");
    CHECK(g3 != g2); // style flip -> rebuilt even at the same size
    CHECK_FALSE(lv_obj_check_type(g3, &lv_canvas_class));

    lv_obj_delete(spool);
    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "3d");
}

// The spool family's shared material-label rule (ams_slot and the strip's
// spool cells both render through it).
TEST_CASE("lane_material_text: the spool family's label rule", "[ams][lane_spool]") {
    using helix::ui::LaneState;
    CHECK(std::string(helix::ui::lane_material_text(LaneState::Empty, "PLA")) ==
          std::string(lv_tr("Empty")));
    CHECK(std::string(helix::ui::lane_material_text(LaneState::Ghosted, "")) == "--");
    CHECK(std::string(helix::ui::lane_material_text(LaneState::Present, "")) == "--");
    CHECK(std::string(helix::ui::lane_material_text(LaneState::Present, nullptr)) == "--");
    CHECK(std::string(helix::ui::lane_material_text(LaneState::Present, "PLA")) == "PLA");
}
