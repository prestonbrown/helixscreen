// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_bed_mesh_collector.cpp
 * @brief Unit tests for BedMeshProgressCollector
 *
 * Tests the regex parsing, progress callbacks, completion detection,
 * and error handling for bed mesh calibration progress tracking.
 */

#include "bed_mesh_probe_parser.h"

#include <algorithm>
#include <cstdio>
#include <regex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Regex Parsing Tests (standalone, no collector instance needed)
// ============================================================================

namespace {

/**
 * @brief Parse a probe progress line and extract current/total values
 *
 * Handles both formats:
 * - "Probing point 5/25"
 * - "Probe point 5 of 25"
 *
 * @param line The G-code response line to parse
 * @param current Output: current probe number
 * @param total Output: total probe count
 * @return true if line matched and was parsed successfully
 */
bool parse_probe_progress(const std::string& line, int& current, int& total) {
    // Static regex for performance - handles both formats
    static const std::regex probe_regex(R"(Prob(?:ing point|e point) (\d+)[/\s]+(?:of\s+)?(\d+))");

    std::smatch match;
    if (std::regex_search(line, match, probe_regex) && match.size() == 3) {
        try {
            current = std::stoi(match[1].str());
            total = std::stoi(match[2].str());
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    return false;
}

/**
 * @brief Check if a line indicates mesh calibration completion
 */
bool is_completion_line(const std::string& line) {
    // Case-insensitive check for completion markers
    return line.find("Mesh Bed Leveling Complete") != std::string::npos ||
           line.find("Mesh bed leveling complete") != std::string::npos ||
           (line.find("BED_MESH_CALIBRATE") != std::string::npos &&
            line.find("ok") != std::string::npos);
}

/**
 * @brief Check if a line indicates an error
 */
bool is_error_line(const std::string& line) {
    return line.rfind("!! ", 0) == 0 ||              // Emergency errors start with "!! "
           line.rfind("Error:", 0) == 0 ||           // Standard errors
           line.find("error:") != std::string::npos; // Python tracebacks
}

} // namespace

// ============================================================================
// Regex Parsing Tests
// ============================================================================

TEST_CASE("BedMeshCollector parses 'Probing point X/Y' format", "[bed_mesh_collector][regex]") {
    int current = 0, total = 0;

    SECTION("simple case") {
        REQUIRE(parse_probe_progress("Probing point 5/25", current, total));
        REQUIRE(current == 5);
        REQUIRE(total == 25);
    }

    SECTION("first point") {
        REQUIRE(parse_probe_progress("Probing point 1/25", current, total));
        REQUIRE(current == 1);
        REQUIRE(total == 25);
    }

    SECTION("last point") {
        REQUIRE(parse_probe_progress("Probing point 25/25", current, total));
        REQUIRE(current == 25);
        REQUIRE(total == 25);
    }

    SECTION("large grid") {
        REQUIRE(parse_probe_progress("Probing point 49/100", current, total));
        REQUIRE(current == 49);
        REQUIRE(total == 100);
    }

    SECTION("with prefix text") {
        REQUIRE(parse_probe_progress("// Probing point 3/9", current, total));
        REQUIRE(current == 3);
        REQUIRE(total == 9);
    }
}

TEST_CASE("BedMeshCollector parses 'Probe point X of Y' format", "[bed_mesh_collector][regex]") {
    int current = 0, total = 0;

    SECTION("simple case") {
        REQUIRE(parse_probe_progress("Probe point 5 of 25", current, total));
        REQUIRE(current == 5);
        REQUIRE(total == 25);
    }

    SECTION("first point") {
        REQUIRE(parse_probe_progress("Probe point 1 of 16", current, total));
        REQUIRE(current == 1);
        REQUIRE(total == 16);
    }

    SECTION("last point") {
        REQUIRE(parse_probe_progress("Probe point 16 of 16", current, total));
        REQUIRE(current == 16);
        REQUIRE(total == 16);
    }

    SECTION("large grid") {
        REQUIRE(parse_probe_progress("Probe point 77 of 144", current, total));
        REQUIRE(current == 77);
        REQUIRE(total == 144);
    }
}

TEST_CASE("BedMeshCollector rejects invalid lines", "[bed_mesh_collector][regex]") {
    int current = 0, total = 0;

    SECTION("empty string") {
        REQUIRE_FALSE(parse_probe_progress("", current, total));
    }

    SECTION("unrelated gcode output") {
        REQUIRE_FALSE(parse_probe_progress("ok", current, total));
        REQUIRE_FALSE(parse_probe_progress("G28", current, total));
        REQUIRE_FALSE(parse_probe_progress("M104 S200", current, total));
    }

    SECTION("similar but different text") {
        REQUIRE_FALSE(parse_probe_progress("Moving to point 5/25", current, total));
        REQUIRE_FALSE(parse_probe_progress("Point 5 of 25", current, total));
    }

    SECTION("malformed numbers") {
        REQUIRE_FALSE(parse_probe_progress("Probing point abc/def", current, total));
    }
}

// ============================================================================
// Completion Detection Tests
// ============================================================================

TEST_CASE("BedMeshCollector detects completion markers", "[bed_mesh_collector][completion]") {
    SECTION("standard completion message") {
        REQUIRE(is_completion_line("Mesh Bed Leveling Complete"));
    }

    SECTION("lowercase variant") {
        REQUIRE(is_completion_line("Mesh bed leveling complete"));
    }

    SECTION("with prefix") {
        REQUIRE(is_completion_line("// Mesh Bed Leveling Complete"));
    }

    SECTION("non-completion lines") {
        REQUIRE_FALSE(is_completion_line("ok"));
        REQUIRE_FALSE(is_completion_line("Probing point 5/25"));
        REQUIRE_FALSE(is_completion_line("Moving to bed mesh position"));
    }
}

// ============================================================================
// Error Detection Tests
// ============================================================================

TEST_CASE("BedMeshCollector detects error markers", "[bed_mesh_collector][error]") {
    SECTION("emergency error prefix") {
        REQUIRE(is_error_line("!! Probe triggered prior to move"));
        REQUIRE(is_error_line("!! Timer too close"));
    }

    SECTION("standard error prefix") {
        REQUIRE(is_error_line("Error: Probe failed to trigger"));
        REQUIRE(is_error_line("Error: Heater extruder not heating at expected rate"));
    }

    SECTION("python traceback error") {
        REQUIRE(is_error_line("klippy/extras/probe.py:123: error: probe not found"));
    }

    SECTION("non-error lines") {
        REQUIRE_FALSE(is_error_line("ok"));
        REQUIRE_FALSE(is_error_line("Probing point 5/25"));
        REQUIRE_FALSE(is_error_line("// Comment with error word"));
        REQUIRE_FALSE(is_error_line("B:60.0 /60.0 T0:200.0 /200.0"));
    }
}

// ============================================================================
// Progress Callback Integration Tests
// ============================================================================

TEST_CASE("BedMeshCollector progress callback receives correct values",
          "[bed_mesh_collector][callback]") {
    // Simulate what the collector would do with parsed values
    std::vector<std::pair<int, int>> progress_calls;

    auto on_progress = [&progress_calls](int current, int total) {
        progress_calls.push_back({current, total});
    };

    // Simulate parsing a sequence of lines
    std::vector<std::string> lines = {
        "// Moving to first probe position",
        "Probing point 1/9",
        "Probing point 2/9",
        "Probing point 3/9",
        "// Probe result: z=0.125",
        "Probing point 4/9",
        "Probing point 5/9",
        "Probing point 6/9",
        "Probing point 7/9",
        "Probing point 8/9",
        "Probing point 9/9",
        "Mesh Bed Leveling Complete",
    };

    for (const auto& line : lines) {
        int current = 0, total = 0;
        if (parse_probe_progress(line, current, total)) {
            on_progress(current, total);
        }
    }

    REQUIRE(progress_calls.size() == 9);
    REQUIRE(progress_calls[0] == std::make_pair(1, 9));
    REQUIRE(progress_calls[4] == std::make_pair(5, 9));
    REQUIRE(progress_calls[8] == std::make_pair(9, 9));
}

TEST_CASE("BedMeshCollector handles mixed format progress lines",
          "[bed_mesh_collector][callback]") {
    std::vector<std::pair<int, int>> progress_calls;

    auto on_progress = [&progress_calls](int current, int total) {
        progress_calls.push_back({current, total});
    };

    // Some printers might use different formats
    std::vector<std::string> lines = {
        "Probe point 1 of 25",
        "Probing point 2/25",
        "Probe point 3 of 25",
        "Probing point 4/25",
    };

    for (const auto& line : lines) {
        int current = 0, total = 0;
        if (parse_probe_progress(line, current, total)) {
            on_progress(current, total);
        }
    }

    REQUIRE(progress_calls.size() == 4);
    // All should parse to same total
    for (const auto& call : progress_calls) {
        REQUIRE(call.second == 25);
    }
    REQUIRE(progress_calls[0].first == 1);
    REQUIRE(progress_calls[1].first == 2);
    REQUIRE(progress_calls[2].first == 3);
    REQUIRE(progress_calls[3].first == 4);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("BedMeshCollector handles edge case probe counts", "[bed_mesh_collector][edge]") {
    int current = 0, total = 0;

    SECTION("minimum grid (2x2 = 4 points)") {
        REQUIRE(parse_probe_progress("Probing point 1/4", current, total));
        REQUIRE(current == 1);
        REQUIRE(total == 4);
    }

    SECTION("large grid (20x20 = 400 points)") {
        REQUIRE(parse_probe_progress("Probing point 399/400", current, total));
        REQUIRE(current == 399);
        REQUIRE(total == 400);
    }

    SECTION("adaptive mesh with odd count") {
        REQUIRE(parse_probe_progress("Probing point 17/37", current, total));
        REQUIRE(current == 17);
        REQUIRE(total == 37);
    }
}

// ============================================================================
// Probe Samples Per Point (fallback "probe at" path)
// ============================================================================

/**
 * @brief Drive the real ProbePointCounter over a linear sweep of mesh points
 *
 * Firmware that doesn't emit "Probing point X/Y" still emits one
 * "probe at X,Y is z=Z" line per SAMPLE, so the collector has to collapse
 * samples into mesh points. Returns the (mesh_point, total) pair the collector
 * would report for every line, in order.
 *
 * This used to reimplement `ceil(count / samples)` locally, which is why it
 * stayed green while production had the #1224 bug — assert against real code.
 */
static std::vector<std::pair<int, int>> simulate_fallback_probing(int grid_points,
                                                                  int probe_samples) {
    std::vector<std::pair<int, int>> progress_calls;
    const int samples = std::max(probe_samples, 1);
    helix::ProbePointCounter counter(samples);

    // Each mesh point produces `samples` "probe at" lines at one (x,y).
    for (int point = 0; point < grid_points; ++point) {
        const double x = 20.0 + point * 15.0;
        const double y = 30.0;
        for (int s = 0; s < samples; ++s) {
            char line[128];
            std::snprintf(line, sizeof(line), "// probe at %.3f,%.3f is z=-0.031000", x, y);
            if (auto mesh_point = counter.feed(line)) {
                progress_calls.push_back({*mesh_point, grid_points});
            }
        }
    }
    return progress_calls;
}

TEST_CASE("Fallback probe count with samples=1 reports raw count",
          "[bed_mesh_collector][samples]") {
    auto calls = simulate_fallback_probing(25, 1);
    REQUIRE(calls.size() == 25);
    REQUIRE(calls.front() == std::make_pair(1, 25));
    REQUIRE(calls.back() == std::make_pair(25, 25));
}

TEST_CASE("Fallback probe count with samples=3 reports mesh points",
          "[bed_mesh_collector][samples]") {
    // 5x5 grid, 3 samples per point = 75 "probe at" lines
    auto calls = simulate_fallback_probing(25, 3);
    REQUIRE(calls.size() == 75);

    // First 3 samples all map to mesh point 1
    REQUIRE(calls[0] == std::make_pair(1, 25));
    REQUIRE(calls[1] == std::make_pair(1, 25));
    REQUIRE(calls[2] == std::make_pair(1, 25));

    // Samples 4-6 map to mesh point 2
    REQUIRE(calls[3] == std::make_pair(2, 25));
    REQUIRE(calls[5] == std::make_pair(2, 25));

    // Last sample maps to mesh point 25, not 75
    REQUIRE(calls.back() == std::make_pair(25, 25));
}

TEST_CASE("Fallback probe count with samples=5 reports mesh points",
          "[bed_mesh_collector][samples]") {
    // 3x3 grid, 5 samples per point = 45 "probe at" lines
    auto calls = simulate_fallback_probing(9, 5);
    REQUIRE(calls.size() == 45);
    REQUIRE(calls.back() == std::make_pair(9, 9));

    // After 10 samples (2 mesh points * 5 samples), should report point 2
    REQUIRE(calls[9] == std::make_pair(2, 9));
    // After 11 samples, should report point 3 (ceiling division)
    REQUIRE(calls[10] == std::make_pair(3, 9));
}

TEST_CASE("is_probe_result_line detects standard Klipper probe output",
          "[bed_mesh_collector][samples]") {
    REQUIRE(helix::is_probe_result_line("probe at 150.000,150.000 is z=1.234"));
    REQUIRE(helix::is_probe_result_line("probe at 0.000,0.000 is z=-0.050"));
    REQUIRE_FALSE(helix::is_probe_result_line("Probing point 5/25"));
    REQUIRE_FALSE(helix::is_probe_result_line("ok"));
}

// ============================================================================
// parse_probe_position — underpins sample deduplication
// ============================================================================

TEST_CASE("parse_probe_position extracts X,Y from comma-separated form",
          "[bed_mesh_collector][probe_position]") {
    auto p = helix::parse_probe_position("probe at 150.000,120.500 is z=-0.050");
    REQUIRE(p.has_value());
    REQUIRE(p->x == Catch::Approx(150.000));
    REQUIRE(p->y == Catch::Approx(120.500));
}

TEST_CASE("parse_probe_position handles 'x:'/'y:' prefix form (Snapmaker U1)",
          "[bed_mesh_collector][probe_position]") {
    // Snapmaker U1 Klipper emits this labeled form
    auto p = helix::parse_probe_position("probe at x: 181.474, y: 55.048 is z=-0.214167");
    REQUIRE(p.has_value());
    REQUIRE(p->x == Catch::Approx(181.474));
    REQUIRE(p->y == Catch::Approx(55.048));
}

TEST_CASE("parse_probe_position handles negative coordinates",
          "[bed_mesh_collector][probe_position]") {
    auto p = helix::parse_probe_position("probe at -5.500,-10.250 is z=0.000");
    REQUIRE(p.has_value());
    REQUIRE(p->x == Catch::Approx(-5.500));
    REQUIRE(p->y == Catch::Approx(-10.250));
}

TEST_CASE("parse_probe_position rejects non-probe lines", "[bed_mesh_collector][probe_position]") {
    REQUIRE_FALSE(helix::parse_probe_position("Probing point 5/25").has_value());
    REQUIRE_FALSE(helix::parse_probe_position("ok").has_value());
    REQUIRE_FALSE(helix::parse_probe_position("probe at is z=0").has_value());
}

// ============================================================================
// parse_adapted_probe_count — stock Klipper adaptive bed_mesh signal
// ============================================================================

TEST_CASE("parse_adapted_probe_count extracts N*M from Klipper line",
          "[bed_mesh_collector][adapted]") {
    SECTION("Bare form") {
        auto n = helix::parse_adapted_probe_count("Adapted probe count: 4,4");
        REQUIRE(n.has_value());
        REQUIRE(*n == 16);
    }

    SECTION("With gcode_response // prefix") {
        auto n = helix::parse_adapted_probe_count("// Adapted probe count: 5,3");
        REQUIRE(n.has_value());
        REQUIRE(*n == 15);
    }

    SECTION("Whitespace tolerance around comma") {
        auto n = helix::parse_adapted_probe_count("Adapted probe count: 6 , 6");
        REQUIRE(n.has_value());
        REQUIRE(*n == 36);
    }

    SECTION("Large grid (Voron 350)") {
        auto n = helix::parse_adapted_probe_count("Adapted probe count: 9,9");
        REQUIRE(n.has_value());
        REQUIRE(*n == 81);
    }
}

TEST_CASE("parse_adapted_probe_count rejects unrelated lines", "[bed_mesh_collector][adapted]") {
    REQUIRE_FALSE(helix::parse_adapted_probe_count("Probing point 5/25").has_value());
    REQUIRE_FALSE(helix::parse_adapted_probe_count("// bed_mesh: generated points").has_value());
    REQUIRE_FALSE(helix::parse_adapted_probe_count("Mesh X,Y: 10,10").has_value());
    REQUIRE_FALSE(helix::parse_adapted_probe_count("Adapted probe count: 0,4").has_value());
    REQUIRE_FALSE(helix::parse_adapted_probe_count("Adapted probe count: 4,0").has_value());
    REQUIRE_FALSE(helix::parse_adapted_probe_count("").has_value());
}

// ============================================================================
// Sample dedupe — drives the REAL helix::ProbePointCounter
// ============================================================================
//
// These previously ran against a local reimplementation of the dedupe rule,
// which is why they stayed green while the production collector did no dedupe
// at all and divided by a config-derived sample count instead (#1224). Feed the
// real counter real Klipper lines, or the tests prove nothing about shipping
// code.

namespace {

/// Render the line Klipper actually emits for one probe sample.
std::string probe_line(double x, double y, double z = -0.031) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "// probe at %.3f,%.3f is z=%.6f", x, y, z);
    return buf;
}

/**
 * @brief Drive a real ProbePointCounter over a full grid sweep
 *
 * @param configured_samples What the counter was TOLD (from configfile). Pass 1
 *                           to model a printer whose probe section we failed to
 *                           recognise — the #1224 case.
 * @param actual_samples     How many lines the firmware really emits per point.
 * @return points counted
 */
int sweep_grid(int grid_rows, int grid_cols, int actual_samples, int configured_samples = 1,
               double x_spacing = 46.1, double y_spacing = 41.62) {
    helix::ProbePointCounter counter(configured_samples);
    for (int r = 0; r < grid_rows; ++r) {
        for (int c = 0; c < grid_cols; ++c) {
            const double x = 43.173 + c * x_spacing;
            const double y = 55.048 + r * y_spacing;
            for (int s = 0; s < actual_samples; ++s) {
                counter.feed(probe_line(x, y));
            }
        }
    }
    return counter.points();
}

} // namespace

TEST_CASE("Qidi Q2: two touches per point does not double the count",
          "[bed_mesh_collector][dedupe][1224]") {
    // The reported bug: a 6x6 mesh ran past 36 because the Q2 probes each point
    // twice and its probe section is not one the configfile scan recognises, so
    // the collector was told samples=1 and counted raw lines.
    REQUIRE(sweep_grid(6, 6, /*actual_samples=*/2, /*configured_samples=*/1) == 36);
}

TEST_CASE("Sample dedupe survives an unknown probe section at any sample count",
          "[bed_mesh_collector][dedupe][1224]") {
    // Positional dedupe needs no configuration at all, which is the whole point:
    // the set of probe section names across firmware forks is open-ended.
    REQUIRE(sweep_grid(5, 5, 3, /*configured_samples=*/1) == 25);
    REQUIRE(sweep_grid(3, 3, 5, /*configured_samples=*/1) == 9);
}

TEST_CASE("Sample dedupe: tolerance retries at one point do not inflate the count",
          "[bed_mesh_collector][dedupe][1224]") {
    // samples_tolerance_retries re-probes the SAME point an unpredictable number
    // of times. A fixed divisor cannot model that; position dedupe absorbs it.
    helix::ProbePointCounter counter(2);
    for (int i = 0; i < 7; ++i) { // 7 samples at one point — not a multiple of 2
        counter.feed(probe_line(100.0, 100.0));
    }
    REQUIRE(counter.points() == 1);
    counter.feed(probe_line(150.0, 100.0));
    REQUIRE(counter.points() == 2);
}

TEST_CASE("Sample dedupe: 6x5 grid with samples=3 counts 30 points, not 90",
          "[bed_mesh_collector][dedupe]") {
    // Snapmaker U1: probe_count=[13,13] config, adaptive mesh probes 6x5=30
    // points, 3 samples each = 90 "probe at" lines.
    REQUIRE(sweep_grid(6, 5, 3, /*configured_samples=*/3) == 30);
}

TEST_CASE("Sample dedupe: 5x5 grid with samples=1 counts 25 points",
          "[bed_mesh_collector][dedupe]") {
    REQUIRE(sweep_grid(5, 5, 1, /*configured_samples=*/1) == 25);
}

TEST_CASE("Sample dedupe: 3x3 grid with samples=5 counts 9 points",
          "[bed_mesh_collector][dedupe]") {
    REQUIRE(sweep_grid(3, 3, 5, /*configured_samples=*/5) == 9);
}

TEST_CASE("Sample dedupe: tolerance distinguishes adjacent grid points",
          "[bed_mesh_collector][dedupe]") {
    // Neighboring probe positions 1mm apart must NOT collapse. Tolerance is
    // 0.05mm — repeated samples differ by exactly 0, real grid moves by mm.
    REQUIRE(sweep_grid(2, 2, 2, /*configured_samples=*/1, 1.0, 1.0) == 4);
}

TEST_CASE("ProbePointCounter reports the U1 'x:'/'y:' prefixed form",
          "[bed_mesh_collector][dedupe][1224]") {
    helix::ProbePointCounter counter(1);
    REQUIRE(counter.feed("// probe at x: 181.474, y: 55.048 is z=-0.031") == 1);
    REQUIRE(counter.feed("// probe at x: 181.474, y: 55.048 is z=-0.029") == 1);
    REQUIRE(counter.feed("// probe at x: 227.574, y: 55.048 is z=-0.030") == 2);
}

TEST_CASE("ProbePointCounter ignores lines that are not probe results",
          "[bed_mesh_collector][dedupe][1224]") {
    helix::ProbePointCounter counter(1);
    REQUIRE_FALSE(counter.feed("// Klipper state: Ready").has_value());
    REQUIRE_FALSE(counter.feed("Probing point 3/25").has_value());
    REQUIRE_FALSE(counter.feed("").has_value());
    REQUIRE(counter.points() == 0);
}

TEST_CASE("ProbePointCounter falls back to the divisor when coordinates do not parse",
          "[bed_mesh_collector][dedupe][1224]") {
    // A probe result line whose coordinates are unreadable still has to advance
    // progress; the configured sample count is the only signal left.
    helix::ProbePointCounter counter(2);
    const std::string bad = "// probe at nan,nan is z=-0.031";
    REQUIRE(counter.feed(bad) == 1); // ceil(1/2)
    REQUIRE(counter.feed(bad) == 1); // ceil(2/2)
    REQUIRE(counter.feed(bad) == 2); // ceil(3/2)
}

TEST_CASE("ProbePointCounter never counts backwards", "[bed_mesh_collector][dedupe][1224]") {
    // A malformed line landing mid-sweep must not drag a positional count down
    // to the divisor's smaller estimate — progress readouts only move forward.
    helix::ProbePointCounter counter(4);
    counter.feed(probe_line(10.0, 10.0));
    counter.feed(probe_line(20.0, 10.0));
    counter.feed(probe_line(30.0, 10.0));
    REQUIRE(counter.points() == 3);
    counter.feed("// probe at nan,nan is z=-0.031"); // ceil(4/4) = 1
    REQUIRE(counter.points() == 3);
}

TEST_CASE("Revisited point does not count twice", "[bed_mesh_collector][dedupe]") {
    // Consecutive-only dedupe collapses A A B but not A B A. Firmware that
    // returns to an earlier point after moving away must not gain a point.
    helix::ProbePointCounter counter(1);
    counter.feed(probe_line(10.0, 10.0));
    counter.feed(probe_line(60.0, 10.0));
    counter.feed(probe_line(10.0, 10.0));
    REQUIRE(counter.points() == 2);
}

TEST_CASE("Corner re-probe at sub-millimetre offsets does not inflate the count",
          "[bed_mesh_collector][dedupe][k2]") {
    // K2 Plus G29_RE_CHECK re-touches a mesh corner eight times, each round
    // sampling the four quadrant offsets around it. The offsets are +/-0.25mm,
    // far enough apart that consecutive-only dedupe at 0.05mm counted every
    // one: a 67-point mesh reported 147.
    helix::ProbePointCounter counter(1);
    counter.feed(probe_line(345.000, 47.500)); // the grid point, during the sweep
    counter.feed(probe_line(345.000, 302.500));
    REQUIRE(counter.points() == 2);

    for (int round = 0; round < 8; ++round) {
        for (double dx : {-0.25, 0.25}) {
            for (double dy : {-0.25, 0.25}) {
                counter.feed(probe_line(345.0 + dx, 47.5 + dy));
                counter.feed(probe_line(345.0 + dx, 302.5 + dy));
            }
        }
    }
    REQUIRE(counter.points() == 2);
}

TEST_CASE("Adjacent grid points stay distinct at the widened tolerance",
          "[bed_mesh_collector][dedupe]") {
    // The tolerance has to absorb 0.5mm of re-probe jitter without merging real
    // neighbours. The tightest mesh spacing any firmware ships is millimetres.
    helix::ProbePointCounter counter(1);
    counter.feed(probe_line(100.0, 100.0));
    counter.feed(probe_line(101.0, 100.0));
    counter.feed(probe_line(100.0, 101.0));
    REQUIRE(counter.points() == 3);
}

/**
 * @brief Replay a K2 Plus bed mesh the way the firmware actually emits it
 *
 * Reconstructed from klippy.log of the 2026-08-16 print
 * quattrobox_bottom_cover_ASA-GF (12:22:30-12:28:56). Three properties of that
 * stream broke the old consecutive-only dedupe, and all three are reproduced:
 *
 *  - Two "probe at" lines per touch. Creality reports the raw Z and the
 *    z_compensation-adjusted Z as separate lines at one position.
 *  - The sweep is adaptive. The configured grid is 9x9, but the firmware
 *    trimmed it to the print area and skipped five cells of the Y=5 row, so
 *    it probed 67 points and no configfile number predicts that.
 *  - It closes with eight G29_RE_CHECK rounds alternating between two corners
 *    it already probed, each round sampling the four +/-0.25mm quadrant
 *    offsets. Those are already-seen points, not new ones.
 *
 * The count must equal the grid the sweep visited, not the lines fed.
 */
static std::vector<std::pair<double, double>> k2_adaptive_grid() {
    const double xs[] = {5.0, 47.5, 90.0, 132.5, 175.0, 217.5, 260.0, 302.5, 345.0};
    const double ys[] = {5.0, 47.5, 90.0, 132.5, 175.0, 217.5, 260.0, 302.5};
    std::vector<std::pair<double, double>> grid;
    for (double x : xs) {
        for (double y : ys) {
            // The five Y=5 cells past mid-bed fall outside the print area and
            // were never touched on the captured run.
            if (y == 5.0 && x > 132.5) {
                continue;
            }
            grid.push_back({x, y});
        }
    }
    return grid;
}

/// Both lines Klipper emits for one K2 probe touch.
static void feed_k2_touch(helix::ProbePointCounter& counter, double x, double y) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "// probe at %.3f,%.3f is z=-0.647500 z_compensation=0.050000",
                  x, y);
    counter.feed(buf);
    std::snprintf(buf, sizeof(buf), "// probe at %.3f,%.3f is z=-0.597500", x, y);
    counter.feed(buf);
}

