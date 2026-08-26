// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "z_offset_utils.h"

#include <cstring>
#include <optional>

#include "../catch_amalgamated.hpp"

using namespace helix::zoffset;
using helix::ZOffsetCalibrationStrategy;

// ============================================================================
// format_delta tests
// ============================================================================

TEST_CASE("format_delta: zero microns produces empty string", "[zoffset][format]") {
    char buf[32] = "garbage";
    format_delta(0, buf, sizeof(buf));
    REQUIRE(buf[0] == '\0');
}

TEST_CASE("format_delta: positive microns formats with plus sign", "[zoffset][format]") {
    char buf[32] = {};
    format_delta(50, buf, sizeof(buf));
    REQUIRE(std::string(buf) == "+0.050mm");
}

TEST_CASE("format_delta: negative microns formats with minus sign", "[zoffset][format]") {
    char buf[32] = {};
    format_delta(-25, buf, sizeof(buf));
    REQUIRE(std::string(buf) == "-0.025mm");
}

TEST_CASE("format_delta: large positive value", "[zoffset][format]") {
    char buf[32] = {};
    format_delta(1500, buf, sizeof(buf));
    REQUIRE(std::string(buf) == "+1.500mm");
}

// ============================================================================
// format_offset tests
// ============================================================================

TEST_CASE("format_offset: zero microns shows +0.000mm", "[zoffset][format]") {
    char buf[32] = {};
    format_offset(0, buf, sizeof(buf));
    REQUIRE(std::string(buf) == "+0.000mm");
}

TEST_CASE("format_offset: positive microns", "[zoffset][format]") {
    char buf[32] = {};
    format_offset(100, buf, sizeof(buf));
    REQUIRE(std::string(buf) == "+0.100mm");
}

TEST_CASE("format_offset: negative microns", "[zoffset][format]") {
    char buf[32] = {};
    format_offset(-250, buf, sizeof(buf));
    REQUIRE(std::string(buf) == "-0.250mm");
}

// ============================================================================
// is_auto_saved tests
// ============================================================================

TEST_CASE("is_auto_saved: FIRMWARE_MANAGED returns true", "[zoffset][strategy]") {
    REQUIRE(is_auto_saved(ZOffsetCalibrationStrategy::FIRMWARE_MANAGED) == true);
}

TEST_CASE("is_auto_saved: PROBE_CALIBRATE returns false", "[zoffset][strategy]") {
    REQUIRE(is_auto_saved(ZOffsetCalibrationStrategy::PROBE_CALIBRATE) == false);
}

TEST_CASE("is_auto_saved: ENDSTOP returns false", "[zoffset][strategy]") {
    REQUIRE(is_auto_saved(ZOffsetCalibrationStrategy::ENDSTOP) == false);
}

// ============================================================================
// displayed_z_offset_microns
//
// Firmware that persists the z-offset itself (ZMOD) zeroes Klipper's live
// gcode_move offset outside a print and re-applies the stored value at
// START_PRINT. So while idle the live value says 0.000 and lies; the stored
// value is what the next print will actually use.
// ============================================================================

TEST_CASE("displayed z-offset: mid-print the live offset wins", "[zoffset][display]") {
    // Baby steps land in gcode_move first, so live is authoritative during a print
    // even when a stale stored value disagrees.
    CHECK(displayed_z_offset_microns(-160, std::optional<int>(-150), /*print_active=*/true) ==
          -160);
}

TEST_CASE("displayed z-offset: idle prefers the persisted value", "[zoffset][display]") {
    // The regression Negan reported: live reads 0 while the stored offset is -0.150.
    CHECK(displayed_z_offset_microns(0, std::optional<int>(-150), /*print_active=*/false) == -150);
}

TEST_CASE("displayed z-offset: idle persisted zero is honored, not treated as missing",
          "[zoffset][display]") {
    // Distinct from the no-value case below: here the firmware really does say 0.
    CHECK(displayed_z_offset_microns(-160, std::optional<int>(0), /*print_active=*/false) == 0);
}

TEST_CASE("displayed z-offset: no persisted value falls back to live", "[zoffset][display]") {
    // Every non-ZMOD printer, and ZMOD before save_variables has been delivered.
    CHECK(displayed_z_offset_microns(-160, std::nullopt, /*print_active=*/false) == -160);
    CHECK(displayed_z_offset_microns(-160, std::nullopt, /*print_active=*/true) == -160);
}

// ============================================================================
// build_z_adjust_gcode
//
// A relative Z_ADJUST is only safe when the base we showed the user IS Klipper's
// live offset. On ZMOD at idle the live offset has been zeroed, so a relative
// nudge would compute from 0 -- and ZMOD's SET_GCODE_OFFSET override persists the
// result, silently discarding the stored offset. Send absolute Z= there.
// ============================================================================

TEST_CASE("z-adjust gcode: relative when the base is the live offset", "[zoffset][adjust]") {
    // Mid-print baby stepping — unchanged historic behavior.
    CHECK(build_z_adjust_gcode(-150, -150, -10, /*all_homed=*/true) ==
          "SET_GCODE_OFFSET Z_ADJUST=-0.010 MOVE=1");
}

TEST_CASE("z-adjust gcode: MOVE=1 only when all axes are homed", "[zoffset][adjust]") {
    // MOVE=1 against an unhomed axis is a Klipper error.
    CHECK(build_z_adjust_gcode(-150, -150, -10, /*all_homed=*/false) ==
          "SET_GCODE_OFFSET Z_ADJUST=-0.010");
    CHECK(build_z_adjust_gcode(-150, 0, -10, /*all_homed=*/false) == "SET_GCODE_OFFSET Z=-0.160");
}

TEST_CASE("z-adjust gcode: absolute when the base is not the live offset", "[zoffset][adjust]") {
    // ZMOD idle: we displayed the stored -0.150, Klipper's live offset is 0.
    // A relative -0.010 would land on -0.010 and persist that. Absolute lands on
    // -0.160 and persists the right thing.
    CHECK(build_z_adjust_gcode(-150, 0, -10, /*all_homed=*/true) ==
          "SET_GCODE_OFFSET Z=-0.160 MOVE=1");
}

TEST_CASE("z-adjust gcode: absolute handles a positive delta and a sign crossing",
          "[zoffset][adjust]") {
    CHECK(build_z_adjust_gcode(-150, 0, 50, /*all_homed=*/true) ==
          "SET_GCODE_OFFSET Z=-0.100 MOVE=1");
    // Crosses zero — must not emit "-0.000" or drop the sign.
    CHECK(build_z_adjust_gcode(-10, 0, 10, /*all_homed=*/true) ==
          "SET_GCODE_OFFSET Z=0.000 MOVE=1");
    CHECK(build_z_adjust_gcode(-10, 0, 60, /*all_homed=*/true) ==
          "SET_GCODE_OFFSET Z=0.050 MOVE=1");
}
