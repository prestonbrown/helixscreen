// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The DPI figures below are measured, not assumed. Each was taken from the
// device over SSH via FBIOGET_VSCREENINFO and then corrected against the panel's
// true physical diagonal, because 6 of 8 devices in the fleet report a physical
// size that is wrong or absent. Notably the Raspberry Pi rig reports 154x86mm —
// the exact active area of the official 7" touchscreen — for a panel that is
// really ~115mm diagonal, so a plausibility check alone would never catch it.

#include "board_identity.h"
#include "display_metrics.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::DisplayMetrics;
using helix::DpiSource;
using helix::PanelSpec;
using helix::board::Board;

namespace {

/// True DPI of every panel in the test fleet, corrected against measured
/// physical diagonals. Current sizing on all of these is known-good, so the
/// scale factor must leave every one of them untouched.
struct FleetPanel {
    const char* name;
    double true_dpi;
};

constexpr FleetPanel kFleet[] = {
    {"CC1 (480x272, 107mm)", 131.0},       {"SonicPad (1024x600, 7in)", 169.5},
    {"CB1/HDMI5 (800x480, 127mm)", 186.7}, {"Pi (800x480, 115mm)", 206.1},
    {"K1C (480x800, 108.6mm)", 218.2},     {"K2 Plus (480x800, 108.6mm)", 218.2},
    {"AD5M (800x480, 107mm)", 221.5},
};

/// The case that motivated the work: 1080x2400 phone at ~405 DPI.
constexpr double kPhoneDpi = 405.0;

/// GridLayout::GRID_CELL XXLarge track edge is 96; a cell is 2 tracks.
constexpr int32_t kXXLargeCellPx = 192;

} // namespace

// ============================================================================
// No-regression guarantee — the whole point of the deadband
// ============================================================================

TEST_CASE("every measured fleet panel keeps scale 1.0", "[display_metrics][dpi]") {
    for (const auto& p : kFleet) {
        INFO("panel: " << p.name << " at " << p.true_dpi << " DPI");
        REQUIRE(DisplayMetrics::ui_scale_for_dpi(p.true_dpi) == Catch::Approx(1.0));
    }
}

TEST_CASE("scale never shrinks a low-DPI panel", "[display_metrics][dpi]") {
    // A 1.0 floor, not a proportional shrink. The CC1 at 131 DPI is the lowest
    // real panel and must not lose size.
    for (double dpi : {40.0, 96.0, 120.0, 131.0, 160.0, 200.0, 225.0}) {
        INFO("dpi: " << dpi);
        REQUIRE(DisplayMetrics::ui_scale_for_dpi(dpi) == Catch::Approx(1.0));
    }
}

TEST_CASE("deadband ends just above the highest verified-good panel", "[display_metrics][dpi]") {
    // AD5M measures 221.5 and is known-good, so it must be inside the band.
    REQUIRE(DisplayMetrics::ui_scale_for_dpi(221.5) == Catch::Approx(1.0));
    REQUIRE(DisplayMetrics::kScaleDeadbandDpi > 221.5);
    // And the band must actually end, or high-DPI panels are never corrected.
    REQUIRE(DisplayMetrics::ui_scale_for_dpi(300.0) > 1.0);
}

// ============================================================================
// The high-DPI correction actually lands on target
// ============================================================================

TEST_CASE("405 DPI phone reaches the intended physical cell size", "[display_metrics][dpi]") {
    const double scale = DisplayMetrics::ui_scale_for_dpi(kPhoneDpi);

    // Target is ~1.578x, turning the 192px XXLarge cell into ~303px.
    REQUIRE(scale == Catch::Approx(1.578).margin(0.01));

    const int32_t cell = DisplayMetrics::scaled_px(kXXLargeCellPx, scale);
    REQUIRE(cell >= 300);
    REQUIRE(cell <= 306);
}

TEST_CASE("405 DPI phone body font grows into readable range", "[display_metrics][dpi]") {
    // 40px body font at 405 DPI measures ~2.5mm cap height; scaled it clears 60px.
    const double scale = DisplayMetrics::ui_scale_for_dpi(kPhoneDpi);
    REQUIRE(DisplayMetrics::scaled_px(40, scale) >= 60);
}

