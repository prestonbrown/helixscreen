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

TEST_CASE("unit art: an empty bay shows no filament; black filament is still filament",
          "[ams][unit-art]") {
    CHECK_FALSE(unit_art_lane(slot(SlotStatus::EMPTY, 0x123456)).filled);
    CHECK_FALSE(unit_art_lane(slot(SlotStatus::UNKNOWN, 0x123456)).filled);
    const UnitArtLane lane = unit_art_lane(slot(SlotStatus::LOADED, 0x000000));
    CHECK((lane.filled && lane.color == 0x000000));
}

TEST_CASE("unit art: drawn only at the lane counts its art is baked in", "[ams][unit-art]") {
    CHECK(unit_art_available(ace_system(), 0));
    CHECK_FALSE(unit_art_available(ace_system(6), 0));
    CHECK_FALSE(unit_art_available(ace_system(), 3));
    for (int lanes : {1, 2, 3, 4, 5, 6})
        CHECK(unit_art_available(quattro_system(lanes), 0) ==
              (lanes == 2 || lanes == 4 || lanes == 5));
    for (int gates : {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14})
        CHECK(unit_art_available(rail_system("MMU", "ERCF", gates), 0) ==
              (gates == 4 || gates == 6 || gates == 8 || gates == 9 || gates == 12));
}

