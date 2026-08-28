// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_heater_display_off_target.cpp
 * @brief helix::ui::temperature::heater_display() with the heater off
 *
 * Run with: ./build/bin/helix-tests "[temp_utils][heater_display]"
 *
 * The no-target branch of src/ui/ui_temperature_utils.cpp:193-197: when nothing
 * is commanded, the reading renders alone ("25°C"), not as a pair against a zero
 * setpoint ("25 / 0°C"). Every heater card and the print-status header route
 * through this one function, so a regression here reads as the printer being
 * told to hold 0°C on every idle screen in the app.
 *
 * The paired branch, the decidegree conversion, and the status/color fields are
 * covered in test_ui_temperature_utils.cpp.
 */

#include "ui_temperature_utils.h"

#include "../catch_amalgamated.hpp"

using helix::ui::temperature::heater_display;

TEST_CASE("heater_display: a zero target renders the reading alone",
          "[temp_utils][heater_display]") {
    SECTION("cold nozzle, heater off") {
        auto r = heater_display(/*current_deci=*/250, /*target_deci=*/0);
        CHECK(r.temp == "25°C");
        CHECK(r.status == "Off");
        CHECK(r.pct == 0);
    }

    SECTION("hot nozzle cooling down after the print, heater off") {
        // The dangerous read: 180°C with no setpoint must not print "180 / 0°C",
        // which looks like an active command to crash-cool the hotend.
        auto r = heater_display(/*current_deci=*/1800, /*target_deci=*/0);
        CHECK(r.temp == "180°C");
        CHECK(r.status == "Off");
    }

    SECTION("a negative target is off too, not a pair") {
        auto r = heater_display(/*current_deci=*/250, /*target_deci=*/-10);
        CHECK(r.temp == "25°C");
        CHECK(r.status == "Off");
    }
}

TEST_CASE("heater_display: any positive target renders the pair", "[temp_utils][heater_display]") {
    // The boundary itself: 0.1°C is still a commanded target.
    auto r = heater_display(/*current_deci=*/250, /*target_deci=*/1);
    CHECK(r.temp == "25 / 0°C");

    // And the ordinary case, so the branch above is not the only shape asserted.
    auto hot = heater_display(/*current_deci=*/2100, /*target_deci=*/2200);
    CHECK(hot.temp == "210 / 220°C");
}
