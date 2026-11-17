// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_types.h"
#include "ams_unit_shell.h"

#include <string>

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

TEST_CASE("unit shell: a Box Turtle is found by its name; climate data means the enclosed one",
          "[ams][unit-art]") {
    AmsSystemInfo info = one_unit(AmsType::AFC, "Box_Turtle Turtle_1");
    CHECK(std::string(unit_shell_for(info, 0)->id) == "box_turtle");
    CHECK(std::string(unit_shell_for(one_unit(AmsType::AFC, "Box_Turtle Owl_1"), 0)->id) ==
          "box_turtle");
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

TEST_CASE("unit shell: a Quattro Box or an EMU is found through both of its firmwares",
          "[ams][unit-art]") {
    const auto id = [](AmsType type, const char* name) {
        const UnitShell* shell = unit_shell_for(one_unit(type, name), 0);
        return std::string(shell ? shell->id : "none");
    };
    CHECK(id(AmsType::AFC, "Quattro_Box QuattroBox_1") == "quattro_box");
    // AFC names read "<type> <instance>", and the instance is the user's.
    CHECK(id(AmsType::AFC, "Quattro_Box Turtle_1") == "quattro_box");
    CHECK(id(AmsType::HAPPY_HARE, "QuattroBox") == "quattro_box");
    CHECK(id(AmsType::HAPPY_HARE, "Quattro Box 2") == "quattro_box");
    CHECK(id(AmsType::HAPPY_HARE, "ERCF") == "none");
    CHECK(id(AmsType::AFC, "EMU Turtle_1") == "emu");
    CHECK(id(AmsType::HAPPY_HARE, "EMU") == "emu");
}

TEST_CASE("unit shell: a variable unit keeps its shell at every lane count", "[ams][unit-art]") {
    AmsSystemInfo info = one_unit(AmsType::AFC, "Quattro_Box QuattroBox_1");
    for (int lanes : {1, 2, 3, 5, 8}) {
        info.units[0].slot_count = lanes;
        REQUIRE(unit_shell_for(info, 0) != nullptr);
        CHECK(std::string(unit_shell_for(info, 0)->id) == "quattro_box");
    }
}

TEST_CASE("unit shell: unidentified hardware keeps the generic tray", "[ams][unit-art]") {
    CHECK(unit_shell_for(one_unit(AmsType::HAPPY_HARE, "MMU"), 0) == nullptr);
    CHECK(unit_shell_for(one_unit(AmsType::ACE, "ACE 1"), 5) == nullptr);
    const UnitShell* bmcu = helix::ui::tray::unit_shell_by_id("bmcu");
    REQUIRE(bmcu != nullptr);
    CHECK(unit_shell_for(one_unit(AmsType::AFC, "BMCU BMCU_1"), 0) == bmcu);
    CHECK(bmcu->art_lanes == 4);
    const UnitShell* owl = helix::ui::tray::unit_shell_by_id("night_owl");
    REQUIRE(owl != nullptr);
    CHECK(owl->art_lanes == 2);
}

TEST_CASE("unit shell: every unit names where its lanes merge", "[ams][unit-art]") {
    // Preston's table, docs/devel/UNIT_ART.md § "Combiner".
    using helix::ui::tray::Combiner;
    const std::pair<const char*, Combiner> table[] = {
        {"box_turtle", Combiner::Internal},    {"box_turtle_enclosed", Combiner::Internal},
        {"ams_2_pro", Combiner::Internal},     {"cfs", Combiner::Internal},
        {"night_owl", Combiner::Internal},     {"ercf_rail", Combiner::Internal},
        {"tradrack_rail", Combiner::Internal}, {"ace_pro", Combiner::External},
        {"qidi_box", Combiner::External},      {"htlf", Combiner::External},
        {"ad5x_ifs", Combiner::External},      {"elegoo_canvas", Combiner::External},
        {"emu", Combiner::External},           {"btt_vivid", Combiner::External},
        {"bmcu", Combiner::External},          {"mmx", Combiner::External},
        {"3ms", Combiner::External},           {"kms", Combiner::External},
        {"quattro_box", Combiner::External},   {"ams_ht", Combiner::Absent},
    };
    for (const auto& [id, combiner] : table) {
        CAPTURE(id);
        const UnitShell* shell = helix::ui::tray::unit_shell_by_id(id);
        REQUIRE(shell != nullptr);
        CHECK(shell->combiner == combiner);
        CHECK(shell->port_block ==
              (std::string(id) == "ad5x_ifs" || std::string(id) == "elegoo_canvas"));
    }
}

TEST_CASE("unit shell: feeders resolve from what their backends report", "[ams][unit-art]") {
    const auto id = [](AmsType type, const char* name) {
        const UnitShell* shell = unit_shell_for(one_unit(type, name), 0);
        return std::string(shell ? shell->id : "none");
    };
    CHECK(id(AmsType::AD5X_IFS, "IFS") == "ad5x_ifs");
    CHECK(id(AmsType::AFC, "HTLF HTLF_1") == "htlf");
    CHECK(id(AmsType::AFC, "HTLF Turtle_1") == "htlf");
    // AFC names read "<type> <instance>": only the type token identifies an HTLF.
    CHECK(id(AmsType::AFC, "Claymore HTLF_claymore_1") == "none");
    CHECK(id(AmsType::AFC, "Box_Turtle my_htlf") == "box_turtle");
    CHECK(id(AmsType::AFC, "HTLF vivid_1") == "htlf");
    // AFC types an ACE Pro "ACE", an ACE Pro 2 "ACE2" and a CFS "CFS", only as the leading token.
    CHECK(id(AmsType::AFC, "ACE ACE_1") == "ace_pro");
    CHECK(id(AmsType::AFC, "ACE2 Turtle_1") == "ace_pro");
    CHECK(id(AmsType::AFC, "CFS CFS_1") == "cfs");
    CHECK(id(AmsType::AFC, "Claymore ace 2") == "none");
    CHECK(id(AmsType::HAPPY_HARE, "MMX") == "mmx");
    CHECK(id(AmsType::HAPPY_HARE, "3MS") == "3ms");
    // The Canvas is drawn, but no backend reports one.
    CHECK(helix::ui::tray::unit_shell_by_id("elegoo_canvas") != nullptr);
}

TEST_CASE("unit shell: a Happy Hare unit is found by the vendor it reports", "[ams][unit-art]") {
    // Vendor strings from Happy Hare's mmu_constants.py, with the display name each unit
    // gets when its config sets none.
    struct Case {
        const char* vendor;
        const char* default_name;
        const char* shell;
    };
    const Case cases[] = {
        {"KMS", "KMS", "kms"},
        {"VVD", "BTT ViViD", "btt_vivid"},
        {"QIDI", "QIDI Box", "qidi_box"},
        {"HTLF", "HTLF", "htlf"},
        {"NightOwl", "Night Owl", "night_owl"},
        {"BoxTurtle", "Box Turtle", "box_turtle"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.vendor);
        AmsSystemInfo info = one_unit(AmsType::HAPPY_HARE, c.default_name);
        info.units[0].model = c.vendor;
        REQUIRE(unit_shell_for(info, 0) != nullptr);
        CHECK(std::string(unit_shell_for(info, 0)->id) == c.shell);
        info.units[0].name = "Dry box"; // renamed, still reporting its vendor
        REQUIRE(unit_shell_for(info, 0) != nullptr);
        CHECK(std::string(unit_shell_for(info, 0)->id) == c.shell);
    }
    AmsSystemInfo turtle = one_unit(AmsType::HAPPY_HARE, "Box Turtle");
    turtle.units[0].model = "BoxTurtle";
    CHECK(std::string(unit_shell_for(turtle, 0, /*has_dryer=*/true)->id) == "box_turtle_enclosed");
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
