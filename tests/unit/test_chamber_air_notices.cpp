// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_chamber_air_notices.cpp
 * @brief When the screen tells the user a chamber air accessory was fitted or
 *        removed, and the copy each provider supplies.
 */

#include "ui_chamber_air_notices.h"

#include "chamber_air_modes.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ui::ChamberAirNotice;
using helix::ui::decide_chamber_air_notice;

TEST_CASE("chamber air notices: which notice a presence reading calls for",
          "[chamber_air][notices]") {
    // present, at_startup, first_fit_shown, print_active
    CHECK(decide_chamber_air_notice(true, false, false, false) == ChamberAirNotice::FirstFit);
    // Booting with the accessory already fitted still says so, once.
    CHECK(decide_chamber_air_notice(true, true, false, false) == ChamberAirNotice::FirstFit);
    CHECK(decide_chamber_air_notice(true, true, true, false) == ChamberAirNotice::None);
    CHECK(decide_chamber_air_notice(true, false, true, false) == ChamberAirNotice::None);
    // A first fit during a print waits for the print to end.
    CHECK(decide_chamber_air_notice(true, false, false, true) == ChamberAirNotice::None);
    // Removal is warned every time, mid-print included, but connecting without it is not
    // a removal.
    CHECK(decide_chamber_air_notice(false, false, true, false) == ChamberAirNotice::Removed);
    CHECK(decide_chamber_air_notice(false, false, false, true) == ChamberAirNotice::Removed);
    CHECK(decide_chamber_air_notice(false, true, false, false) == ChamberAirNotice::None);
}

TEST_CASE("chamber air notices: only a detachable accessory has copy", "[chamber_air][notices]") {
    helix::PrinterDiscovery u1;
    u1.parse_objects(nlohmann::json::array({"purifier", "fm175xx_reader", "tool"}));
    const auto copy = helix::chamber_air::accessory_copy(u1);
    REQUIRE(copy.has_value());
    CHECK(std::string(copy->name) == "Top Cover");
    CHECK(copy->suggests_shaper_recalibration);

    helix::PrinterDiscovery c5;
    c5.parse_objects(nlohmann::json::array({"gcode_macro AIR_CIRCULATION_STOP", "extruder"}));
    CHECK_FALSE(helix::chamber_air::accessory_copy(c5).has_value());
}
