// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "calibration_types.h"
#include "moonraker_api_mock.h"
#include "screws_tilt_parser.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Parse a block of Klipper console lines the way ScrewsTiltCollector does.
std::vector<ScrewTiltResult> parse_lines(const std::vector<std::string>& lines) {
    std::vector<ScrewTiltResult> results;
    for (const auto& line : lines) {
        ScrewTiltResult result;
        if (helix::parse_screws_tilt_line(line, result)) {
            results.push_back(result);
        }
    }
    return results;
}

/**
 * The bed from prestonbrown/helixscreen#1225, verbatim.
 *
 * Probed Z, and the exact adjustments Klipper emitted for a CW-M4 thread
 * (diff = z_base - z; front_left is the base because it is first in config
 * order). Corner-to-corner error is 0.11125 mm — 8x what the reporter reached
 * by levelling from the console — yet every screw is <= 5 minutes from the base.
 */
const std::vector<std::string> OUT_OF_LEVEL_BED = {
    "// front_left (base) : x=30.0, y=30.0, z=-0.055125",
    "// front_right : x=200.0, y=30.0, z=-0.111375 : adjust CW 00:05",
    "// rear_right : x=200.0, y=200.0, z=-0.000125 : adjust CCW 00:05",
    "// rear_left : x=30.0, y=200.0, z=-0.022625 : adjust CCW 00:03",
};

/// The reporter's follow-up console run: 0.01375 mm spread, genuinely level.
const std::vector<std::string> LEVEL_BED = {
    "// front_left (base) : x=30.0, y=30.0, z=-0.067500",
    "// front_right : x=200.0, y=30.0, z=-0.055000 : adjust CCW 00:01",
    "// rear_right : x=200.0, y=200.0, z=-0.060000 : adjust CCW 00:01",
    "// rear_left : x=30.0, y=200.0, z=-0.068750 : adjust CW 00:00",
};

ScrewTiltResult make_screw(const std::string& name, const std::string& adjustment,
                           bool is_reference = false) {
    ScrewTiltResult r;
    r.screw_name = name;
    r.adjustment = adjustment;
    r.is_reference = is_reference;
    return r;
}

} // namespace

TEST_CASE("flip_screws_tilt_direction swaps CW and CCW tokens", "[calibration][screws_tilt]") {
    SECTION("CW becomes CCW") {
        std::string s = "CW 01:15";
        flip_screws_tilt_direction(s);
        REQUIRE(s == "CCW 01:15");
    }

    SECTION("CCW becomes CW") {
        std::string s = "CCW 00:30";
        flip_screws_tilt_direction(s);
        REQUIRE(s == "CW 00:30");
    }

    SECTION("Zero-turn CW preserves magnitude") {
        std::string s = "CW 00:00";
        flip_screws_tilt_direction(s);
        REQUIRE(s == "CCW 00:00");
    }

    SECTION("Non-matching prefix is untouched") {
        std::string s = "XW 01:00";
        flip_screws_tilt_direction(s);
        REQUIRE(s == "XW 01:00");
    }

    SECTION("Empty string is a no-op") {
        std::string s;
        flip_screws_tilt_direction(s);
        REQUIRE(s.empty());
    }

    SECTION("String without direction token is a no-op") {
        std::string s = "00:00";
        flip_screws_tilt_direction(s);
        REQUIRE(s == "00:00");
    }

    SECTION("Mid-string CW is not flipped") {
        std::string s = "foo CW bar";
        flip_screws_tilt_direction(s);
        REQUIRE(s == "foo CW bar");
    }

    SECTION("Round-trip flip restores original") {
        std::string s = "CW 02:45";
        flip_screws_tilt_direction(s);
        flip_screws_tilt_direction(s);
        REQUIRE(s == "CW 02:45");
    }
}

