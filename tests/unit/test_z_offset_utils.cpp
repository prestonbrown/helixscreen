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

TEST_CASE("displayed z-offset: a per-head offset shows the mounted head's live value",
          "[zoffset][display][per_head]") {
    // A head changer whose firmware applies each head's own offset at pickup:
    // the stored machine-wide value belongs to no head in particular, and the
    // live +3.413 is the mounted head's real offset.
    CHECK(displayed_z_offset_microns(3413, std::optional<int>(-150), /*print_active=*/false,
                                     /*per_head=*/true) == 3413);
    CHECK(displayed_z_offset_microns(3413, std::optional<int>(-150), /*print_active=*/true,
                                     /*per_head=*/true) == 3413);
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

TEST_CASE("z-adjust gcode: a per-head offset is only ever adjusted relatively",
          "[zoffset][adjust][per_head]") {
    // An absolute Z= sets whatever head is mounted to a number that belongs to
    // no head (a -0.150 over a live +3.413 drops the nozzle 3.5mm), and the
    // firmware saves only a Z_ADJUST against the mounted head.
    CHECK(build_z_adjust_gcode(-150, 3413, -10, /*all_homed=*/true, /*per_head=*/true) ==
          "SET_GCODE_OFFSET Z_ADJUST=-0.010 MOVE=1");
    CHECK(build_z_adjust_gcode(-150, 3413, 50, /*all_homed=*/false, /*per_head=*/true) ==
          "SET_GCODE_OFFSET Z_ADJUST=0.050");
    CHECK(build_z_adjust_gcode(3413, 3413, -10, /*all_homed=*/true, /*per_head=*/true) ==
          "SET_GCODE_OFFSET Z_ADJUST=-0.010 MOVE=1");
}

// ============================================================================
// save_available: the one definition of "is there an offset worth saving"
// ============================================================================

namespace {
constexpr SaveAvailability facts(bool supported, bool global_dirty, bool tools_dirty) {
    return SaveAvailability{supported, global_dirty, tools_dirty};
}
} // namespace

TEST_CASE("save_available: nothing dirty offers no save", "[zoffset][save-rule]") {
    CHECK_FALSE(save_available(facts(true, false, false)));
}

TEST_CASE("save_available: the machine-wide offset alone is enough", "[zoffset][save-rule]") {
    CHECK(save_available(facts(true, true, false)));
}

TEST_CASE("save_available: a dirty tool alone is enough", "[zoffset][save-rule]") {
    // The case a machine-wide-only test would miss: a tool adjusted while the
    // global offset stayed at 0. SET_TOOL_PARAMETER is runtime-only, so with no
    // save offered the adjustment dies at the next Klipper restart.
    CHECK(save_available(facts(true, false, true)));
}

TEST_CASE("save_available: both dirty still offers one save", "[zoffset][save-rule]") {
    CHECK(save_available(facts(true, true, true)));
}

TEST_CASE("save_available: firmware that persists on its own offers no save",
          "[zoffset][save-rule]") {
    CHECK_FALSE(save_available(facts(false, false, false)));
    CHECK_FALSE(save_available(facts(false, true, false)));
    CHECK_FALSE(save_available(facts(false, false, true)));
    CHECK_FALSE(save_available(facts(false, true, true)));
}

TEST_CASE("save_available: saving turned off for this printer offers no save",
          "[zoffset][save-rule][save-toggle]") {
    SaveAvailability off = facts(true, true, true);
    off.save_enabled = false;
    CHECK_FALSE(save_available(off));
    off.global_dirty = false;
    CHECK_FALSE(save_available(off)); // a dirty tool alone does not bring it back
}

// ============================================================================
// save_enabled: the per-printer "offer Z-offset saves" decision (#1766)
// ============================================================================

TEST_CASE("save_enabled: Auto offers saves on a printer with one offset",
          "[zoffset][save-toggle]") {
    CHECK(save_enabled(SaveMode::Auto, /*per_tool_z_offsets=*/false));
}

TEST_CASE("save_enabled: Auto withholds saves when each tool keeps its own Z offset",
          "[zoffset][save-toggle]") {
    // A machine-wide save folds the live offset into the probe or endstop,
    // under every tool's own offset at once.
    CHECK_FALSE(save_enabled(SaveMode::Auto, /*per_tool_z_offsets=*/true));
}

TEST_CASE("save_enabled: an explicit choice beats the toolchanger default",
          "[zoffset][save-toggle]") {
    CHECK(save_enabled(SaveMode::Show, true));
    CHECK(save_enabled(SaveMode::Show, false));
    CHECK_FALSE(save_enabled(SaveMode::Hide, true));
    CHECK_FALSE(save_enabled(SaveMode::Hide, false));
}

TEST_CASE("save_enabled: an out-of-range stored mode falls back to Auto",
          "[zoffset][save-toggle]") {
    CHECK(save_enabled(static_cast<SaveMode>(7), false));
    CHECK_FALSE(save_enabled(static_cast<SaveMode>(7), true));
}

TEST_CASE("saves_global: the machine-wide offset is written only while saving is on",
          "[zoffset][save-toggle]") {
    SaveAvailability f = facts(true, true, true);
    CHECK(saves_global(f));
    f.save_enabled = false;
    CHECK_FALSE(saves_global(f));
    f.save_enabled = true;
    f.global_dirty = false;
    CHECK_FALSE(saves_global(f));
}

// ============================================================================
// saved_offset_line (#1998)
// ============================================================================

namespace {
helix::zoffset::SaveAvailability manual_saves() {
    helix::zoffset::SaveAvailability facts;
    facts.manual_save_supported = true;
    facts.save_enabled = true;
    return facts;
}
} // namespace

TEST_CASE("saved_offset_line: a save writes the saved value minus the session offset",
          "[zoffset][1998]") {
    using helix::zoffset::saved_offset_line;
    auto line = saved_offset_line(manual_saves(), false, 1250, 20);
    REQUIRE(line);
    CHECK(line->saved_microns == 1250);
    CHECK(line->after_save_microns == 1230);

    line = saved_offset_line(manual_saves(), false, 1250, -30);
    REQUIRE(line);
    CHECK(line->after_save_microns == 1280);

    line = saved_offset_line(manual_saves(), false, 1250, 0);
    REQUIRE(line);
    CHECK(line->after_save_microns == 1250);
}

TEST_CASE("saved_offset_line: no line where it would mislead", "[zoffset][1998]") {
    using helix::zoffset::saved_offset_line;
    auto firmware = manual_saves();
    firmware.manual_save_supported = false;
    CHECK_FALSE(saved_offset_line(firmware, false, 1250, 20));

    auto withheld = manual_saves();
    withheld.save_enabled = false;
    CHECK_FALSE(saved_offset_line(withheld, false, 1250, 20));

    CHECK_FALSE(saved_offset_line(manual_saves(), true, 1250, 20));
    CHECK_FALSE(saved_offset_line(manual_saves(), false, std::nullopt, 20));
}

TEST_CASE("format_saved_offset_line names the projection only while it differs",
          "[zoffset][1998]") {
    using helix::zoffset::format_saved_offset_line;
    CHECK(format_saved_offset_line({1250, 1250}) == "Saved 1.250 mm");
    CHECK(format_saved_offset_line({1250, 1230}) == "Saved 1.250 mm, 1.230 mm after Save");
    CHECK(format_saved_offset_line({-50, 10}) == "Saved -0.050 mm, 0.010 mm after Save");
}
