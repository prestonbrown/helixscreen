// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_input_shaper_chart.cpp
 * @brief Unit tests for the Klipper shaper-calibration CSV parser
 *
 * parse_shaper_csv() is what stands between Klipper's calibration CSV and the
 * frequency-response chart: frequency bins, the raw PSD for the requested axis,
 * and one filtered response curve per fitted shaper.
 *
 * These tests assert on ShaperCsvData directly. They used to route everything
 * through a local build_result_from_csv() that "mirrored the collector logic" —
 * a copy of src/api/moonraker_advanced_api.cpp:1332-1354 that dropped its
 * `if (!csv_data.frequencies.empty())` gate and the else-branch that raises
 * `chart_data_unavailable`. A mirror that has silently diverged from the code it
 * mirrors tests nothing but itself.
 */

#include "../../include/calibration_types.h"
#include "../../include/shaper_csv_parser.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;
namespace fs = std::filesystem;

// ============================================================================
// Test Helpers
// ============================================================================

namespace {

/// RAII temp file that auto-deletes on destruction
struct TempCsvFile {
    std::string path;

    explicit TempCsvFile(const std::string& content) {
        path = (fs::temp_directory_path() / ("test_is_chart_" +
                                             std::to_string(std::hash<std::string>{}(content) ^
                                                            reinterpret_cast<uintptr_t>(this)) +
                                             ".csv"))
                   .string();
        std::ofstream out(path);
        out << content;
    }

    ~TempCsvFile() {
        std::remove(path.c_str());
    }
};

/// Header of a standard Klipper calibrate_shaper.py CSV (real format, no marker)
const char* CSV_HEADER = "freq, psd_x, psd_y, psd_z, psd_xyz, zv(59.0), mzv(53.8), "
                         "ei(56.2), 2hump_ei(71.8), 3hump_ei(89.6)\n";

/// Realistic CSV content matching Klipper's calibrate_shaper.py output
const char* REALISTIC_CSV =
    "freq, psd_x, psd_y, psd_z, psd_xyz, zv(59.0), mzv(53.8), "
    "ei(56.2), 2hump_ei(71.8), 3hump_ei(89.6)\n"
    "5.0, 1.234e-03, 2.345e-03, 1.123e-03, 4.702e-03, 0.001, 0.001, 0.001, 0.000, 0.000\n"
    "10.0, 2.500e-03, 3.100e-03, 1.800e-03, 7.400e-03, 0.002, 0.002, 0.002, 0.001, 0.001\n"
    "15.0, 4.100e-03, 5.200e-03, 2.900e-03, 1.220e-02, 0.004, 0.003, 0.004, 0.002, 0.001\n"
    "20.0, 8.700e-03, 1.020e-02, 5.600e-03, 2.450e-02, 0.009, 0.007, 0.008, 0.004, 0.003\n"
    "25.0, 1.500e-02, 1.800e-02, 9.200e-03, 4.220e-02, 0.016, 0.012, 0.014, 0.008, 0.005\n"
    "30.0, 3.200e-02, 4.100e-02, 2.100e-02, 9.400e-02, 0.035, 0.026, 0.030, 0.017, 0.011\n"
    "35.0, 6.800e-02, 8.500e-02, 4.200e-02, 1.950e-01, 0.074, 0.055, 0.063, 0.036, 0.024\n"
    "40.0, 1.200e-01, 1.500e-01, 7.800e-02, 3.480e-01, 0.130, 0.098, 0.112, 0.065, 0.043\n"
    "45.0, 2.100e-01, 2.800e-01, 1.400e-01, 6.300e-01, 0.228, 0.171, 0.196, 0.113, 0.075\n"
    "50.0, 3.500e-01, 4.200e-01, 2.100e-01, 9.800e-01, 0.380, 0.285, 0.327, 0.189, 0.126\n"
    "55.0, 2.800e-01, 3.600e-01, 1.700e-01, 8.100e-01, 0.304, 0.228, 0.261, 0.151, 0.101\n"
    "60.0, 1.500e-01, 2.000e-01, 9.500e-02, 4.450e-01, 0.163, 0.122, 0.140, 0.081, 0.054\n";

} // anonymous namespace

// ============================================================================
// Test 1: CSV data populates the frequency bins and the per-axis raw PSD
// ============================================================================