TEST_CASE("scale is monotonic and capped", "[display_metrics][dpi]") {
    double prev = 0.0;
    for (double dpi = 40.0; dpi <= 700.0; dpi += 5.0) {
        const double s = DisplayMetrics::ui_scale_for_dpi(dpi);
        INFO("dpi: " << dpi << " scale: " << s);
        REQUIRE(s >= prev);
        REQUIRE(s <= DisplayMetrics::kMaxScale);
        prev = s;
    }
}

// ============================================================================
// Trust filtering — 6 of 8 devices lie
// ============================================================================

TEST_CASE("implausible reported DPI is rejected", "[display_metrics][dpi]") {
    // CB1/HDMI5 reports 890x500mm for a 5" panel, computing to 23 DPI.
    REQUIRE_FALSE(DisplayMetrics::dpi_is_plausible(23.2));
    // Snapmaker U1 claims a 217x136mm panel on a 480x320 display: 57 DPI.
    REQUIRE_FALSE(DisplayMetrics::dpi_is_plausible(57.2));
    // Zero is the honest "I don't know" the K1C reports.
    REQUIRE_FALSE(DisplayMetrics::dpi_is_plausible(0.0));

    REQUIRE(DisplayMetrics::dpi_is_plausible(131.0));
    REQUIRE(DisplayMetrics::dpi_is_plausible(405.0));
}

TEST_CASE("simpledrm's synthesized 96 DPI is not treated as a measurement",
          "[display_metrics][dpi]") {
    REQUIRE(DisplayMetrics::dpi_is_synthesized(96.0));
    REQUIRE(DisplayMetrics::dpi_is_synthesized(96.3)); // CC1's actual report
    REQUIRE_FALSE(DisplayMetrics::dpi_is_synthesized(131.0));
    REQUIRE_FALSE(DisplayMetrics::dpi_is_synthesized(160.0));

    // Synthesized values are plausible but must still be distrusted.
    REQUIRE(DisplayMetrics::dpi_is_plausible(96.0));
    REQUIRE_FALSE(DisplayMetrics::dpi_is_trustworthy(96.0));
    REQUIRE_FALSE(DisplayMetrics::dpi_is_trustworthy(23.2));
    REQUIRE(DisplayMetrics::dpi_is_trustworthy(405.0));
}

// ============================================================================
// Known-panel table — integrated displays only
// ============================================================================

TEST_CASE("integrated-panel boards have a known panel", "[display_metrics][panel]") {
    for (Board board : {Board::Ad5m, Board::Cc1, Board::K1, Board::K2}) {
        INFO("board: " << helix::board::info(board).key);
        REQUIRE(DisplayMetrics::known_panel(board).has_value());
    }
}

TEST_CASE("known_panel finds the K1 on the unified build", "[display_metrics][panel]") {
    // A release K1 is the "mips" platform key, so only the resolved board can find its row.
    const auto panel = DisplayMetrics::known_panel(Board::K1);
    REQUIRE(panel.has_value());
    CHECK(panel->width_px == 480);
    CHECK(panel->height_px == 800);
    CHECK(panel->diagonal_mm == Catch::Approx(108.6));
    CHECK(DisplayMetrics::resolve_dpi(0, std::nullopt, Board::K1).source == DpiSource::KnownPanel);
}

TEST_CASE("platforms driving arbitrary panels have no entry", "[display_metrics][panel]") {
    // A Pi, a CB1, or an x86 desktop drives a user-chosen display, so panel
    // size is not a property of the platform. Claiming one would be a
    // guess applied to every user of that build. The AD5X and Snapmaker U1 have
    // no measured panel at all.
    for (Board board : {Board::Generic, Board::Ad5x, Board::SnapmakerU1}) {
        INFO("board: " << helix::board::info(board).key);
        REQUIRE_FALSE(DisplayMetrics::known_panel(board).has_value());
    }
}

TEST_CASE("a panel with a known mount but no measured diagonal sizes by the fallback",
          "[display_metrics][panel]") {
    // The Creator 5 line's framebuffer and mount are known, its diagonal is not.
    for (Board board : {Board::Creator5, Board::Creator5Pro}) {
        INFO("board: " << helix::board::info(board).key);
        REQUIRE(DisplayMetrics::known_panel(board).has_value());
        const auto r = DisplayMetrics::resolve_dpi(0, std::nullopt, board);
        CHECK(r.source == DpiSource::Fallback);
        CHECK(r.dpi == Catch::Approx(DisplayMetrics::kReferenceDpi));
    }
}