TEST_CASE("ScrewTiltResult::adjustment_minutes parses arc-minute totals",
          "[calibration][screws_tilt]") {
    ScrewTiltResult r;
    r.is_reference = false;

    SECTION("CW 00:15 is 15 minutes") {
        r.adjustment = "CW 00:15";
        REQUIRE(r.adjustment_minutes() == 15);
    }

    SECTION("CCW 01:30 is 90 minutes") {
        r.adjustment = "CCW 01:30";
        REQUIRE(r.adjustment_minutes() == 90);
    }

    SECTION("Multi-turn CW 02:45 is 165 minutes") {
        r.adjustment = "CW 02:45";
        REQUIRE(r.adjustment_minutes() == 165);
    }

    SECTION("Reference screw returns 0") {
        r.is_reference = true;
        r.adjustment = "CW 01:15"; // Should be ignored for reference
        REQUIRE(r.adjustment_minutes() == 0);
    }

    SECTION("Empty adjustment returns 0") {
        r.adjustment = "";
        REQUIRE(r.adjustment_minutes() == 0);
    }

    SECTION("Malformed adjustment returns 0") {
        r.adjustment = "garbage";
        REQUIRE(r.adjustment_minutes() == 0);
    }
}

TEST_CASE("ScrewTiltResult::signed_adjustment_minutes keeps CW/CCW sign",
          "[calibration][screws_tilt][1225]") {
    SECTION("CW is positive") {
        REQUIRE(make_screw("a", "CW 00:05").signed_adjustment_minutes() == 5);
    }

    SECTION("CCW is negative") {
        REQUIRE(make_screw("a", "CCW 00:05").signed_adjustment_minutes() == -5);
    }

    SECTION("Turns carry the sign too") {
        REQUIRE(make_screw("a", "CCW 01:30").signed_adjustment_minutes() == -90);
    }

    SECTION("Klipper's uncarried 00:60 reads as a full turn") {
        // screws_tilt_adjust.py rounds decimal_part*60 without carrying into
        // full_turns, so a near-full turn really is printed as "00:60".
        REQUIRE(make_screw("a", "CW 00:60").signed_adjustment_minutes() == 60);
        REQUIRE(make_screw("a", "CCW 00:60").signed_adjustment_minutes() == -60);
    }

    SECTION("Base screw is exactly zero") {
        REQUIRE(make_screw("base", "", true).signed_adjustment_minutes() == 0);
    }

    SECTION("Unknown direction token is a parse failure, not zero") {
        REQUIRE_FALSE(make_screw("a", "XW 00:05").signed_adjustment_minutes().has_value());
    }

    SECTION("Missing direction token is a parse failure") {
        REQUIRE_FALSE(make_screw("a", "00:05").signed_adjustment_minutes().has_value());
    }

    SECTION("Empty adjustment on a non-base screw is a parse failure") {
        REQUIRE_FALSE(make_screw("a", "").signed_adjustment_minutes().has_value());
    }

    SECTION("Garbage is a parse failure") {
        REQUIRE_FALSE(make_screw("a", "garbage").signed_adjustment_minutes().has_value());
    }
}

TEST_CASE("Screw tolerance follows the configured thread pitch",
          "[calibration][screws_tilt][1225]") {
    SECTION("Klipper screw_thread tokens map to their pitch") {
        REQUIRE(screw_thread_pitch_mm("CW-M3") == Catch::Approx(0.5f));
        REQUIRE(screw_thread_pitch_mm("CCW-M3") == Catch::Approx(0.5f));
        REQUIRE(screw_thread_pitch_mm("CW-M4") == Catch::Approx(0.7f));
        REQUIRE(screw_thread_pitch_mm("CCW-M4") == Catch::Approx(0.7f));
        REQUIRE(screw_thread_pitch_mm("CW-M5") == Catch::Approx(0.8f));
        REQUIRE(screw_thread_pitch_mm("CW-M6") == Catch::Approx(1.0f));
    }

    SECTION("Unknown or missing thread falls back to M3") {
        REQUIRE(screw_thread_pitch_mm("") == Catch::Approx(SCREW_PITCH_M3_MM));
        REQUIRE(screw_thread_pitch_mm("CW-M8") == Catch::Approx(SCREW_PITCH_M3_MM));
        REQUIRE(screw_thread_pitch_mm("nonsense") == Catch::Approx(SCREW_PITCH_M3_MM));
    }

    SECTION("A 0.05mm level window is a different minute count per pitch") {
        // 0.05mm / pitch * 60, rounded
        REQUIRE(screw_level_tolerance_minutes(SCREW_PITCH_M3_MM) == 6);
        REQUIRE(screw_level_tolerance_minutes(SCREW_PITCH_M4_MM) == 4);
        REQUIRE(screw_level_tolerance_minutes(SCREW_PITCH_M5_MM) == 4);
        REQUIRE(screw_level_tolerance_minutes(SCREW_PITCH_M6_MM) == 3);
    }

    SECTION("A coarser thread never gets a wider window than a finer one") {
        REQUIRE(screw_level_tolerance_minutes(SCREW_PITCH_M3_MM) >=
                screw_level_tolerance_minutes(SCREW_PITCH_M4_MM));
        REQUIRE(screw_level_tolerance_minutes(SCREW_PITCH_M4_MM) >=
                screw_level_tolerance_minutes(SCREW_PITCH_M6_MM));
    }

    SECTION("Tolerance never collapses to an exact-match test") {
        REQUIRE(screw_minutes_for_mm(0.0f, SCREW_PITCH_M6_MM) == 1);
        REQUIRE(screw_minutes_for_mm(SCREW_LEVEL_TOLERANCE_MM, 0.0f) ==
                screw_level_tolerance_minutes(SCREW_PITCH_DEFAULT_MM));
    }
}