TEST_CASE("CSV data populates frequency bins and raw PSD", "[input_shaper_chart]") {
    TempCsvFile csv(REALISTIC_CSV);
    auto csv_data = parse_shaper_csv(csv.path, 'X');

    REQUIRE(csv_data.frequencies.size() == 12);
    REQUIRE(csv_data.raw_psd.size() == csv_data.frequencies.size());

    SECTION("frequency values match CSV") {
        CHECK(csv_data.frequencies[0] == Catch::Approx(5.0f));
        CHECK(csv_data.frequencies[5] == Catch::Approx(30.0f));
        CHECK(csv_data.frequencies[11] == Catch::Approx(60.0f));
    }

    SECTION("PSD amplitude values match the psd_x column") {
        CHECK(csv_data.raw_psd[0] == Catch::Approx(1.234e-03f));
        CHECK(csv_data.raw_psd[7] == Catch::Approx(1.200e-01f));
        CHECK(csv_data.raw_psd[9] == Catch::Approx(3.500e-01f));
    }

    SECTION("the axis argument selects the PSD column") {
        // psd_y, not psd_x — reading the wrong column is a silent wrong chart,
        // not an error.
        auto y_data = parse_shaper_csv(csv.path, 'Y');
        REQUIRE(y_data.raw_psd.size() == 12);
        CHECK(y_data.raw_psd[0] == Catch::Approx(2.345e-03f));
        CHECK(y_data.raw_psd[9] == Catch::Approx(4.200e-01f));
        // The frequency bins are shared between axes.
        CHECK(y_data.frequencies == csv_data.frequencies);
    }
}

// ============================================================================
// Test 2: CSV data populates shaper_curves
// ============================================================================

TEST_CASE("CSV data populates shaper curves", "[input_shaper_chart]") {
    TempCsvFile csv(REALISTIC_CSV);
    auto csv_data = parse_shaper_csv(csv.path, 'X');

    REQUIRE(csv_data.shaper_curves.size() == 5);

    SECTION("shaper names match expected order") {
        CHECK(csv_data.shaper_curves[0].name == "zv");
        CHECK(csv_data.shaper_curves[1].name == "mzv");
        CHECK(csv_data.shaper_curves[2].name == "ei");
        CHECK(csv_data.shaper_curves[3].name == "2hump_ei");
        CHECK(csv_data.shaper_curves[4].name == "3hump_ei");
    }

    SECTION("shaper fitted frequencies come from the column headers") {
        CHECK(csv_data.shaper_curves[0].frequency == Catch::Approx(59.0f));
        CHECK(csv_data.shaper_curves[1].frequency == Catch::Approx(53.8f));
        CHECK(csv_data.shaper_curves[2].frequency == Catch::Approx(56.2f));
        CHECK(csv_data.shaper_curves[3].frequency == Catch::Approx(71.8f));
        CHECK(csv_data.shaper_curves[4].frequency == Catch::Approx(89.6f));
    }

    SECTION("shaper curve values have the same row count as the frequency bins") {
        for (const auto& curve : csv_data.shaper_curves) {
            INFO("Checking shaper: " << curve.name);
            CHECK(curve.values.size() == csv_data.frequencies.size());
        }
    }

    SECTION("curve values are the transfer coefficient times the raw PSD") {
        // The CSV stores 0..1 transfer coefficients; the chart needs the shaped
        // PSD, so the parser multiplies through (shaper_csv_parser.cpp:166-172).
        // Row 9 (50 Hz): psd_x = 0.350, mzv coefficient = 0.285.
        CHECK(csv_data.shaper_curves[1].values[9] == Catch::Approx(0.285f * 0.350f));
        CHECK(csv_data.shaper_curves[0].values[9] == Catch::Approx(0.380f * 0.350f));
        // Every shaped value stays at or below the raw PSD it attenuates.
        for (const auto& curve : csv_data.shaper_curves) {
            INFO("Checking shaper: " << curve.name);
            for (size_t i = 0; i < curve.values.size(); ++i) {
                CHECK(curve.values[i] >= 0.0f);
                CHECK(curve.values[i] <= csv_data.raw_psd[i] + 1e-6f);
            }
        }
    }
}

// ============================================================================
// Test 3: Recommended shaper identification
// ============================================================================