TEST_CASE("known panels reproduce their measured DPI", "[display_metrics][panel]") {
    struct Expect {
        Board board;
        double dpi;
    };
    // K1C reports 0x0 over the ioctl, so the table is the only source it has.
    constexpr Expect expected[] = {
        {Board::Ad5m, 221.5},
        {Board::Cc1, 131.0},
        {Board::K1, 218.2},
        {Board::K2, 218.2},
    };
    for (const auto& e : expected) {
        INFO("board: " << helix::board::info(e.board).key);
        auto panel = DisplayMetrics::known_panel(e.board);
        REQUIRE(panel.has_value());
        REQUIRE(DisplayMetrics::dpi_of(*panel) == Catch::Approx(e.dpi).margin(1.0));
    }
}

TEST_CASE("known panel DPI values all sit inside the deadband", "[display_metrics][panel]") {
    // If this ever fails, adding a panel silently resized a shipping device.
    for (Board board : {Board::Ad5m, Board::Cc1, Board::K1, Board::K2}) {
        INFO("board: " << helix::board::info(board).key);
        auto panel = DisplayMetrics::known_panel(board);
        REQUIRE(panel.has_value());
        REQUIRE(DisplayMetrics::ui_scale_for_dpi(DisplayMetrics::dpi_of(*panel)) ==
                Catch::Approx(1.0));
    }
}

TEST_CASE("a K1 panel comes up in its mount without the rotation probe",
          "[display_metrics][panel]") {
    // K1, K1C and K1 Max share one portrait panel in one mount and report no
    // kernel panel_orientation, so the board is the only source of the answer.
    CHECK(DisplayMetrics::board_mount_rotation(Board::K1, 480, 800) == 270);
}

TEST_CASE("a Creator 5 panel comes up in its mount without the rotation probe",
          "[display_metrics][panel]") {
    // The landscape panel sits behind a 480x800 portrait framebuffer. A Z-Mod install
    // is a plain unzip with no installer to write the preset's rotation, so the board
    // is the only source of the answer there.
    for (Board board : {Board::Creator5, Board::Creator5Pro}) {
        INFO("board: " << helix::board::info(board).key);
        CHECK(DisplayMetrics::board_mount_rotation(board, 480, 800) == 90);
        CHECK(DisplayMetrics::board_mount_rotation(board, 800, 480) == -1);
    }
}

TEST_CASE("a K1-resolved board with another framebuffer still runs the probe",
          "[display_metrics][panel]") {
    // Board::K1 is the mips fall-through: an AD5X whose mod probe missed is 800x480,
    // a Creator 5 Pro is 480x800 portrait at rotate 90, a K2 SE or KE is unmeasured.
    CHECK(DisplayMetrics::board_mount_rotation(Board::K1, 800, 480) == -1);
    CHECK(DisplayMetrics::board_mount_rotation(Board::K1, 480, 272) == -1);
    CHECK(DisplayMetrics::board_mount_rotation(Board::K1, 1024, 600) == -1);
    CHECK(DisplayMetrics::board_mount_rotation(Board::K1, 0, 0) == -1);
}

TEST_CASE("a board with no known mount leaves orientation to the probe",
          "[display_metrics][panel]") {
    for (Board board :
         {Board::Generic, Board::Ad5m, Board::Ad5x, Board::Cc1, Board::K2, Board::SnapmakerU1}) {
        INFO("board: " << helix::board::info(board).key);
        const auto panel = DisplayMetrics::known_panel(board);
        const int w = panel ? panel->width_px : 480;
        const int h = panel ? panel->height_px : 800;
        CHECK(DisplayMetrics::board_mount_rotation(board, w, h) == -1);
    }
}