TEST_CASE("parse_screws_tilt_line reads Klipper console output",
          "[calibration][screws_tilt][1225]") {
    SECTION("Base screw line") {
        ScrewTiltResult r;
        REQUIRE(helix::parse_screws_tilt_line(OUT_OF_LEVEL_BED[0], r));
        REQUIRE(r.screw_name == "front_left");
        REQUIRE(r.is_reference);
        REQUIRE(r.z_height == Catch::Approx(-0.055125f));
        REQUIRE(r.signed_adjustment_minutes() == 0);
    }

    SECTION("Adjusted screw line") {
        ScrewTiltResult r;
        REQUIRE(helix::parse_screws_tilt_line(OUT_OF_LEVEL_BED[1], r));
        REQUIRE(r.screw_name == "front_right");
        REQUIRE_FALSE(r.is_reference);
        REQUIRE(r.x_pos == Catch::Approx(200.0f));
        REQUIRE(r.y_pos == Catch::Approx(30.0f));
        REQUIRE(r.z_height == Catch::Approx(-0.111375f));
        REQUIRE(r.adjustment_minutes() == 5);
    }

    SECTION("Non-screw console chatter is rejected") {
        ScrewTiltResult r;
        REQUIRE_FALSE(helix::parse_screws_tilt_line("ok", r));
        REQUIRE_FALSE(helix::parse_screws_tilt_line("// Klipper state: Ready", r));
    }
}

