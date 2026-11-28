// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_snapmaker_screws_tilt.cpp
 * @brief Mapping the Snapmaker U1 [auto_screws_tilt_adjust] status object
 *
 * The U1 replaces upstream SCREWS_TILT_CALCULATE with a firmware module whose
 * result arrives as a status object instead of console lines. These tests pin
 * the mapping from that object to the ScrewTiltResult set the rest of the
 * screws-tilt UI already consumes:
 *
 *   - diff_mm = target_z - base_point_i; positive renders CW, negative CCW
 *     (the same sign convention as upstream, which uses z_base - z)
 *   - target_z is read from the status field, never recomputed from the base
 *     points: the firmware's adjust_screws path re-derives it as a midrange
 *     after a tuning round, so the field is not always the mean
 *   - magnitude in clock-minutes via screw_minutes_for_mm(mm, pitch)
 *   - NO screw is a reference: upstream picks screw1 as a zero base, Snapmaker
 *     measures every screw against the MEAN of the four (target_z), so there
 *     is no base screw to exempt from adjustment
 *   - z_height is the probed base_point_i
 *   - a missing/null base_point or target_z is an error, never a screw silently
 *     zeroed to read as a level bed
 *
 * Coordinates and names come from configfile.settings.auto_screws_tilt_adjust
 * (screw1..4 as "x,y" and screw1_name..4_name).
 */

#include "calibration_types.h"
#include "snapmaker_screws_tilt.h"

#include <json.hpp> // nlohmann/json from libhv

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;

namespace {

// Real U1 coordinates and names, verbatim from the device's
// configfile.settings.auto_screws_tilt_adjust section.
const json kU1ConfigSection = {
    {"screw1", "15,15"},
    {"screw2", "255,15"},
    {"screw3", "255,255"},
    {"screw4", "15,255"},
    {"screw1_name", "front left screw"},
    {"screw2_name", "front right screw"},
    {"screw3_name", "rear right screw"},
    {"screw4_name", "rear left screw"},
};

/// A status object in the shape the U1 publishes. target_z is the mean of the
/// four base points, as the firmware defines it.
json make_status(float target_z, float base1, float base2, float base3, float base4) {
    return {
        {"target_z", target_z},
        {"base_point1", base1},
        {"base_point2", base2},
        {"base_point3", base3},
        {"base_point4", base4},
        // Fields the mapper must tolerate without needing; a parser that
        // rejects the payload because of them would break on every real U1.
        {"min_z", base1},
        {"max_z", base4},
        {"current_point", 4},
    };
}

/**
 * A clearly tilted bed: diffs of +0.25 / -0.15 / +0.075 / -0.175 mm about the
 * mean. Signed minutes on an M3 (0.5mm) pitch: +30 / -18 / +9 / -21.
 */
const json kTiltedStatus = [] { return make_status(0.4f, 0.15f, 0.55f, 0.325f, 0.575f); }();

/// The same bed within 0.015mm of level: diffs of ±0.015 / ±0.010 mm.
const json kLevelStatus = [] { return make_status(0.4f, 0.385f, 0.415f, 0.39f, 0.41f); }();

/**
 * The discriminating fixture: target_z deliberately OFFSET from the mean of
 * the base points (0.5 vs the mean 0.4). The firmware's adjust_screws path
 * re-derives target_z as a midrange after a tuning round, so the mapper must
 * follow the field, and only this shape tells the two apart.
 */
const json kOffsetTargetStatus = [] { return make_status(0.5f, 0.15f, 0.55f, 0.325f, 0.575f); }();

/// Measured on the U1, where target_z and the mean coincide: base points
/// -0.18083 / -0.53958 / -0.11625 / -0.295 with target_z exactly their mean.
const json kDeviceMeasuredStatus = [] {
    return make_status(-0.28292f, -0.18083f, -0.53958f, -0.11625f, -0.295f);
}();

/// Expected signed minutes for one screw, derived from the spec's arithmetic
/// (target_z - base_point, M3 pitch) rather than hardcoded.
int expected_minutes(float target_z, float base_point) {
    return screw_minutes_for_mm(target_z - base_point, kScrewPitchM3Mm);
}

} // namespace

// ============================================================================
// Direction, coordinates, names, reference flags
// ============================================================================