TEST_CASE("known panel mounts match the platform manifest", "[display_metrics][panel]") {
    // assets/config/platforms.json is the record of each measured panel; the table
    // here is its compiled copy for the boards that use it at runtime.
    const auto path = std::filesystem::current_path() / "assets" / "config" / "platforms.json";
    INFO("manifest path: " << path.string());
    std::ifstream f(path);
    REQUIRE(f.good());
    nlohmann::json manifest;
    f >> manifest;
    const auto& platforms = manifest.contains("platforms") ? manifest["platforms"] : manifest;
    for (const char* key : {"k1"}) {
        INFO("platform: " << key);
        const auto& panel = platforms.at(key).at("panel");
        const auto spec = DisplayMetrics::known_panel(helix::board::resolve(key));
        REQUIRE(spec.has_value());
        CHECK(spec->width_px == panel.at("width").get<int>());
        CHECK(spec->height_px == panel.at("height").get<int>());
        CHECK(spec->upright_rotation == panel.at("rotate").get<int>());
    }
    // Boards with no platform entry of their own are inline candidates of the unified build.
    for (Board board : {Board::Creator5, Board::Creator5Pro}) {
        const std::string key = helix::board::info(board).key;
        INFO("mips candidate: " << key);
        const nlohmann::json* panel = nullptr;
        for (const auto& candidate : platforms.at("mips").at("panel").at("candidates")) {
            if (candidate.is_object() && candidate.value("board", "") == key) {
                panel = &candidate;
            }
        }
        REQUIRE(panel != nullptr);
        const auto spec = DisplayMetrics::known_panel(board);
        REQUIRE(spec.has_value());
        CHECK(spec->width_px == panel->at("width").get<int>());
        CHECK(spec->height_px == panel->at("height").get<int>());
        CHECK(spec->upright_rotation == panel->at("rotate").get<int>());
    }
}

TEST_CASE("K1 presets leave the panel mount to the board table", "[display_metrics][panel]") {
    // A preset applies after Moonraker connects, and on any box that detects as a
    // K1. The mount belongs to the geometry-checked board table alone.
    for (const char* name : {"k1", "k1c"}) {
        INFO("preset: " << name);
        const auto path = std::filesystem::current_path() / "assets" / "config" / "presets" /
                          (std::string(name) + ".json");
        std::ifstream f(path);
        REQUIRE(f.good());
        nlohmann::json preset;
        f >> preset;
        const auto display = preset.value("display", nlohmann::json::object());
        CHECK_FALSE(display.contains("rotate"));
        CHECK_FALSE(display.contains("rotation_probed"));
    }
}

// ============================================================================
// Source precedence
// ============================================================================

TEST_CASE("user override beats every other source", "[display_metrics][resolve]") {
    auto r = DisplayMetrics::resolve_dpi(300, 405.0, Board::Ad5m);
    REQUIRE(r.source == DpiSource::UserOverride);
    REQUIRE(r.dpi == Catch::Approx(300.0));
}

TEST_CASE("trusted platform measurement beats the known-panel table",
          "[display_metrics][resolve]") {
    auto r = DisplayMetrics::resolve_dpi(0, 405.0, Board::Ad5m);
    REQUIRE(r.source == DpiSource::PlatformMeasured);
    REQUIRE(r.dpi == Catch::Approx(405.0));
}

TEST_CASE("untrustworthy platform measurement falls through to the table",
          "[display_metrics][resolve]") {
    // A trusted platform can still hand back junk; the band still applies.
    auto r = DisplayMetrics::resolve_dpi(0, 23.2, Board::K2);
    REQUIRE(r.source == DpiSource::KnownPanel);
    REQUIRE(r.dpi == Catch::Approx(218.2).margin(1.0));
}

TEST_CASE("no measurement and no known panel lands on the authoring reference",
          "[display_metrics][resolve]") {
    auto r = DisplayMetrics::resolve_dpi(0, std::nullopt, Board::Generic);
    REQUIRE(r.source == DpiSource::Fallback);
    REQUIRE(r.dpi == Catch::Approx(DisplayMetrics::kReferenceDpi));
    // And the fallback must be a no-op on sizing.
    REQUIRE(DisplayMetrics::ui_scale_for_dpi(r.dpi) == Catch::Approx(1.0));
}

TEST_CASE("out-of-range user override is ignored rather than obeyed",
          "[display_metrics][resolve]") {
    auto r = DisplayMetrics::resolve_dpi(5000, std::nullopt, Board::Generic);
    REQUIRE(r.source != DpiSource::UserOverride);
}

// ============================================================================
// scaled_px
// ============================================================================