TEST_CASE("unit art: a disconnected unit keeps its drawing; units on one rig resolve alone",
          "[ams][unit-art]") {
    AmsSystemInfo info = ace_system();
    info.units[0].connected = false;
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

TEST_CASE("unit art glow: only a drying unit on a capable board glows", "[ams][unit-art][glow]") {
    CHECK(unit_art_glow_for(true, true, false) == UnitArtGlow::Pulsing);
    CHECK(unit_art_glow_for(true, false, false) == UnitArtGlow::Steady);
    CHECK(unit_art_glow_for(false, true, false) == UnitArtGlow::Off);
    CHECK(unit_art_glow_for(true, true, true) == UnitArtGlow::Off);
    CHECK(unit_art_glow_for(true, false, true) == UnitArtGlow::Off);
}

TEST_CASE("unit art glow: the chamber lights around the lanes, not the case",
          "[ams][unit-art][glow]") {
    constexpr int kSide = 48;
    const auto at = [](const std::vector<uint8_t>& px, int x, int y) {
        return px[static_cast<size_t>(y * kSide + x)];
    };
    std::vector<uint8_t> lanes(kSide * kSide, 0);
    for (int y = 12; y < 24; ++y)
        for (int x = 12; x < 36; ++x)
            lanes[static_cast<size_t>(y * kSide + x)] = 255;
    std::vector<uint8_t> front(kSide * kSide, 0);

    const auto open = unit_art_chamber_mask(lanes, front, kSide, kSide);
    REQUIRE(open.size() == lanes.size());
    CHECK(at(open, 24, 18) == 255);
    CHECK(at(open, 24, 25) > 0);  // spreads a little past the lane
    CHECK(at(open, 24, 40) == 0); // and no further

    for (int y = 16; y < 24; ++y)
        for (int x = 0; x < kSide; ++x)
            front[static_cast<size_t>(y * kSide + x)] = y < 20 ? 64 : 255; // glass, then case
    const auto shaded = unit_art_chamber_mask(lanes, front, kSide, kSide);
    CHECK(at(shaded, 24, 14) == 255);
    CHECK(at(shaded, 24, 18) == 191);
    CHECK(at(shaded, 24, 22) == 0);

    CHECK(unit_art_chamber_mask(lanes, front, kSide, kSide + 1).empty());
}

TEST_CASE("unit art glow: strength stays inside the drawing and under its peak",
          "[ams][unit-art][glow]") {
    CHECK(unit_art_glow_alpha(0, 255) == 0);
    CHECK(unit_art_glow_alpha(255, 0) == 0);
    CHECK(unit_art_glow_alpha(255, 255) == 150);
    CHECK(unit_art_glow_alpha(128, 255) < 150);
}

TEST_CASE_METHOD(LVGLTestFixture, "unit art glow: lights only part of the drawing",
                 "[ams][unit-art][glow]") {
    lv_obj_t* art = unit_art_create(test_screen(), UnitArtTier::Full);
    lv_obj_set_size(art, 120, 120);
    unit_art_set_unit(art, ace_system(), 0);
    REQUIRE(unit_art_is_flattened(art));
    unit_art_set_glow(art, UnitArtGlow::Steady);
    REQUIRE(unit_art_glow_obj(art) != nullptr);
    lv_obj_t* flat = lv_obj_get_child(art, 1);
    const auto* flat_buf = static_cast<const lv_draw_buf_t*>(lv_image_get_src(flat));
    const auto* glow_buf =
        static_cast<const lv_draw_buf_t*>(lv_image_get_src(unit_art_glow_obj(art)));
    int drawn = 0;
    int lit = 0;
    for (int y = 0; y < 120; ++y) {
        const auto* f = static_cast<const uint8_t*>(lv_draw_buf_goto_xy(flat_buf, 0, y));
        const auto* g = static_cast<const uint8_t*>(lv_draw_buf_goto_xy(glow_buf, 0, y));
        for (int x = 0; x < 120; ++x) {
            drawn += f[x * 4 + 3] > 200 ? 1 : 0;
            lit += g[x * 4 + 3] > 0 ? 1 : 0;
        }
    }
    REQUIRE(drawn > 0);
    CHECK(lit > 0);
    CAPTURE(drawn, lit);
    CHECK(lit * 100 < drawn * 70);
}

TEST_CASE_METHOD(LVGLTestFixture, "unit art glow: pulses, holds steady, and stops",
                 "[ams][unit-art][glow]") {
    lv_obj_t* art = unit_art_create(test_screen(), UnitArtTier::Thumb);
    lv_obj_set_size(art, 40, 40);
    unit_art_set_unit(art, ace_system(), 0);
    REQUIRE(unit_art_is_flattened(art));

    unit_art_set_glow(art, UnitArtGlow::Pulsing);
    lv_obj_t* glow = unit_art_glow_obj(art);
    REQUIRE(glow != nullptr);
    CHECK_FALSE(lv_obj_is_hidden(glow));
    lv_anim_t* pulse = lv_anim_get(glow, nullptr);
    REQUIRE(pulse != nullptr);
    CHECK(pulse->start_value < pulse->end_value);
    CHECK(pulse->repeat_cnt == LV_ANIM_REPEAT_INFINITE);
    pulse->act_time = 77;
    unit_art_set_glow(art, UnitArtGlow::Pulsing); // every refresh repeats this
    CHECK(lv_anim_get(glow, nullptr)->act_time == 77);

    unit_art_set_glow(art, UnitArtGlow::Steady);
    CHECK(lv_anim_get(glow, nullptr) == nullptr);
    CHECK_FALSE(lv_obj_is_hidden(glow));
    CHECK(lv_obj_get_style_image_opa(glow, LV_PART_MAIN) == LV_OPA_COVER);

    unit_art_set_glow(art, UnitArtGlow::Pulsing);
    unit_art_set_glow(art, UnitArtGlow::Off);
    CHECK(lv_anim_get(glow, nullptr) == nullptr);
    CHECK(lv_obj_is_hidden(glow));
}

TEST_CASE_METHOD(LVGLTestFixture, "unit art glow: a new drawing gets a new glow",
                 "[ams][unit-art][glow]") {
    lv_obj_t* art = unit_art_create(test_screen(), UnitArtTier::Thumb);
    lv_obj_set_size(art, 40, 40);
    AmsSystemInfo info = ace_system();
    unit_art_set_unit(art, info, 0);
    unit_art_set_glow(art, UnitArtGlow::Pulsing);
    lv_obj_t* glow = unit_art_glow_obj(art);
    REQUIRE(glow != nullptr);
    const void* before = lv_image_get_src(glow);

    info.units[0].slots[0].color_rgb = 0x00ff00;
    unit_art_set_unit(art, info, 0);
    unit_art_set_glow(art, UnitArtGlow::Pulsing);
    CHECK(lv_image_get_src(glow) != before);
    CHECK_FALSE(lv_obj_is_hidden(glow));

    unit_art_set_unit(art, quattro_system(4), 0); // another shell rebuilds every child
    CHECK(unit_art_glow_obj(art) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "unit art glow: its pulse dies with the widget",
                 "[ams][unit-art][glow]") {
    lv_obj_t* art = unit_art_create(test_screen(), UnitArtTier::Thumb);
    lv_obj_set_size(art, 40, 40);
    unit_art_set_unit(art, ace_system(), 0);
    unit_art_set_glow(art, UnitArtGlow::Pulsing);
    lv_obj_t* glow = unit_art_glow_obj(art);
    REQUIRE(lv_anim_get(glow, nullptr) != nullptr);
    lv_obj_delete(art);
    CHECK(lv_anim_get(glow, nullptr) == nullptr);
}
