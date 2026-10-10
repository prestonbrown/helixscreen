// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"
#include "moonraker_client_mock.h"
#include "resonance_console.h"
#include "shaper_csv_parser.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::calibration;

namespace {

struct BeltMockFixture : LVGLTestFixture {
    MoonrakerClientMock mock{MoonrakerClientMock::PrinterType::VORON_24};
    std::vector<std::string> lines;
    BeltMockFixture() {
        mock.set_belt_line_interval_ms(1);
        mock.register_method_callback("notify_gcode_response", "belt_mock_test",
                                      [this](const nlohmann::json& msg) {
                                          lines.push_back(msg["params"][0].get<std::string>());
                                      });
    }
    ~BeltMockFixture() override {
        MoonrakerClientMock::remove_belt_csvs();
    }
    std::optional<std::string> run(const std::string& gcode) {
        lines.clear();
        mock.gcode_script(gcode);
        for (int i = 0; i < 2000; ++i) {
            lv_tick_inc(2);
            lv_timer_handler();
            if (!lines.empty() && parse_written_csv_path(lines.back()))
                return parse_written_csv_path(lines.back());
        }
        return std::nullopt;
    }
};

} // namespace

TEST_CASE_METHOD(BeltMockFixture, "mock sweep emits Klipper's exact lines", "[belt][mock]") {
    // The exact command test_belt_resonance sends, SWEEPING_PERIOD included.
    auto path =
        run("TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_a SWEEPING_PERIOD=0");
    REQUIRE(path);
    CHECK(lines.front() == "Testing frequency 5 Hz");
    CHECK(lines[lines.size() - 2] == "Testing frequency 135 Hz");
    CHECK(lines.back().rfind(
              "Resonances data written to /tmp/resonances_axis=1.000,-1.000,0.000_helix_belt_a_",
              0) == 0);
    auto d = parse_resonance_csv(*path);
    REQUIRE(d.error == ResonanceCsvError::NONE);
    auto peak = find_peak_frequency(d.curve, 20.0f, d.curve.back().first);
    CHECK(peak.frequency == Catch::Approx(mock.belt_peak_hz('A')).margin(1.0f));
}

TEST_CASE_METHOD(BeltMockFixture, "re-testing a path walks it toward the other", "[belt][mock]") {
    mock.set_belt_peaks_hz(110.0f, 98.0f);
    run("TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_a");
    CHECK(mock.belt_peak_hz('A') == Catch::Approx(110.0f)); // first measurement: no move
    run("TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_a");
    CHECK(mock.belt_peak_hz('A') == Catch::Approx(106.0f));
    run("TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_a");
    run("TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_a");
    CHECK(mock.belt_peak_hz('A') == Catch::Approx(98.0f)); // never overshoots
}

TEST_CASE_METHOD(BeltMockFixture, "mock sweep follows the configured range", "[belt][mock]") {
    mock.set_resonance_sweep_range(10.0, 60.0, 2.0);
    run("TEST_RESONANCES AXIS=1,1 OUTPUT=resonances NAME=helix_belt_b");
    CHECK(lines.front() == "Testing frequency 10 Hz");
    CHECK(lines[lines.size() - 2] == "Testing frequency 60 Hz");
}

TEST_CASE_METHOD(BeltMockFixture, "mock failure modes", "[belt][mock]") {
    SECTION("kalico: two-part axis name and extra column") {
        mock.set_belt_failure(BeltMockFailure::KALICO);
        auto path = run("TEST_RESONANCES AXIS=1,1 OUTPUT=resonances NAME=helix_belt_b");
        REQUIRE(path);
        CHECK(path->find("axis=1.000,1.000_helix_belt_b") != std::string::npos);
        CHECK(parse_resonance_csv(*path).error == ResonanceCsvError::NONE);
    }
    SECTION("multichip") {
        mock.set_belt_failure(BeltMockFailure::MULTICHIP);
        auto path = run("TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_a");
        REQUIRE(path);
        CHECK(parse_resonance_csv(*path).error == ResonanceCsvError::MULTI_CHIP);
    }
    SECTION("nofile") {
        mock.set_belt_failure(BeltMockFailure::NO_FILE);
        auto path = run("TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_a");
        REQUIRE(path);
        CHECK(parse_resonance_csv(*path).error == ResonanceCsvError::MISSING);
    }
    SECTION("stall never reports a file") {
        mock.set_belt_failure(BeltMockFailure::STALL);
        CHECK_FALSE(run("TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_a"));
    }
    SECTION("error emits a !! line") {
        mock.set_belt_failure(BeltMockFailure::ERROR);
        CHECK_FALSE(run("TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_a"));
        CHECK(lines.back().rfind("!! ", 0) == 0);
    }
}