TEST_CASE("scaled_px rounds and never returns zero for a positive input",
          "[display_metrics][dpi]") {
    REQUIRE(DisplayMetrics::scaled_px(100, 1.0) == 100);
    REQUIRE(DisplayMetrics::scaled_px(100, 1.578) == 158);
    REQUIRE(DisplayMetrics::scaled_px(3, 1.0) == 3);
    // A 1px hairline must survive scaling rather than collapsing away.
    REQUIRE(DisplayMetrics::scaled_px(1, 1.0) >= 1);
}

// ============================================================================
// Font remapping — the ladder tops out at 40 (26 light), so the scale needs
// the larger faces added in FONTS_XXLARGE
// ============================================================================

TEST_CASE("font names remap to the nearest larger face", "[display_metrics][font]") {
    const double scale = DisplayMetrics::ui_scale_for_dpi(kPhoneDpi); // ~1.578

    // font_body_xxlarge: 32 * 1.578 = 50.5, nearest rung is 48.
    REQUIRE(DisplayMetrics::scaled_font_name("noto_sans_32", scale) == "noto_sans_48");
    // font_heading_xxlarge: 40 * 1.578 = 63.1 — 64 is off by 1, 48 by 15.
    REQUIRE(DisplayMetrics::scaled_font_name("noto_sans_40", scale) == "noto_sans_64");
    // font_small_xxlarge: light 26 * 1.578 = 41.0.
    REQUIRE(DisplayMetrics::scaled_font_name("noto_sans_light_26", scale) == "noto_sans_light_40");
    REQUIRE(DisplayMetrics::scaled_font_name("noto_sans_bold_40", scale) == "noto_sans_bold_64");
}

TEST_CASE("font remapping preserves weight", "[display_metrics][font]") {
    // Splitting on the FIRST underscore would turn light_26 into a regular
    // face; the prefix must be "noto_sans_light_", not "noto_sans_".
    const double scale = DisplayMetrics::ui_scale_for_dpi(kPhoneDpi);
    const std::string light = DisplayMetrics::scaled_font_name("noto_sans_light_20", scale);
    const std::string bold = DisplayMetrics::scaled_font_name("noto_sans_bold_32", scale);
    REQUIRE(light.rfind("noto_sans_light_", 0) == 0);
    REQUIRE(bold.rfind("noto_sans_bold_", 0) == 0);
}

TEST_CASE("icon fonts remap on their own rungs", "[display_metrics][font]") {
    const double scale = DisplayMetrics::ui_scale_for_dpi(kPhoneDpi);
    // 64 * 1.578 = 101; rungs are 80/96/128, so 96 wins.
    REQUIRE(DisplayMetrics::scaled_font_name("mdi_icons_64", scale) == "mdi_icons_96");
}

TEST_CASE("font remapping is a no-op at scale 1.0", "[display_metrics][font]") {
    // Every printer sits here, so this is the no-regression case.
    for (const char* name :
         {"noto_sans_32", "noto_sans_light_26", "noto_sans_bold_40", "mdi_icons_64"}) {
        INFO("font: " << name);
        REQUIRE(DisplayMetrics::scaled_font_name(name, 1.0) == name);
    }
}

TEST_CASE("font remapping never returns a smaller face", "[display_metrics][font]") {
    // The scale only ever grows; a face must never shrink out from under a panel.
    REQUIRE(DisplayMetrics::scaled_font_name("noto_sans_64", 1.05) == "noto_sans_64");
    REQUIRE(DisplayMetrics::scaled_font_name("noto_sans_40", 0.5) == "noto_sans_40");
}

TEST_CASE("unparseable or unknown font names pass through", "[display_metrics][font]") {
    const double scale = 1.578;
    REQUIRE(DisplayMetrics::scaled_font_name("some_custom_face", scale) == "some_custom_face");
    REQUIRE(DisplayMetrics::scaled_font_name("noname", scale) == "noname");
    REQUIRE(DisplayMetrics::scaled_font_name("trailing_", scale) == "trailing_");
    REQUIRE(DisplayMetrics::scaled_font_name("", scale) == "");
}

// ============================================================================
// Active scale
// ============================================================================