TEST_CASE("parse_auto_screws_tilt maps status fields to ScrewTiltResult",
          "[calibration][screws_tilt][auto_screws]") {
    const AutoScrewsTiltResults res =
        parse_auto_screws_tilt(kTiltedStatus, kU1ConfigSection, kScrewPitchM3Mm);

    REQUIRE(res.ok());
    REQUIRE(res.screws.size() == 4);

    SECTION("coords and names come from the configfile section, in screw order") {
        REQUIRE(res.screws[0].screw_name == "front left screw");
        REQUIRE(res.screws[0].x_pos == Catch::Approx(15.0f));
        REQUIRE(res.screws[0].y_pos == Catch::Approx(15.0f));

        REQUIRE(res.screws[1].screw_name == "front right screw");
        REQUIRE(res.screws[1].x_pos == Catch::Approx(255.0f));
        REQUIRE(res.screws[1].y_pos == Catch::Approx(15.0f));

        REQUIRE(res.screws[2].screw_name == "rear right screw");
        REQUIRE(res.screws[2].x_pos == Catch::Approx(255.0f));
        REQUIRE(res.screws[2].y_pos == Catch::Approx(255.0f));

        REQUIRE(res.screws[3].screw_name == "rear left screw");
        REQUIRE(res.screws[3].x_pos == Catch::Approx(15.0f));
        REQUIRE(res.screws[3].y_pos == Catch::Approx(255.0f));
    }

    SECTION("diff = target_z - base_point: above the mean is CW, below is CCW") {
        // Screw 1 sits 0.25mm above target_z, screw 2 0.15mm below.
        REQUIRE(res.screws[0].signed_adjustment_minutes() == expected_minutes(0.4f, 0.15f));
        REQUIRE(*res.screws[0].signed_adjustment_minutes() > 0);
        REQUIRE(res.screws[1].signed_adjustment_minutes() == expected_minutes(0.4f, 0.55f));
        REQUIRE(*res.screws[1].signed_adjustment_minutes() < 0);
        REQUIRE(res.screws[2].signed_adjustment_minutes() == expected_minutes(0.4f, 0.325f));
        REQUIRE(*res.screws[2].signed_adjustment_minutes() > 0);
        REQUIRE(res.screws[3].signed_adjustment_minutes() == expected_minutes(0.4f, 0.575f));
        REQUIRE(*res.screws[3].signed_adjustment_minutes() < 0);
    }

    SECTION("adjustment strings use the existing CW/CCW TT:MM format") {
        REQUIRE(res.screws[0].adjustment == "CW 00:30");
        REQUIRE(res.screws[1].adjustment == "CCW 00:18");
        REQUIRE(res.screws[2].adjustment == "CW 00:09");
        REQUIRE(res.screws[3].adjustment == "CCW 00:21");
    }

    SECTION("z_height is the probed base_point") {
        REQUIRE(res.screws[0].z_height == Catch::Approx(0.15f));
        REQUIRE(res.screws[1].z_height == Catch::Approx(0.55f));
        REQUIRE(res.screws[2].z_height == Catch::Approx(0.325f));
        REQUIRE(res.screws[3].z_height == Catch::Approx(0.575f));
    }

    SECTION("no screw is a reference - the U1 measures against the mean") {
        // Upstream's first screw is a zero base; Snapmaker's target_z is the
        // mean of all four, so every screw can carry a real adjustment. A
        // reference flag here would hide a genuine correction from the user.
        for (const auto& screw : res.screws) {
            INFO("screw " << screw.screw_name);
            REQUIRE_FALSE(screw.is_reference);
        }
    }
}

// ============================================================================
// target_z comes from the field, not a recomputed mean
// ============================================================================

TEST_CASE("parse_auto_screws_tilt reads the target_z field, not a recomputed mean",
          "[calibration][screws_tilt][auto_screws]") {
    SECTION("a target_z offset from the mean drives the diffs") {
        // Preconditions: the fixture really discriminates - following the
        // field and following the mean must give different minutes for every
        // screw, or this section would pass against a mean-recomputing mapper.
        const float base_points[] = {0.15f, 0.55f, 0.325f, 0.575f};
        for (float base : base_points) {
            INFO("base " << base);
            REQUIRE(expected_minutes(0.5f, base) != expected_minutes(0.4f, base));
        }

        const AutoScrewsTiltResults res =
            parse_auto_screws_tilt(kOffsetTargetStatus, kU1ConfigSection, kScrewPitchM3Mm);
        REQUIRE(res.ok());
        REQUIRE(res.screws.size() == 4);

        for (size_t i = 0; i < 4; i++) {
            INFO("screw " << res.screws[i].screw_name);
            REQUIRE(res.screws[i].signed_adjustment_minutes() ==
                    expected_minutes(0.5f, base_points[i]));
        }
    }

    SECTION("the measured U1 payload, where field and mean coincide, still maps") {
        const AutoScrewsTiltResults res =
            parse_auto_screws_tilt(kDeviceMeasuredStatus, kU1ConfigSection, kScrewPitchM3Mm);
        REQUIRE(res.ok());
        REQUIRE(res.screws.size() == 4);
        REQUIRE(res.screws[1].z_height == Catch::Approx(-0.53958f));

        const float base_points[] = {-0.18083f, -0.53958f, -0.11625f, -0.295f};
        for (size_t i = 0; i < 4; i++) {
            INFO("screw " << res.screws[i].screw_name);
            REQUIRE(res.screws[i].signed_adjustment_minutes() ==
                    expected_minutes(-0.28292f, base_points[i]));
        }
    }
}