TEST_CASE("evaluate_screw_level judges the spread, not each screw's magnitude",
          "[calibration][screws_tilt][1225]") {
    SECTION("The #1225 bed is NOT level on its real M4 thread") {
        auto results = parse_lines(OUT_OF_LEVEL_BED);
        REQUIRE(results.size() == 4);

        auto report = evaluate_screw_level(results, SCREW_PITCH_M4_MM);
        // +5 (CW) and -5 (CCW) about a mid-range base: 10 minutes corner to
        // corner even though no single screw exceeds 5.
        REQUIRE_FALSE(report.is_level());
        REQUIRE(report.verdict == ScrewLevelVerdict::NEEDS_ADJUSTMENT);
        REQUIRE(report.spread_minutes == 10);
        REQUIRE(report.tolerance_minutes == 4);
    }

    SECTION("The #1225 bed is NOT level on an M3 thread either") {
        auto results = parse_lines(OUT_OF_LEVEL_BED);
        auto report = evaluate_screw_level(results, SCREW_PITCH_M3_MM);
        REQUIRE_FALSE(report.is_level());
        REQUIRE(report.spread_minutes == 10);
        REQUIRE(report.tolerance_minutes == 6);
    }

    SECTION("Base mid-range with opposite signs is caught at every pitch") {
        // Each screw is inside a 6-minute window of the base, but the outer two
        // are 12 minutes apart from each other.
        std::vector<ScrewTiltResult> results = {
            make_screw("front_left", "", true),
            make_screw("front_right", "CW 00:06"),
            make_screw("rear_right", "CCW 00:06"),
            make_screw("rear_left", "CW 00:00"),
        };

        for (float pitch :
             {SCREW_PITCH_M3_MM, SCREW_PITCH_M4_MM, SCREW_PITCH_M5_MM, SCREW_PITCH_M6_MM}) {
            auto report = evaluate_screw_level(results, pitch);
            INFO("pitch " << pitch);
            REQUIRE(report.spread_minutes == 12);
            REQUIRE_FALSE(report.is_level());
        }
    }

    SECTION("A genuinely level bed still reports level") {
        auto results = parse_lines(LEVEL_BED);
        REQUIRE(results.size() == 4);

        auto report = evaluate_screw_level(results, SCREW_PITCH_M4_MM);
        REQUIRE(report.spread_minutes == 1);
        REQUIRE(report.verdict == ScrewLevelVerdict::LEVEL);
        for (size_t i = 0; i < results.size(); i++) {
            INFO("screw " << results[i].screw_name);
            REQUIRE(report.in_spec[i]);
        }
    }

    SECTION("All screws level on one side of the base is still level") {
        // Everything within the window of the base and of each other.
        std::vector<ScrewTiltResult> results = {
            make_screw("front_left", "", true),
            make_screw("front_right", "CW 00:02"),
            make_screw("rear_right", "CW 00:03"),
            make_screw("rear_left", "CW 00:01"),
        };
        auto report = evaluate_screw_level(results, SCREW_PITCH_M3_MM);
        REQUIRE(report.spread_minutes == 3);
        REQUIRE(report.is_level());
    }

    SECTION("All screws marked in-spec exactly when the bed is level") {
        // The green-checkmark display must never disagree with the banner.
        std::vector<ScrewTiltResult> results = {
            make_screw("front_left", "", true),
            make_screw("front_right", "CW 00:04"),
            make_screw("rear_right", "CCW 00:04"),
            make_screw("rear_left", "CW 00:00"),
        };
        auto report = evaluate_screw_level(results, SCREW_PITCH_M3_MM);
        REQUIRE(report.spread_minutes == 8); // > 6-minute M3 window
        REQUIRE_FALSE(report.is_level());

        bool all_in_spec = true;
        for (bool ok : report.in_spec) {
            all_in_spec = all_in_spec && ok;
        }
        REQUIRE_FALSE(all_in_spec);
    }

    SECTION("Worst screw is the one furthest from the level plane") {
        std::vector<ScrewTiltResult> results = {
            make_screw("front_left", "", true),
            make_screw("front_right", "CW 00:10"),
            make_screw("rear_right", "CCW 00:40"),
            make_screw("rear_left", "CW 00:02"),
        };
        auto report = evaluate_screw_level(results, SCREW_PITCH_M3_MM);
        REQUIRE(report.worst_index == 2);
        REQUIRE_FALSE(report.in_spec[2]);
    }
}

TEST_CASE("evaluate_screw_level fails loudly instead of reading as level",
          "[calibration][screws_tilt][1225]") {
    SECTION("A malformed adjustment is a parse error, not a level bed") {
        std::vector<ScrewTiltResult> results = {
            make_screw("front_left", "", true),
            make_screw("front_right", "CW 00:01"),
            make_screw("rear_right", "adjust me"), // Klipper never emits this
            make_screw("rear_left", "CW 00:00"),
        };
        auto report = evaluate_screw_level(results, SCREW_PITCH_M3_MM);
        REQUIRE(report.verdict == ScrewLevelVerdict::PARSE_ERROR);
        REQUIRE_FALSE(report.is_level());
        REQUIRE(report.parse_error.find("rear_right") != std::string::npos);
    }

    SECTION("A missing adjustment on a non-base screw is a parse error") {
        std::vector<ScrewTiltResult> results = {
            make_screw("front_left", "", true),
            make_screw("front_right", ""), // ": adjust ..." never arrived
        };
        auto report = evaluate_screw_level(results, SCREW_PITCH_M3_MM);
        REQUIRE(report.verdict == ScrewLevelVerdict::PARSE_ERROR);
        REQUIRE_FALSE(report.is_level());
    }

    SECTION("An empty result set is not a level bed") {
        auto report = evaluate_screw_level({}, SCREW_PITCH_M3_MM);
        REQUIRE_FALSE(report.is_level());
    }

    SECTION("A truncated console line does not silently read as level") {
        // A line that lost its ": adjust CW 00:05" tail mid-transmission.
        auto results = parse_lines({
            "// front_left (base) : x=30.0, y=30.0, z=-0.055125",
            "// front_right : x=200.0, y=30.0, z=-0.111375",
        });
        REQUIRE(results.size() == 2);
        auto report = evaluate_screw_level(results, SCREW_PITCH_M4_MM);
        REQUIRE(report.verdict == ScrewLevelVerdict::PARSE_ERROR);
    }
}