TEST_CASE("Recommended shaper is identified from all_shapers", "[input_shaper_chart]") {
    // Build a result with all_shapers populated (as mock/collector would)
    InputShaperResult result;
    result.axis = 'X';
    result.shaper_type = "mzv"; // Recommended by Klipper
    result.shaper_freq = 53.8f;

    ShaperOption zv;
    zv.type = "zv";
    zv.frequency = 59.0f;
    zv.vibrations = 5.2f;
    zv.smoothing = 0.045f;
    zv.max_accel = 13400.0f;

    ShaperOption mzv;
    mzv.type = "mzv";
    mzv.frequency = 53.8f;
    mzv.vibrations = 1.6f;
    mzv.smoothing = 0.130f;
    mzv.max_accel = 4000.0f;

    ShaperOption ei;
    ei.type = "ei";
    ei.frequency = 56.2f;
    ei.vibrations = 0.7f;
    ei.smoothing = 0.120f;
    ei.max_accel = 4600.0f;

    ShaperOption two_hump;
    two_hump.type = "2hump_ei";
    two_hump.frequency = 71.8f;
    two_hump.vibrations = 0.0f;
    two_hump.smoothing = 0.260f;
    two_hump.max_accel = 8800.0f;

    ShaperOption three_hump;
    three_hump.type = "3hump_ei";
    three_hump.frequency = 89.6f;
    three_hump.vibrations = 0.0f;
    three_hump.smoothing = 0.350f;
    three_hump.max_accel = 8800.0f;

    result.all_shapers = {zv, mzv, ei, two_hump, three_hump};

    SECTION("recommended shaper matches result.shaper_type") {
        // Find the recommended shaper in all_shapers
        auto it =
            std::find_if(result.all_shapers.begin(), result.all_shapers.end(),
                         [&](const ShaperOption& opt) { return opt.type == result.shaper_type; });
        REQUIRE(it != result.all_shapers.end());
        CHECK(it->type == "mzv");
        CHECK(it->frequency == Catch::Approx(53.8f));
    }

    SECTION("recommended shaper frequency matches result.shaper_freq") {
        auto it =
            std::find_if(result.all_shapers.begin(), result.all_shapers.end(),
                         [&](const ShaperOption& opt) { return opt.type == result.shaper_type; });
        REQUIRE(it != result.all_shapers.end());
        CHECK(it->frequency == Catch::Approx(result.shaper_freq));
    }

    SECTION("recommended shaper has lower vibrations than zv") {
        auto rec_it =
            std::find_if(result.all_shapers.begin(), result.all_shapers.end(),
                         [&](const ShaperOption& opt) { return opt.type == result.shaper_type; });
        auto zv_it = std::find_if(result.all_shapers.begin(), result.all_shapers.end(),
                                  [](const ShaperOption& opt) { return opt.type == "zv"; });
        REQUIRE(rec_it != result.all_shapers.end());
        REQUIRE(zv_it != result.all_shapers.end());
        CHECK(rec_it->vibrations < zv_it->vibrations);
    }
}

// ============================================================================
// Test 4: Unreadable / unusable CSV produces no frequency data
// ============================================================================