// ============================================================================
// Pitch invariance - the property that lets us ship without the U1's real
// wheel thread pitch
// ============================================================================

TEST_CASE("parse_auto_screws_tilt verdicts are invariant to thread pitch",
          "[calibration][screws_tilt][auto_screws]") {
    // The M3 and M6 pitches are the extremes of Klipper's table; the U1's real
    // wheel thread is unknown, so the level/tilt verdict must not depend on it.
    // The spread in minutes DOES depend on pitch (the same millimetres are more
    // minutes on a finer thread) - only the verdict is invariant.

    SECTION("a tilted bed needs adjustment at every pitch") {
        const auto m3 = parse_auto_screws_tilt(kTiltedStatus, kU1ConfigSection, kScrewPitchM3Mm);
        const auto m6 = parse_auto_screws_tilt(kTiltedStatus, kU1ConfigSection, kScrewPitchM6Mm);
        REQUIRE(m3.ok());
        REQUIRE(m3.screws.size() == 4);
        REQUIRE(m6.ok());
        REQUIRE(m6.screws.size() == 4);

        const ScrewLevelReport r3 = evaluate_screw_level(m3.screws, kScrewPitchM3Mm);
        const ScrewLevelReport r6 = evaluate_screw_level(m6.screws, kScrewPitchM6Mm);

        REQUIRE(r3.verdict == ScrewLevelVerdict::NEEDS_ADJUSTMENT);
        REQUIRE(r6.verdict == ScrewLevelVerdict::NEEDS_ADJUSTMENT);
        REQUIRE(r3.spread_minutes != r6.spread_minutes);
    }

    SECTION("a level bed reads level at every pitch") {
        const auto m3 = parse_auto_screws_tilt(kLevelStatus, kU1ConfigSection, kScrewPitchM3Mm);
        const auto m6 = parse_auto_screws_tilt(kLevelStatus, kU1ConfigSection, kScrewPitchM6Mm);
        REQUIRE(m3.ok());
        REQUIRE(m6.ok());

        const ScrewLevelReport r3 = evaluate_screw_level(m3.screws, kScrewPitchM3Mm);
        const ScrewLevelReport r6 = evaluate_screw_level(m6.screws, kScrewPitchM6Mm);

        REQUIRE(r3.verdict == ScrewLevelVerdict::LEVEL);
        REQUIRE(r6.verdict == ScrewLevelVerdict::LEVEL);
        REQUIRE(r3.spread_minutes != r6.spread_minutes);
    }
}

// ============================================================================
// Unusable payloads are errors, never zero-valued screws
// ============================================================================

