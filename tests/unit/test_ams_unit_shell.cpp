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

TEST_CASE("unit shell: unidentified hardware keeps the generic tray", "[ams][unit-art]") {
    CHECK(unit_shell_for(one_unit(AmsType::HAPPY_HARE, "MMU"), 0) == nullptr);
    CHECK(unit_shell_for(one_unit(AmsType::ACE, "ACE 1"), 5) == nullptr);
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