TEST_CASE("ScrewTiltResult::friendly_adjustment maps direction to verb",
          "[calibration][screws_tilt]") {
    ScrewTiltResult r;
    r.is_reference = false;

    SECTION("CW output becomes Tighten") {
        r.adjustment = "CW 00:18";
        REQUIRE(r.friendly_adjustment(false) == "Tighten 1/4 turn");
    }

    SECTION("CCW output becomes Loosen") {
        r.adjustment = "CCW 00:18";
        REQUIRE(r.friendly_adjustment(false) == "Loosen 1/4 turn");
    }

    SECTION("In-spec screw reports Level") {
        r.adjustment = "CW 00:03";
        REQUIRE(r.friendly_adjustment(true) == "Level");
    }

    SECTION("Reference screw reports Reference") {
        r.is_reference = true;
        REQUIRE(r.friendly_adjustment(false) == "Reference");
    }

    SECTION("Multi-turn magnitudes are described") {
        r.adjustment = "CW 02:30";
        REQUIRE(r.friendly_adjustment(false) == "Tighten 3 turns");
    }
}

// ============================================================================
// Mock fidelity — what --test prints must be what Klipper really prints
// ============================================================================

namespace {

/// The "z=" field of a Klipper screw line, as text (so its precision survives).
std::string z_field(const std::string& line) {
    size_t pos = line.find("z=");
    if (pos == std::string::npos) {
        return "";
    }
    pos += 2;
    size_t end = line.find_first_of(", ", pos);
    if (end == std::string::npos) {
        end = line.length();
    }
    return line.substr(pos, end - pos);
}

size_t decimals_in(const std::string& number) {
    size_t dot = number.find('.');
    return dot == std::string::npos ? 0 : number.length() - dot - 1;
}

} // namespace

TEST_CASE("Mock SCREWS_TILT_CALCULATE output matches Klipper's console format",
          "[calibration][screws_tilt][mock][1225]") {
    MockScrewsTiltState bed;
    const std::vector<std::string> lines = bed.probe_lines();

    SECTION("The turn legend leads, and is not mistaken for a screw") {
        REQUIRE(lines.size() == 5);
        REQUIRE(lines[0] ==
                "// 01:20 means 1 full turn and 20 minutes, CW=clockwise, CCW=counter-clockwise");

        ScrewTiltResult phantom;
        REQUIRE_FALSE(helix::parse_screws_tilt_line(lines[0], phantom));
        REQUIRE(parse_lines(lines).size() == 4);
    }

    SECTION("Z carries Klipper's 5 decimal places, not 6") {
        for (size_t i = 1; i < lines.size(); i++) {
            INFO(lines[i]);
            REQUIRE(decimals_in(z_field(lines[i])) == 5);
        }
    }

    SECTION("The initial bed is the #1225 mid-range-base shape") {
        // Asserted on the RAW emitted text, before parse_screws_tilt_line()
        // applies any printer-database CW/CCW override — this is Klipper's
        // output, not the display string.
        REQUIRE(lines[1].rfind("// front_left (base) : x=30.0, y=30.0, z=2.5", 0) == 0);
        // 0.5 mm M3 pitch: +0.15 mm above the base is 0.3 turns, and a screw
        // above the base gives a negative z_base - z, hence CCW.
        REQUIRE(lines[2].find("// front_right : x=200.0, y=30.0, z=2.6") == 0);
        REQUIRE(lines[2].find(" : adjust CCW 00:18") != std::string::npos);
        REQUIRE(lines[3].find(" : adjust CW 00:10") != std::string::npos);
        REQUIRE(lines[4].find(" : adjust CCW 00:14") != std::string::npos);

        // Every screw is inside a 20-minute magnitude window, yet the corners
        // are 28 minutes apart — the exact trap from #1225. The spread survives
        // a wholesale CW<->CCW flip, so this holds under any direction override.
        auto results = parse_lines(lines);
        REQUIRE(results.size() == 4);
        REQUIRE(results[0].screw_name == "front_left");
        REQUIRE(results[0].is_reference);

        auto report = evaluate_screw_level(results, SCREW_PITCH_M3_MM);
        REQUIRE(report.spread_minutes == 28);
        REQUIRE(report.verdict == ScrewLevelVerdict::NEEDS_ADJUSTMENT);
    }
}

