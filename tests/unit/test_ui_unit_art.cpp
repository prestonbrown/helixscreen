// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_unit_art.h"

#include "../lvgl_test_fixture.h"
#include "ams_types.h"
#include "ams_unit_shell.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

SlotInfo slot(SlotStatus status, uint32_t rgb) {
    SlotInfo s;
    s.status = status;
    s.color_rgb = rgb;
    return s;
}

AmsSystemInfo ace_system(int lanes = 4) {
    AmsSystemInfo info;
    info.type = AmsType::ACE;
    AmsUnit unit;
    unit.name = "ACE 1";
    unit.slot_count = lanes;
    for (int i = 0; i < lanes; ++i)
        unit.slots.push_back(slot(SlotStatus::AVAILABLE, 0xff0000));
    unit.slots[static_cast<size_t>(lanes - 1)] = slot(SlotStatus::EMPTY, 0xff0000);
    info.units.push_back(unit);
    return info;
}

AmsSystemInfo quattro_system(int lanes) {
    AmsSystemInfo info;
    info.type = AmsType::AFC;
    AmsUnit unit;
    unit.name = "Quattro_Box QuattroBox_1";
    unit.slot_count = lanes;
    for (int i = 0; i < lanes; ++i)
        unit.slots.push_back(slot(SlotStatus::AVAILABLE, 0x00ff00));
    info.units.push_back(unit);
    return info;
}

} // namespace

TEST_CASE("unit art: an empty bay shows no filament", "[ams][unit-art]") {
    CHECK_FALSE(unit_art_lane(slot(SlotStatus::EMPTY, 0x123456)).filled);
    CHECK_FALSE(unit_art_lane(slot(SlotStatus::UNKNOWN, 0x123456)).filled);
}

TEST_CASE("unit art: black filament is still filament", "[ams][unit-art]") {
    const UnitArtLane lane = unit_art_lane(slot(SlotStatus::LOADED, 0x000000));
    CHECK(lane.filled);
    CHECK(lane.color == 0x000000);
}

TEST_CASE("unit art: drawn only where the bays match the art", "[ams][unit-art]") {
    CHECK(unit_art_available(ace_system(), 0));
    CHECK_FALSE(unit_art_available(ace_system(6), 0));
    CHECK_FALSE(unit_art_available(ace_system(), 3));
}

TEST_CASE("unit art: a variable unit has art only at its baked lane counts", "[ams][unit-art]") {
    for (int lanes : {2, 4, 5})
        CHECK(unit_art_available(quattro_system(lanes), 0));
    for (int lanes : {1, 3, 6})
        CHECK_FALSE(unit_art_available(quattro_system(lanes), 0));
}

TEST_CASE("unit art: a disconnected unit keeps its drawing", "[ams][unit-art]") {
    AmsSystemInfo info = ace_system();
    info.units[0].connected = false;
    CHECK(unit_art_available(info, 0));
}

TEST_CASE("unit art: units on one rig resolve on their own", "[ams][unit-art]") {
    AmsSystemInfo info = ace_system();
    AmsUnit other;
    other.name = "ACE 2";
    other.slot_count = 2;
    info.units.push_back(other);
    CHECK(unit_art_available(info, 0));
    CHECK_FALSE(unit_art_available(info, 1));
}

TEST_CASE_METHOD(LVGLTestFixture, "unit art widget: tints filled lanes and hides empty ones",
                 "[ams][unit-art]") {
    lv_obj_t* art = unit_art_create(test_screen(), UnitArtTier::Thumb);
    REQUIRE(art != nullptr);
    unit_art_set_unit(art, ace_system(), 0);

    CHECK_FALSE(lv_obj_is_hidden(art));
    lv_obj_t* first = unit_art_lane_color_obj(art, 0);
    lv_obj_t* last = unit_art_lane_color_obj(art, 3);
    REQUIRE(first != nullptr);
    REQUIRE(last != nullptr);
    CHECK_FALSE(lv_obj_is_hidden(first));
    CHECK(lv_color_to_u32(lv_obj_get_style_image_recolor(first, LV_PART_MAIN)) ==
          lv_color_to_u32(lv_color_hex(0xff0000)));
    CHECK(lv_obj_is_hidden(last));
}