TEST_CASE("active scale defaults to 1.0 and rejects shrinking values", "[display_metrics][scale]") {
    DisplayMetrics::set_active_scale(1.5);
    REQUIRE(DisplayMetrics::active_scale() == Catch::Approx(1.5));

    // Anything that would shrink the UI is refused, so a bad stored setting
    // cannot make the whole interface smaller than authored.
    DisplayMetrics::set_active_scale(0.4);
    REQUIRE(DisplayMetrics::active_scale() == Catch::Approx(1.0));

    DisplayMetrics::set_active_scale(std::nan(""));
    REQUIRE(DisplayMetrics::active_scale() == Catch::Approx(1.0));

    DisplayMetrics::set_active_scale(1.0); // restore for other tests
}

// ============================================================================
// UI scale setting
// ============================================================================

TEST_CASE("Automatic follows the DPI-derived scale", "[display_metrics][scale]") {
    const double automatic = DisplayMetrics::kScaleSettingAutomatic;
    REQUIRE(DisplayMetrics::scale_for_setting(automatic, 1.578) == Catch::Approx(1.578));
    REQUIRE(DisplayMetrics::scale_for_setting(automatic, 1.0) == Catch::Approx(1.0));

    // Every shipping printer resolves to 1.0, so Automatic is the identity
    // there and the setting cannot be what changes a printer's sizing.
    REQUIRE(DisplayMetrics::scale_for_setting(automatic, DisplayMetrics::ui_scale_for_dpi(221.5)) ==
            Catch::Approx(1.0));
}

TEST_CASE("an explicit setting overrides the DPI-derived scale", "[display_metrics][scale]") {
    // The point of the setting: a user on a panel we scaled wrongly, or who
    // simply wants bigger type, wins over the measurement.
    REQUIRE(DisplayMetrics::scale_for_setting(125, 1.578) == Catch::Approx(1.25));
    REQUIRE(DisplayMetrics::scale_for_setting(200, 1.0) == Catch::Approx(2.0));

    // 100% is a real choice, not a synonym for Automatic: it pins a high-DPI
    // panel back to authored sizing.
    REQUIRE(DisplayMetrics::scale_for_setting(100, 1.578) == Catch::Approx(1.0));
}

TEST_CASE("an out-of-range setting falls back to Automatic rather than clamping",
          "[display_metrics][scale]") {
    // A value this far out is corrupt or hand-edited. Clamping it would pin
    // the UI to a size nobody picked and look like the setting was ignored;
    // falling back to the measurement is the recoverable answer.
    REQUIRE(DisplayMetrics::scale_for_setting(50, 1.578) == Catch::Approx(1.578));
    REQUIRE(DisplayMetrics::scale_for_setting(99, 1.578) == Catch::Approx(1.578));
    REQUIRE(DisplayMetrics::scale_for_setting(201, 1.578) == Catch::Approx(1.578));
    REQUIRE(DisplayMetrics::scale_for_setting(-1, 1.578) == Catch::Approx(1.578));

    // The ceiling is the same one the curve itself is capped at, so the
    // setting cannot ask for a scale the DPI path would refuse to produce.
    REQUIRE(DisplayMetrics::kMaxScaleSettingPercent ==
            static_cast<int>(DisplayMetrics::kMaxScale * 100));
    REQUIRE(DisplayMetrics::scale_for_setting(DisplayMetrics::kMaxScaleSettingPercent, 1.0) ==
            Catch::Approx(DisplayMetrics::kMaxScale));
}

TEST_CASE("a bad auto scale cannot shrink the UI below authored size", "[display_metrics][scale]") {
    // scale_for_setting is the last gate before set_active_scale, so it holds
    // the same never-shrink floor rather than trusting its caller.
    REQUIRE(DisplayMetrics::scale_for_setting(DisplayMetrics::kScaleSettingAutomatic, 0.4) ==
            Catch::Approx(1.0));
    REQUIRE(DisplayMetrics::scale_for_setting(DisplayMetrics::kScaleSettingAutomatic,
                                              std::nan("")) == Catch::Approx(1.0));
}

TEST_CASE("the auto scale survives being overridden", "[display_metrics][scale]") {
    // The settings UI renders "Automatic (158%)" from this, so it has to stay
    // readable after an explicit setting has taken over active_scale().
    DisplayMetrics::set_auto_scale(1.578);
    DisplayMetrics::set_active_scale(DisplayMetrics::scale_for_setting(125, 1.578));

    REQUIRE(DisplayMetrics::active_scale() == Catch::Approx(1.25));
    REQUIRE(DisplayMetrics::auto_scale() == Catch::Approx(1.578));

    DisplayMetrics::set_auto_scale(1.0); // restore for other tests
    DisplayMetrics::set_active_scale(1.0);
}

