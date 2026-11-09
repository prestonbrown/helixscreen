// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_unit_art.h"

#include "../lvgl_test_fixture.h"
#include "ams_types.h"
#include "ams_unit_shell.h"
#include "display_settings_manager.h"

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

AmsSystemInfo rail_system(const char* name, const char* model, int gates,
                          PathTopology topology = PathTopology::LINEAR) {
    AmsSystemInfo info;
    info.type = AmsType::HAPPY_HARE;
    AmsUnit unit;
    unit.name = name;
    unit.model = model;
    unit.slot_count = gates;
    unit.topology = topology;
    for (int i = 0; i < gates; ++i)
        unit.slots.push_back(slot(SlotStatus::AVAILABLE, 0x00ff00));
    info.units.push_back(unit);
    return info;
}

const char* shell_id(const AmsSystemInfo& info) {
    const tray::UnitShell* shell = tray::unit_shell_for(info, 0);
    return shell ? shell->id : "";
}

} // namespace

TEST_CASE("unit art: a selector rail is known by its vendor, on a selector backend",
          "[ams][unit-art]") {
    CHECK(std::string(shell_id(rail_system("MMU", "ERCF", 9))) == "ercf_rail");
    CHECK(std::string(shell_id(rail_system("MMU", "Tradrack", 12))) == "tradrack_rail");
    CHECK(std::string(shell_id(rail_system("ERCF", "", 6))) == "ercf_rail");
    // A vendor string on a hub-topology unit is not a rail.
    CHECK(std::string(shell_id(rail_system("MMU", "ERCF", 6, PathTopology::HUB))) != "ercf_rail");
    CHECK(std::string(shell_id(rail_system("MMU", "Other", 6))).empty());
    CHECK(std::string(shell_id(rail_system("MMU", "", 6))).empty());
}

TEST_CASE("unit art: a rail has art only for the gate counts it is baked in", "[ams][unit-art]") {
    for (int gates : {4, 6, 8, 9, 12})
        CHECK(unit_art_available(rail_system("MMU", "ERCF", gates), 0));
    for (int gates : {3, 5, 7, 10, 11, 14})
        CHECK_FALSE(unit_art_available(rail_system("MMU", "ERCF", gates), 0));
}

TEST_CASE("unit art: the carriage offset scales with the art's displayed width",
          "[ams][unit-art]") {
    const tray::CarriageTrack track{6, 0.1f, 0.05f, 2.0f};
    // Width-limited: a 100 x 100 box shows the 2:1 art 100 wide.
    auto wide = tray::carriage_offset(track, 3, 100, 100);
    CHECK(wide.x == 30);
    CHECK(wide.y == 15);
    // Height-limited: a 400 x 50 box shows the art 100 wide as well.
    auto tall = tray::carriage_offset(track, 2, 400, 50);
    CHECK(tall.x == 20);
    CHECK(tall.y == 10);
    CHECK(tray::carriage_offset(track, 0, 100, 100).x == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "unit art: the carriage goes to the active gate",
                 "[ams][unit-art]") {
    auto& settings = DisplaySettingsManager::instance();
    const bool restore = settings.get_animations_enabled();
    lv_obj_t* art = unit_art_create(test_screen(), UnitArtTier::Full);
    lv_obj_set_size(art, 300, 200);
    lv_obj_update_layout(art);
    AmsSystemInfo info = rail_system("MMU", "ERCF", 6);
    info.current_slot = 3;

    const tray::UnitShell* shell = tray::unit_shell_for(info, 0);
    REQUIRE(shell != nullptr);
    const tray::CarriageTrack* track = tray::unit_shell_carriage(*shell, 6);
    REQUIRE(track != nullptr);
    const auto gate3 = tray::carriage_offset(*track, 3, 300, 200);
    const auto gate5 = tray::carriage_offset(*track, 5, 300, 200);
    REQUIRE(gate3.x != gate5.x);

    auto translate_x = [&] {
        return lv_obj_get_style_translate_x(unit_art_carriage_obj(art), LV_PART_MAIN);
    };

    settings.set_animations_enabled(false);
    unit_art_set_unit(art, info, 0);
    process_lvgl(20);
    CHECK(unit_art_is_flattened(art));
    CHECK(translate_x() == gate3.x);

    info.current_slot = 5;
    unit_art_set_unit(art, info, 0);
    CHECK(translate_x() == gate5.x); // animations off: there at once

    // Nothing loaded: the selector stays where it was.
    info.current_slot = -1;
    unit_art_set_unit(art, info, 0);
    CHECK(translate_x() == gate5.x);

    settings.set_animations_enabled(true);
    info.current_slot = 3;
    unit_art_set_unit(art, info, 0);
    CHECK(translate_x() == gate5.x); // the slide starts where the carriage is
    process_lvgl(1200);
    CHECK(translate_x() == gate3.x);
    settings.set_animations_enabled(restore);
}

TEST_CASE_METHOD(LVGLTestFixture, "unit art: a unit without a selector has no carriage",
                 "[ams][unit-art]") {
    lv_obj_t* art = unit_art_create(test_screen(), UnitArtTier::Full);
    lv_obj_set_size(art, 200, 200);
    lv_obj_update_layout(art);
    unit_art_set_unit(art, ace_system(), 0);
    CHECK(unit_art_carriage_obj(art) == nullptr);
}

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