TEST_CASE("Unusable CSV input produces no frequency data", "[input_shaper_chart]") {
    // This is the precondition the collector turns into
    // InputShaperResult::chart_data_unavailable: Klipper reported a CSV path,
    // but nothing readable came back from it, so the UI says "chart
    // unavailable" instead of drawing a blank graph
    // (src/api/moonraker_advanced_api.cpp:1334, :1350).

    SECTION("nonexistent file path") {
        auto csv_data = parse_shaper_csv("/tmp/nonexistent_chart_test.csv", 'X');
        CHECK(csv_data.frequencies.empty());
        CHECK(csv_data.raw_psd.empty());
        CHECK(csv_data.shaper_curves.empty());
    }

    SECTION("empty string path") {
        auto csv_data = parse_shaper_csv("", 'X');
        CHECK(csv_data.frequencies.empty());
        CHECK(csv_data.raw_psd.empty());
        CHECK(csv_data.shaper_curves.empty());
    }

    SECTION("empty file content") {
        TempCsvFile csv("");
        auto csv_data = parse_shaper_csv(csv.path, 'X');
        CHECK(csv_data.frequencies.empty());
        CHECK(csv_data.shaper_curves.empty());
    }

    SECTION("header only, no data rows") {
        // The header parses fine and yields five named curves — but with no
        // rows there is nothing to plot. Only `frequencies` distinguishes this
        // from a usable file, which is why the collector gates on that and not
        // on shaper_curves.
        TempCsvFile csv(CSV_HEADER);
        auto csv_data = parse_shaper_csv(csv.path, 'X');

        CHECK(csv_data.frequencies.empty());
        CHECK(csv_data.raw_psd.empty());
        REQUIRE(csv_data.shaper_curves.size() == 5);
        for (const auto& curve : csv_data.shaper_curves) {
            INFO("Checking shaper: " << curve.name);
            CHECK_FALSE(curve.name.empty());
            CHECK(curve.values.empty());
        }
    }

    SECTION("data rows but no recognizable header") {
        // A truncated or reworded header with no 'freq' column: the rows are
        // unusable even though the file is full of numbers.
        TempCsvFile csv("time, accel_x, accel_y\n"
                        "0.0, 1.0, 2.0\n"
                        "0.1, 1.1, 2.1\n");
        auto csv_data = parse_shaper_csv(csv.path, 'X');

        CHECK(csv_data.frequencies.empty());
        CHECK(csv_data.shaper_curves.empty());
    }

    SECTION("freq column present but the requested axis column is missing") {
        // A Y-axis request against an X-only CSV must yield nothing rather
        // than silently charting the wrong column.
        TempCsvFile csv("freq, psd_x, zv(59.0)\n"
                        "5.0, 1.0, 0.5\n");
        auto x_data = parse_shaper_csv(csv.path, 'X');
        REQUIRE(x_data.frequencies.size() == 1);

        auto y_data = parse_shaper_csv(csv.path, 'Y');
        CHECK(y_data.frequencies.empty());
        CHECK(y_data.shaper_curves.empty());
    }
}

// ============================================================================
// Test 5: Shaper curves match expected count (5 standard Klipper shapers)
// ============================================================================

TEST_CASE("Shaper curves match expected count from standard Klipper CSV", "[input_shaper_chart]") {
    TempCsvFile csv(REALISTIC_CSV);
    auto csv_data = parse_shaper_csv(csv.path, 'X');

    SECTION("5 shaper curves from standard Klipper output") {
        REQUIRE(csv_data.shaper_curves.size() == 5);
    }

    SECTION("standard Klipper shaper types present") {
        std::vector<std::string> expected_types = {"zv", "mzv", "ei", "2hump_ei", "3hump_ei"};
        REQUIRE(csv_data.shaper_curves.size() == expected_types.size());
        for (size_t i = 0; i < expected_types.size(); ++i) {
            CHECK(csv_data.shaper_curves[i].name == expected_types[i]);
        }
    }

    SECTION("each shaper curve has a positive fitted frequency") {
        for (const auto& curve : csv_data.shaper_curves) {
            INFO("Checking shaper: " << curve.name);
            CHECK(curve.frequency > 0.0f);
        }
    }

    SECTION("all shaper curves have same number of data points as freq bins") {
        size_t expected_bins = csv_data.frequencies.size();
        REQUIRE(expected_bins == 12);
        for (const auto& curve : csv_data.shaper_curves) {
            INFO("Checking shaper: " << curve.name);
            CHECK(curve.values.size() == expected_bins);
        }
    }

    SECTION("the legacy 'shapers:' marker column is skipped, not fitted") {
        // Older Klipper emits a bare "shapers:" separator column; it is not a
        // shaper and must not become a sixth curve.
        TempCsvFile legacy("freq, psd_x, psd_y, psd_z, psd_xyz, shapers:, zv(59.0), mzv(53.8)\n"
                           "5.0, 1.0, 2.0, 3.0, 6.0, , 0.5, 0.4\n"
                           "10.0, 2.0, 3.0, 4.0, 9.0, , 0.6, 0.5\n");
        auto legacy_data = parse_shaper_csv(legacy.path, 'X');
        REQUIRE(legacy_data.shaper_curves.size() == 2);
        CHECK(legacy_data.shaper_curves[0].name == "zv");
        CHECK(legacy_data.shaper_curves[1].name == "mzv");
        CHECK(legacy_data.frequencies.size() == 2);
    }
}