TEST_CASE_METHOD(LVGLTestFixture, "unit art widget: flattens its layers once sized",
                 "[ams][unit-art]") {
    lv_obj_t* art = unit_art_create(test_screen(), UnitArtTier::Thumb);
    lv_obj_set_size(art, 0, 0);
    unit_art_set_unit(art, ace_system(), 0);
    CHECK_FALSE(unit_art_is_flattened(art)); // no size yet: the layers stay in use
    lv_obj_set_size(art, 40, 40);
    unit_art_set_unit(art, ace_system(), 0);
    CHECK(unit_art_is_flattened(art));
    AmsSystemInfo other = ace_system();
    other.units[0].slots[0].color_rgb = 0x00ff00;
    unit_art_set_unit(art, other, 0); // a new colour must flatten again, not keep the old image
    CHECK(unit_art_is_flattened(art));
    CHECK(lv_color_to_u32(
              lv_obj_get_style_image_recolor(unit_art_lane_color_obj(art, 0), LV_PART_MAIN)) ==
          lv_color_to_u32(lv_color_hex(0x00ff00)));
}

TEST_CASE_METHOD(LVGLTestFixture, "unit art widget: hides itself with no art", "[ams][unit-art]") {
    lv_obj_t* art = unit_art_create(test_screen(), UnitArtTier::Full);
    AmsSystemInfo info;
    info.type = AmsType::HAPPY_HARE;
    AmsUnit unit;
    unit.slot_count = 4;
    info.units.push_back(unit);
    unit_art_set_unit(art, info, 0);
    CHECK(lv_obj_is_hidden(art));
}

TEST_CASE_METHOD(LVGLTestFixture, "unit art widget: flattens again at a new size",
                 "[ams][unit-art][overview-tiles]") {
    const AmsSystemInfo info = ace_system();
    lv_obj_t* art = unit_art_create(test_screen(), UnitArtTier::Full);
    lv_obj_set_size(art, 80, 60);
    unit_art_set_unit(art, info, 0);
    REQUIRE(unit_art_is_flattened(art));
    lv_obj_t* flat = lv_obj_get_child(art, 1);
    REQUIRE(lv_image_get_src_width(flat) == 80);

    lv_obj_set_size(art, 120, 90);
    unit_art_set_unit(art, info, 0);

    CHECK(lv_image_get_src_width(flat) == 120);
}

TEST_CASE_METHOD(LVGLTestFixture, "unit art widget: a new lane count builds that count's layers",
                 "[ams][unit-art]") {
    lv_obj_t* art = unit_art_create(test_screen(), UnitArtTier::Thumb);
    unit_art_set_unit(art, quattro_system(5), 0);
    CHECK_FALSE(lv_obj_is_hidden(art));
    REQUIRE(unit_art_lane_color_obj(art, 4) != nullptr);
    const auto* src = static_cast<const char*>(lv_image_get_src(unit_art_lane_color_obj(art, 4)));
    CHECK(std::string(src).find("units/quattro_box/5/thumb_lane4.png") != std::string::npos);

    unit_art_set_unit(art, quattro_system(2), 0);
    CHECK(unit_art_lane_color_obj(art, 1) != nullptr);
    CHECK(unit_art_lane_color_obj(art, 2) == nullptr);

    unit_art_set_unit(art, quattro_system(3), 0);
    CHECK(lv_obj_is_hidden(art));
}

TEST_CASE("unit art: a feeder look comes from its dev switch", "[ams][unit-art]") {
    CHECK(parse_feeder_look("stubs") == FeederLook::Stubs);
    CHECK(parse_feeder_look("spools") == FeederLook::Spools);
    CHECK(parse_feeder_look(nullptr) == FeederLook::Spools);
    CHECK(parse_feeder_look("nonsense") == FeederLook::Spools);
}

TEST_CASE("unit art: only a unit with a stub look loads it", "[ams][unit-art]") {
    const auto* feeder = tray::unit_shell_by_id("mmx");
    const auto* ace = tray::unit_shell_by_id("ace_pro");
    REQUIRE(feeder != nullptr);
    REQUIRE(ace != nullptr);
    CHECK(unit_art_base_stem(*feeder, 4, UnitArtTier::Thumb, FeederLook::Stubs) ==
          "assets/images/units/mmx/stubs/thumb");
    CHECK(unit_art_base_stem(*feeder, 4, UnitArtTier::Full, FeederLook::Spools) ==
          "assets/images/units/mmx/full");
    CHECK(unit_art_base_stem(*ace, 4, UnitArtTier::Full, FeederLook::Stubs) ==
          "assets/images/units/ace_pro/full");
}