TEST_CASE("K2 Plus adaptive mesh replay counts the grid, not the sample lines",
          "[bed_mesh_collector][dedupe][k2]") {
    constexpr int kRecheckRounds = 8;
    constexpr int kRecheckCorners = 2;
    constexpr int kQuadrantOffsets = 4;
    constexpr int kLinesPerTouch = 2;

    const auto grid = k2_adaptive_grid();
    helix::ProbePointCounter counter(1);

    for (const auto& [x, y] : grid) {
        feed_k2_touch(counter, x, y);
    }
    const int after_sweep = counter.points();

    // Eight G29_RE_CHECK rounds over two corners the sweep already covered.
    for (int round = 0; round < kRecheckRounds; ++round) {
        for (double dx : {-0.25, 0.25}) {
            for (double dy : {-0.25, 0.25}) {
                feed_k2_touch(counter, 345.0 + dx, 47.5 + dy);
                feed_k2_touch(counter, 345.0 + dx, 302.5 + dy);
            }
        }
    }

    // The sweep itself must collapse two lines per touch down to one point...
    REQUIRE(after_sweep == static_cast<int>(grid.size()));
    // ...and the re-check rounds must add nothing, having touched no new point.
    REQUIRE(counter.points() == static_cast<int>(grid.size()));

    // Guard the premise. Spelling out the lines fed keeps the two quantities
    // visibly different, so a counter that regressed to counting lines — or to
    // any fixed divisor of them — cannot coincidentally land on the grid size.
    const int touches =
        static_cast<int>(grid.size()) + kRecheckRounds * kRecheckCorners * kQuadrantOffsets;
    REQUIRE(counter.sample_lines() == touches * kLinesPerTouch);
    REQUIRE(counter.sample_lines() > counter.points() * 3);
}

TEST_CASE("ProbePointCounter reset clears both counters", "[bed_mesh_collector][dedupe][1224]") {
    helix::ProbePointCounter counter(1);
    counter.feed(probe_line(10.0, 10.0));
    counter.feed(probe_line(20.0, 10.0));
    REQUIRE(counter.points() == 2);
    REQUIRE(counter.sample_lines() == 2);
    counter.reset();
    REQUIRE(counter.points() == 0);
    REQUIRE(counter.sample_lines() == 0);
    // And the position history is cleared, so the first point after reset counts.
    REQUIRE(counter.feed(probe_line(20.0, 10.0)) == 1);
}
