// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_types.h"
#include "ams_unit_shell.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::tray::unit_shell_for;
using helix::ui::tray::UnitShell;

namespace {

AmsSystemInfo one_unit(AmsType type, const std::string& unit_name) {
    AmsSystemInfo info;
    info.type = type;
    AmsUnit unit;
    unit.name = unit_name;
    unit.slot_count = 4;
    info.units.push_back(unit);
    return info;
}

} // namespace

TEST_CASE("unit shell: an ACE resolves to its shell", "[ams][unit-art]") {
    const UnitShell* shell = unit_shell_for(one_unit(AmsType::ACE, "ACE 1"), 0);
    REQUIRE(shell != nullptr);
    CHECK(std::string(shell->id) == "ace_pro");
}

TEST_CASE("unit shell: a Box Turtle is found by its unit's name", "[ams][unit-art]") {
    const UnitShell* shell = unit_shell_for(one_unit(AmsType::AFC, "Box_Turtle Turtle_1"), 0);
    REQUIRE(shell != nullptr);
    CHECK(std::string(shell->id) == "box_turtle");
    CHECK(shell->dome_n == 0);
    CHECK(std::string(unit_shell_for(one_unit(AmsType::AFC, "Box_Turtle Owl_1"), 0)->id) ==
          "box_turtle");
}

TEST_CASE("unit shell: climate data means the enclosed Box Turtle", "[ams][unit-art]") {
    AmsSystemInfo info = one_unit(AmsType::AFC, "Box_Turtle Turtle_1");
    CHECK(std::string(unit_shell_for(info, 0)->id) == "box_turtle");
    CHECK(std::string(unit_shell_for(info, 0, /*has_dryer=*/true)->id) == "box_turtle_enclosed");
    info.units[0].environment = EnvironmentData{};
    CHECK(std::string(unit_shell_for(info, 0)->id) == "box_turtle_enclosed");
    // The ACE has one body whatever it reports.
    CHECK(std::string(unit_shell_for(one_unit(AmsType::ACE, "ACE 1"), 0, true)->id) == "ace_pro");
}

TEST_CASE("unit shell: each identified unit finds its own drawing", "[ams][unit-art]") {
    const auto id = [](AmsType type, const char* name) {
        const UnitShell* shell = unit_shell_for(one_unit(type, name), 0);
        REQUIRE(shell != nullptr);
        return std::string(shell->id);
    };
    CHECK(id(AmsType::CFS, "CFS 1") == "cfs");
    CHECK(id(AmsType::QIDI_BOX, "Box 1") == "qidi_box");
    CHECK(id(AmsType::AFC, "ViViD_1") == "btt_vivid");
    CHECK(id(AmsType::AFC, "ViViD turtle_2") == "btt_vivid");
    CHECK(id(AmsType::AFC, "Night_Owl Owl_1") == "night_owl");
    CHECK(id(AmsType::AFC, "Night_Owl Turtle_1") == "night_owl");
    CHECK(id(AmsType::OPENAMS, "oams1") == "ams_2_pro");
    AmsSystemInfo single = one_unit(AmsType::OPENAMS, "oams1");
    single.units[0].slot_count = 1;
    REQUIRE(unit_shell_for(single, 0) != nullptr);
    CHECK(std::string(unit_shell_for(single, 0)->id) == "ams_ht");
}

TEST_CASE("unit shell: unidentified hardware keeps the generic tray", "[ams][unit-art]") {
    CHECK(unit_shell_for(one_unit(AmsType::HAPPY_HARE, "MMU"), 0) == nullptr);
    CHECK(unit_shell_for(one_unit(AmsType::ACE, "ACE 1"), 5) == nullptr);
    // Drawn, but nothing a BMCU reports identifies it.
    CHECK(unit_shell_for(one_unit(AmsType::AFC, "BMCU 1"), 0) == nullptr);
    const UnitShell* bmcu = helix::ui::tray::unit_shell_by_id("bmcu");
    REQUIRE(bmcu != nullptr);
    CHECK(bmcu->art_lanes == 4);
    CHECK_FALSE(bmcu->built_in_selector);
    const UnitShell* owl = helix::ui::tray::unit_shell_by_id("night_owl");
    REQUIRE(owl != nullptr);
    CHECK(owl->art_lanes == 2);
    CHECK(owl->built_in_selector);
}

TEST_CASE("unit shell: the cheek only climbs toward the back", "[ams][unit-art]") {
    const UnitShell* shell = unit_shell_for(one_unit(AmsType::ACE, "ACE 1"), 0);
    REQUIRE(shell != nullptr);
    REQUIRE(shell->cheek_n >= 2);
    CHECK(shell->cheek[0].height_px == 0.0f);
    for (int i = 1; i < shell->cheek_n; ++i) {
        CHECK(shell->cheek[i].depth_frac > shell->cheek[i - 1].depth_frac);
        CHECK(shell->cheek[i].height_px >= shell->cheek[i - 1].height_px);
    }
    CHECK(shell->cheek[shell->cheek_n - 1].height_px == Catch::Approx(shell->back_extra_px));
}

TEST_CASE("unit shell: the dome starts and ends on the cheek", "[ams][unit-art]") {
    const UnitShell* shell = unit_shell_for(one_unit(AmsType::ACE, "ACE 1"), 0);
    REQUIRE(shell != nullptr);
    REQUIRE(shell->dome_n >= 3);
    CHECK(shell->dome[0].height_px == Catch::Approx(0.0f).margin(0.01));
    const auto& last = shell->dome[shell->dome_n - 1];
    CHECK(last.depth_frac == Catch::Approx(1.0f));
    CHECK(last.height_px == Catch::Approx(shell->cheek[shell->cheek_n - 1].height_px).margin(0.01));
    float crest = 0;
    for (int i = 0; i < shell->dome_n; ++i)
        crest = std::max(crest, shell->dome[i].height_px);
    CHECK(crest > shell->back_extra_px);
}