TEST_CASE("parse_auto_screws_tilt fails loudly on unusable payloads",
          "[calibration][screws_tilt][auto_screws]") {
    SECTION("a missing base_point is an error, not a screw zeroed to target") {
        json status = make_status(0.4f, 0.15f, 0.55f, 0.325f, 0.575f);
        status.erase("base_point3");

        const AutoScrewsTiltResults res =
            parse_auto_screws_tilt(status, kU1ConfigSection, kScrewPitchM3Mm);
        REQUIRE_FALSE(res.ok());
        REQUIRE(res.screws.empty());
        REQUIRE(res.error.find("base_point3") != std::string::npos);
    }

    SECTION("a null base_point is an error") {
        // Klipper emits JSON nulls for keys a printer does not define.
        json status = make_status(0.4f, 0.15f, 0.55f, 0.325f, 0.575f);
        status["base_point2"] = nullptr;

        const AutoScrewsTiltResults res =
            parse_auto_screws_tilt(status, kU1ConfigSection, kScrewPitchM3Mm);
        REQUIRE_FALSE(res.ok());
        REQUIRE(res.screws.empty());
        REQUIRE(res.error.find("base_point2") != std::string::npos);
    }

    SECTION("a missing target_z is an error") {
        json status = make_status(0.4f, 0.15f, 0.55f, 0.325f, 0.575f);
        status.erase("target_z");

        const AutoScrewsTiltResults res =
            parse_auto_screws_tilt(status, kU1ConfigSection, kScrewPitchM3Mm);
        REQUIRE_FALSE(res.ok());
        REQUIRE(res.screws.empty());
        REQUIRE(res.error.find("target_z") != std::string::npos);
    }

    SECTION("a null target_z is an error") {
        json status = make_status(0.4f, 0.15f, 0.55f, 0.325f, 0.575f);
        status["target_z"] = nullptr;

        const AutoScrewsTiltResults res =
            parse_auto_screws_tilt(status, kU1ConfigSection, kScrewPitchM3Mm);
        REQUIRE_FALSE(res.ok());
        REQUIRE(res.screws.empty());
        REQUIRE(res.error.find("target_z") != std::string::npos);
    }

    SECTION("an errored payload can never evaluate as a level bed") {
        json status = make_status(0.4f, 0.15f, 0.55f, 0.325f, 0.575f);
        status.erase("base_point1");

        const AutoScrewsTiltResults res =
            parse_auto_screws_tilt(status, kU1ConfigSection, kScrewPitchM3Mm);
        REQUIRE_FALSE(res.ok());
        // The downstream judge must not render a partial set as green.
        REQUIRE(evaluate_screw_level(res.screws, kScrewPitchM3Mm).verdict ==
                ScrewLevelVerdict::PARSE_ERROR);
    }
}

// ============================================================================
// Held-state predicate: is a leftover calibration state safe to clear?
// ============================================================================

TEST_CASE("auto_screws stale_calibration_state decides a held state is safe to clear",
          "[calibration][screws_tilt][auto_screws]") {
    using helix::snapmaker::screws_tilt::kMainStateScrewsTiltAdjust;
    using helix::snapmaker::screws_tilt::stale_calibration_state;

    SECTION("a terminal or never-started probe step is stale") {
        for (const char* step :
             {"adjust_idle", "adjust_complete", "adjust_failed", "adjust_homing_failed",
              "adjust_plate_detection_error", "adjust_aborted"}) {
            CAPTURE(step);
            REQUIRE(stale_calibration_state(kMainStateScrewsTiltAdjust, step));
        }
    }

    SECTION("every other probe step means work in flight - never stale") {
        for (const char* step :
             {"adjust_start", "adjust_homing", "adjust_homing_done", "adjust_plate_detecting",
              "adjust_plate_detected", "adjust_reset_to_initial", "adjust_probe_refpoint",
              "adjust_refpoint_done", "adjust_aborting", "adjust_probing", "adjust_wait_manual",
              "adjust_next_point_adjust", "adjust_verify"}) {
            CAPTURE(step);
            REQUIRE_FALSE(stale_calibration_state(kMainStateScrewsTiltAdjust, step));
        }
    }

    SECTION("an unknown probe step is not stale") {
        REQUIRE_FALSE(stale_calibration_state(kMainStateScrewsTiltAdjust, "adjust_bogus"));
        REQUIRE_FALSE(stale_calibration_state(kMainStateScrewsTiltAdjust, ""));
    }

    SECTION("a main_state that is not SCREWS_TILT_ADJUST is never ours to clear") {
        REQUIRE_FALSE(stale_calibration_state(0, "adjust_idle"));
        REQUIRE_FALSE(stale_calibration_state(3, "adjust_complete"));
    }
}

// ============================================================================
// Field accessors over status snapshots and query responses
// ============================================================================

namespace {

/// Shape of a printer.objects.query response carrying @p status.
json query_response(const json& status) {
    return {{"result", {{"status", status}}}};
}

} // namespace

TEST_CASE("auto_screws main_state_from_status reads the one field we subscribe for",
          "[calibration][screws_tilt][auto_screws]") {
    using helix::snapmaker::screws_tilt::main_state_from_status;

    SECTION("present and numeric") {
        const json status = {{"machine_state_manager", {{"main_state", 8}}}};
        REQUIRE(main_state_from_status(status) == std::optional<int>(8));
    }

    SECTION("zero is a value, not an absence") {
        const json status = {{"machine_state_manager", {{"main_state", 0}}}};
        REQUIRE(main_state_from_status(status) == std::optional<int>(0));
    }

    SECTION("absent object, absent field, non-numeric field are all unknown") {
        REQUIRE_FALSE(main_state_from_status(json{{"toolhead", {{"x", 1}}}}).has_value());
        REQUIRE_FALSE(
            main_state_from_status(json{{"machine_state_manager", json::object()}}).has_value());
        REQUIRE_FALSE(main_state_from_status(json{{"machine_state_manager", {{"main_state", "8"}}}})
                          .has_value());
    }
}