TEST_CASE("an upscaled logical surface is scaled by its own pixel density",
          "[display_metrics][logical_dpi]") {
    // Galaxy Z Flip 7: a 480 DPI 2520x1080 panel the Android SDL driver draws at
    // 1260x540 and doubles. The logical pixel is 240 DPI, which sits just past
    // the deadband; the native figure would nearly double the UI.
    const double flip = DisplayMetrics::logical_dpi(480.0, 2520, 1080, 1260, 540);
    CHECK(flip == Catch::Approx(240.0));
    CHECK(DisplayMetrics::ui_scale_for_dpi(flip) < 1.06);
    CHECK(DisplayMetrics::ui_scale_for_dpi(480.0) > 1.8);

    // The panel's orientation and the surface's need not agree.
    CHECK(DisplayMetrics::logical_dpi(480.0, 1080, 2520, 1260, 540) == Catch::Approx(240.0));

    // A 3x surface, and a native one, which is the identity.
    CHECK(DisplayMetrics::logical_dpi(450.0, 3200, 1440, 1066, 480) ==
          Catch::Approx(450.0 / 3.0).epsilon(0.01));
    CHECK(DisplayMetrics::logical_dpi(200.0, 1920, 1200, 1920, 1200) == Catch::Approx(200.0));

    // Without both sizes there is nothing to convert by.
    CHECK(DisplayMetrics::logical_dpi(480.0, 0, 0, 1260, 540) == Catch::Approx(480.0));
    CHECK(DisplayMetrics::logical_dpi(480.0, 2520, 1080, 0, 540) == Catch::Approx(480.0));
}

TEST_CASE("the scale decision follows the drawing surface it is given",
          "[display_metrics][decide_scale]") {
    constexpr int kAuto = DisplayMetrics::kScaleSettingAutomatic;

    // Galaxy Z Flip 7 unfolded: 480 DPI native, drawn 2x.
    const auto unfolded =
        DisplayMetrics::decide_scale(0, 480.0, 2520, 1080, 1260, 540, Board::Generic, kAuto);
    CHECK(unfolded.resolved.source == DpiSource::PlatformMeasured);
    CHECK(unfolded.resolved.dpi == Catch::Approx(240.0));
    CHECK(unfolded.ui_scale == Catch::Approx(DisplayMetrics::ui_scale_for_dpi(240.0)));
    CHECK(unfolded.lvgl_dpi == 168);

    // The same panel drawn at a different integer fraction after a fold or a
    // resize gets a different scale, which is what the resize path re-decides.
    const auto native =
        DisplayMetrics::decide_scale(0, 480.0, 2520, 1080, 2520, 1080, Board::Generic, kAuto);
    CHECK(native.ui_scale > unfolded.ui_scale);
    CHECK(native.lvgl_dpi > unfolded.lvgl_dpi);

    // --dpi names the logical surface's DPI directly; the sizes do not touch it.
    const auto forced =
        DisplayMetrics::decide_scale(200, 480.0, 2520, 1080, 1260, 540, Board::Generic, kAuto);
    CHECK(forced.resolved.source == DpiSource::UserOverride);
    CHECK(forced.ui_scale == Catch::Approx(1.0));

    // A stored percentage wins over the measurement, but the auto scale the
    // settings row shows still comes from it.
    const auto fixed =
        DisplayMetrics::decide_scale(0, 480.0, 2520, 1080, 1260, 540, Board::Generic, 150);
    CHECK(fixed.ui_scale == Catch::Approx(1.5));
    CHECK(fixed.auto_scale == Catch::Approx(unfolded.auto_scale));
    CHECK(fixed.lvgl_dpi == 240);

    // No platform measurement (every Linux target): the surface sizes are
    // irrelevant and a generic board stays at the authored size.
    const auto linux_panel =
        DisplayMetrics::decide_scale(0, std::nullopt, 0, 0, 800, 480, Board::Generic, kAuto);
    CHECK(linux_panel.resolved.source == DpiSource::Fallback);
    CHECK(linux_panel.ui_scale == Catch::Approx(1.0));
    CHECK(linux_panel.lvgl_dpi == 160);
}