TEST_CASE("MockScrewsTiltState::diff_to_adjustment mirrors Klipper's arithmetic",
          "[calibration][screws_tilt][mock][1225]") {
    // The mock advertises a CW-M3 thread — 0.5 mm per turn.
    SECTION("Positive diff is CW, negative is CCW") {
        REQUIRE(MockScrewsTiltState::diff_to_adjustment(0.75f) == "CW 01:30");
        REQUIRE(MockScrewsTiltState::diff_to_adjustment(-0.75f) == "CCW 01:30");
    }

    SECTION("Sub-micron diffs are deadbanded before the pitch divide") {
        REQUIRE(MockScrewsTiltState::diff_to_adjustment(0.0005f) == "CW 00:00");
        // Klipper zeroes the value before taking the sign, so a negative diff
        // inside the deadband still prints CW.
        REQUIRE(MockScrewsTiltState::diff_to_adjustment(-0.0005f) == "CW 00:00");
    }

    SECTION("A diff just outside the deadband keeps its sign") {
        REQUIRE(MockScrewsTiltState::diff_to_adjustment(-0.0015f) == "CCW 00:00");
    }

    SECTION("A rounded-up 60 minutes is not carried into the turn count") {
        // 0.4995 mm is 0.999 turns; round(0.999 * 60) is 60. Klipper prints
        // "00:60", never "01:00", so the mock must not carry either.
        REQUIRE(MockScrewsTiltState::diff_to_adjustment(0.4995f) == "CW 00:60");
        REQUIRE(MockScrewsTiltState::diff_to_adjustment(-0.4995f) == "CCW 00:60");
    }

    SECTION("The emitted 00:60 round-trips through the parser as a full turn") {
        ScrewTiltResult r;
        const std::string line = "// rear_left : x=30.0, y=200.0, z=2.00000 : adjust " +
                                 MockScrewsTiltState::diff_to_adjustment(0.4995f);
        REQUIRE(helix::parse_screws_tilt_line(line, r));
        // Magnitude, so a printer-database CW<->CCW override cannot flip it.
        REQUIRE(r.signed_adjustment_minutes().has_value());
        REQUIRE(std::abs(*r.signed_adjustment_minutes()) == 60);
    }
}

TEST_CASE("MockScrewsTiltState judges its simulated bed on the spread",
          "[calibration][screws_tilt][mock][1225]") {
    MockScrewsTiltState bed;

    SECTION("Spread is highest minus lowest, base included") {
        REQUIRE(bed.spread_mm() == Catch::Approx(0.23f)); // +0.15 down to -0.08
        REQUIRE_FALSE(bed.is_level());
    }

    SECTION("A mid-range base does not disguise the corner-to-corner error") {
        // Every screw is within 0.16 mm of the base, but the two extremes are
        // 0.23 mm apart — a per-screw magnitude test would call this level.
        REQUIRE_FALSE(bed.is_level(0.16f));
        REQUIRE(bed.is_level(0.25f));
    }

    SECTION("Simulated user adjustments converge the bed") {
        for (int i = 0; i < 12; i++) {
            bed.simulate_user_adjustments();
        }
        INFO("spread after 12 rounds: " << bed.spread_mm());
        REQUIRE(bed.is_level(0.03f));
    }

    SECTION("Reset restores the out-of-level starting bed") {
        for (int i = 0; i < 12; i++) {
            bed.simulate_user_adjustments();
        }
        bed.reset();
        REQUIRE(bed.spread_mm() == Catch::Approx(0.23f));
        REQUIRE(bed.get_probe_count() == 0);
    }
}