TEST_CASE("auto_screws plate_still_on_bed matches the not-removed failure",
          "[calibration][screws_tilt][auto_screws]") {
    using helix::snapmaker::screws_tilt::plate_still_on_bed;

    SECTION("the fault code, wrapped however Moonraker carries it") {
        REQUIRE(plate_still_on_bed(
            "Klippy Host Error: '0003-0530-0000-0011: The plate has not been removed'"));
    }

    SECTION("a phrase without a code does not match") {
        // The code is the identity; the wording is the firmware's to change.
        REQUIRE_FALSE(plate_still_on_bed("The plate has not been removed"));
    }

    SECTION("any other failure is a genuine detection problem") {
        REQUIRE_FALSE(plate_still_on_bed("inductance coil fault"));
        REQUIRE_FALSE(plate_still_on_bed(""));
        // The opposite assertion's error: the sheet is OFF and PRESENCE=1
        // was asked to confirm it ON. Not our verdict.
        REQUIRE_FALSE(plate_still_on_bed("0003-0530-0000-0010: The plate has been removed"));
    }
}

TEST_CASE("auto_screws results_from_query maps a probe-result query response",
          "[calibration][screws_tilt][auto_screws]") {
    using helix::snapmaker::screws_tilt::results_from_query;

    SECTION("status object plus configfile section map to screw results") {
        const json response = query_response(
            {{helix::snapmaker::screws_tilt::kModuleName, kTiltedStatus},
             {"configfile", {{"settings", {{"auto_screws_tilt_adjust", kU1ConfigSection}}}}}});

        const AutoScrewsTiltResults res = results_from_query(response);
        REQUIRE(res.ok());
        REQUIRE(res.screws.size() == 4);
        REQUIRE(res.screws[0].screw_name == "front left screw");
    }

    SECTION("a missing configfile section still maps - coords and names are optional") {
        const json response =
            query_response({{helix::snapmaker::screws_tilt::kModuleName, kTiltedStatus}});

        const AutoScrewsTiltResults res = results_from_query(response);
        REQUIRE(res.ok());
        REQUIRE(res.screws.size() == 4);
        REQUIRE(res.screws[0].screw_name.empty());
    }

    SECTION("a missing status object is an error, never an empty level bed") {
        const AutoScrewsTiltResults res =
            results_from_query(query_response({{"configfile", json::object()}}));
        REQUIRE_FALSE(res.ok());
        REQUIRE(res.screws.empty());
        REQUIRE(evaluate_screw_level(res.screws, kScrewPitchM3Mm).verdict ==
                ScrewLevelVerdict::PARSE_ERROR);
    }
}

TEST_CASE("screw coordinates read like scanf's \"%f,%f\", keeping a readable x",
          "[calibration][screws_tilt][auto_screws]") {
    const auto coords = [](const char* text) {
        json section = kU1ConfigSection;
        section["screw1"] = text;
        const auto res = parse_auto_screws_tilt(kTiltedStatus, section, kScrewPitchM3Mm);
        REQUIRE(res.ok());
        return std::make_pair(res.screws[0].x_pos, res.screws[0].y_pos);
    };
    const auto unset = coords("garbage");

    CHECK(coords("-15.5, 20").first == Catch::Approx(-15.5f));
    CHECK(coords("-15.5, 20").second == Catch::Approx(20.0f));
    CHECK(coords("  7,8 extra").second == Catch::Approx(8.0f));

    // y malformed or missing: x still lands, y keeps its default.
    CHECK(coords("40,").first == Catch::Approx(40.0f));
    CHECK(coords("40,").second == unset.second);
    CHECK(coords("40;50").first == Catch::Approx(40.0f));
    CHECK(coords("40;50").second == unset.second);
    CHECK(coords("40").first == Catch::Approx(40.0f));
    // x malformed: neither lands.
    CHECK(coords("x,50").first == unset.first);
    CHECK(coords("x,50").second == unset.second);
    CHECK(coords("").second == unset.second);
}
