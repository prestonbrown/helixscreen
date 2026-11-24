// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_test_fixture.h"
#include "belt_settle.h"
#include "lvgl/lvgl.h"
#include "mock_persona.h"
#include "mock_planted_gcodes.h"
#include "moonraker_client_mock.h"
#include "resonance_console.h"
#include "shaper_csv_parser.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>
#include <unistd.h>
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
    CHECK(lines.back().rfind("Resonances data written to " + helix::mock::scratch_dir() +
                                 "/resonances_axis=1.000,-1.000,0.000_helix_belt_a_",
                             0) == 0);
    auto d = parse_resonance_csv(*path);
    REQUIRE(d.error == ResonanceCsvError::NONE);
    auto peak = find_peak_frequency(d.curve, 20.0f, d.curve.back().first);
    CHECK(peak.frequency == Catch::Approx(mock.belt_peak_hz('A')).margin(1.0f));
}

TEST_CASE_METHOD(BeltMockFixture,
                 "removing the belt CSVs deletes what the mock wrote and nothing else in /tmp",
                 "[belt][mock]") {
    auto path =
        run("TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_a SWEEPING_PERIOD=0");
    REQUIRE(path);
    REQUIRE(access(path->c_str(), F_OK) == 0);
    // Named like a belt CSV of this process, but no mock wrote it: the removal
    // works from what it wrote, never from a scan of /tmp.
    const std::string bystander =
        MoonrakerClientMock::belt_csv_path("axis=0.000,0.000,1.000", "not_the_mock");
    std::FILE* f = std::fopen(bystander.c_str(), "w");
    REQUIRE(f != nullptr);
    std::fclose(f);

    MoonrakerClientMock::remove_belt_csvs();

    CHECK(access(path->c_str(), F_OK) != 0);
    CHECK(access(bystander.c_str(), F_OK) == 0);
    std::remove(bystander.c_str());
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

namespace {
constexpr const char* kWindowB =
    "TEST_RESONANCES AXIS=1,1 FREQ_START=88 FREQ_END=120 OUTPUT=resonances NAME=helix_belt_b";
constexpr const char* kWindowA =
    "TEST_RESONANCES AXIS=1,-1 FREQ_START=88 FREQ_END=120 OUTPUT=resonances NAME=helix_belt_a";

float csv_peak(const std::string& path) {
    auto d = parse_resonance_csv(path);
    REQUIRE(d.error == ResonanceCsvError::NONE);
    return find_peak_frequency(d.curve, 20.0f, d.curve.back().first).frequency;
}
} // namespace

TEST_CASE_METHOD(BeltMockFixture, "a windowed mock sweep emits lines only inside the window",
                 "[belt][mock]") {
    auto path = run(kWindowB);
    REQUIRE(path);
    CHECK(lines.front() == "Testing frequency 88 Hz");
    CHECK(lines[lines.size() - 2] == "Testing frequency 120 Hz");
    // The CSV still spans the configured range, as Klipper's does.
    CHECK(parse_resonance_csv(*path).curve.back().first > 130.0f);
}

TEST_CASE_METHOD(BeltMockFixture,
                 "the tune script moves the scripted path's peak per sweep after the first",
                 "[belt][mock]") {
    mock.set_belt_tune_script('B', {102.1f, 107.6f, 109.8f});
    // The first sweep reads the seeded peak; the script starts on the second.
    for (float expected : {98.0f, 102.1f, 107.6f, 109.8f}) {
        auto path = run(kWindowB);
        REQUIRE(path);
        CHECK(csv_peak(*path) == Catch::Approx(expected).margin(0.8f));
    }
    // Exhausted: the last value holds.
    auto path = run(kWindowB);
    REQUIRE(path);
    CHECK(csv_peak(*path) == Catch::Approx(109.8f).margin(0.8f));

    auto a = run(kWindowA);
    REQUIRE(a);
    CHECK(csv_peak(*a) == Catch::Approx(110.0f).margin(0.8f));
}

TEST_CASE_METHOD(BeltMockFixture,
                 "a script turns the walk off and plays on every sweep after the first",
                 "[belt][mock]") {
    mock.set_belt_peaks_hz(110.0f, 98.0f);
    mock.set_belt_tune_script('B', {98.3f, 102.1f});
    const char* full_a = "TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_a";
    const char* full_b = "TEST_RESONANCES AXIS=1,1 OUTPUT=resonances NAME=helix_belt_b";

    auto a0 = run(full_a);
    REQUIRE(a0);
    CHECK(csv_peak(*a0) == Catch::Approx(110.0f).margin(0.8f));
    auto b0 = run(full_b);
    REQUIRE(b0);
    CHECK(csv_peak(*b0) == Catch::Approx(98.0f).margin(0.8f));

    // A full sweep plays the script, then a windowed one plays the next entry.
    auto b1 = run(full_b);
    REQUIRE(b1);
    CHECK(mock.belt_peak_hz('B') == Catch::Approx(98.3f));
    CHECK(csv_peak(*b1) == Catch::Approx(98.3f).margin(0.8f));
    auto b2 = run(kWindowB);
    REQUIRE(b2);
    CHECK(mock.belt_peak_hz('B') == Catch::Approx(102.1f));
    CHECK(csv_peak(*b2) == Catch::Approx(102.1f).margin(0.8f));

    // A has no script, but a script on B switches the walk off for both paths.
    for (int i = 0; i < 2; ++i) {
        auto a = run(full_a);
        REQUIRE(a);
        CHECK(csv_peak(*a) == Catch::Approx(110.0f).margin(0.8f));
    }
    CHECK(mock.belt_peak_hz('A') == Catch::Approx(110.0f));
}

TEST_CASE_METHOD(BeltMockFixture, "windowed sweeps never apply the re-measure walk",
                 "[belt][mock]") {
    mock.set_belt_peaks_hz(110.0f, 98.0f);
    for (int i = 0; i < 4; ++i) {
        auto path = run(kWindowA);
        REQUIRE(path);
        CHECK(csv_peak(*path) == Catch::Approx(110.0f).margin(0.8f));
    }
    CHECK(mock.belt_peak_hz('A') == Catch::Approx(110.0f));
}

TEST_CASE_METHOD(BeltMockFixture, "a malformed script is ignored item by item", "[belt][mock]") {
    mock.apply_belt_tune_script("B:102,abc,,109");
    REQUIRE(run(kWindowB)); // the first sweep reads the seeded peak
    auto first = run(kWindowB);
    REQUIRE(first);
    CHECK(csv_peak(*first) == Catch::Approx(102.0f).margin(0.8f));
    auto second = run(kWindowB);
    REQUIRE(second);
    CHECK(csv_peak(*second) == Catch::Approx(109.0f).margin(0.8f));

    // A letter other than A/B drops the whole spec; the paths keep their peaks.
    mock.apply_belt_tune_script("Q:50,60");
    auto third = run(kWindowB);
    REQUIRE(third);
    CHECK(csv_peak(*third) == Catch::Approx(109.0f).margin(0.8f));
}

TEST_CASE_METHOD(BeltMockFixture, "a flat script item writes a curve with no peak",
                 "[belt][mock]") {
    auto in_window_flatness = [](const std::string& path) {
        auto d = parse_resonance_csv(path);
        REQUIRE(d.error == ResonanceCsvError::NONE);
        float lo = 1e30f, hi = 0.0f;
        for (const auto& [f, v] : d.curve) {
            if (f >= 88.0f && f <= 120.0f) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        return hi / lo;
    };

    SECTION("env-string form") {
        mock.apply_belt_tune_script("B:flat");
        REQUIRE(run(kWindowB)); // the first sweep reads the seeded peak
        auto path = run(kWindowB);
        REQUIRE(path);
        CHECK(in_window_flatness(*path) < 1.25f);
    }
    SECTION("setter form with NaN") {
        mock.set_belt_tune_script('B', {std::numeric_limits<float>::quiet_NaN()});
        REQUIRE(run(kWindowB));
        auto path = run(kWindowB);
        REQUIRE(path);
        CHECK(in_window_flatness(*path) < 1.25f);
    }
    SECTION("a scripted peak is not flat") {
        mock.apply_belt_tune_script("B:104");
        REQUIRE(run(kWindowB));
        auto path = run(kWindowB);
        REQUIRE(path);
        CHECK(in_window_flatness(*path) > 2.0f);
    }
}

namespace {

/// The persona's configfile.settings, as the panel's query reads them.
nlohmann::json mock_settings(MoonrakerClientMock& mock) {
    nlohmann::json settings;
    mock.send_jsonrpc(
        "printer.objects.query",
        nlohmann::json{{"objects", {{"configfile", nlohmann::json::array({"settings"})}}}},
        [&settings](const nlohmann::json& r) {
            settings = r["result"]["status"]["configfile"]["settings"];
        },
        [](const MoonrakerError&) {});
    return settings;
}

/// Settle inputs from the persona's own limits and probe point.
BeltSettleInputs mock_settle_inputs(MoonrakerClientMock& mock, float grow_mm = 0.0f) {
    const auto axis_max = helix::mock::descriptor(mock.get_printer_type()).axis_max;
    BeltSettleInputs in;
    in.x_max = static_cast<float>(axis_max.x) + grow_mm;
    in.y_max = static_cast<float>(axis_max.y) + grow_mm;
    in.has_xy_limits = true;
    in.probe_point = parse_resonance_tester_config(mock_settings(mock)).probe_point;
    in.max_velocity_mm_s = 500.0f;
    return in;
}

} // namespace

TEST_CASE_METHOD(BeltMockFixture,
                 "the mock's resonance_tester names a probe point at the bed centre",
                 "[belt][mock][settle]") {
    const auto persona = helix::mock::descriptor(mock.get_printer_type());
    const auto cfg = parse_resonance_tester_config(mock_settings(mock));
    REQUIRE(cfg.probe_point.has_value());
    CHECK((*cfg.probe_point)[0] == Catch::Approx((persona.axis_min.x + persona.axis_max.x) / 2.0));
    CHECK((*cfg.probe_point)[1] == Catch::Approx((persona.axis_min.y + persona.axis_max.y) / 2.0));
    CHECK((*cfg.probe_point)[2] == Catch::Approx(20.0));
}

TEST_CASE_METHOD(BeltMockFixture,
                 "settling laps inside the travel are accepted and end at the probe point",
                 "[belt][mock][settle]") {
    const auto inputs = mock_settle_inputs(mock);
    REQUIRE(inputs.probe_point.has_value());
    const auto plan = plan_belt_settle(inputs);
    REQUIRE(plan.refusal == BeltSettleRefusal::NONE);
    mock.gcode_script("G28");
    // The mock's toolhead query serves a fixed position; its moves report
    // through notify_status_update, as Klipper's do.
    nlohmann::json position;
    mock.register_method_callback(
        "notify_status_update", "belt_settle_test", [&position](const nlohmann::json& msg) {
            const auto& status = msg["params"][0];
            if (status.contains("toolhead") && status["toolhead"].contains("position")) {
                position = status["toolhead"]["position"];
            }
        });

    lines.clear();
    bool acked = false;
    bool failed = false;
    mock.send_jsonrpc(
        "printer.gcode.script", nlohmann::json{{"script", plan.gcode}},
        [&acked](const nlohmann::json&) { acked = true; },
        [&failed](const MoonrakerError&) { failed = true; });
    CHECK(acked);
    CHECK_FALSE(failed);
    CHECK(std::none_of(lines.begin(), lines.end(),
                       [](const std::string& l) { return l.rfind("!!", 0) == 0; }));

    REQUIRE(position.is_array());
    REQUIRE(position.size() >= 3);
    CHECK(position[0].get<double>() == Catch::Approx((*inputs.probe_point)[0]));
    CHECK(position[1].get<double>() == Catch::Approx((*inputs.probe_point)[1]));
    CHECK(position[2].get<double>() == Catch::Approx((*inputs.probe_point)[2]));
}

TEST_CASE_METHOD(BeltMockFixture, "a lap outside the travel is rejected", "[belt][mock][settle]") {
    // 100 mm past the travel: the inset grows too, but not enough to cover it.
    const auto plan = plan_belt_settle(mock_settle_inputs(mock, 100.0f));
    REQUIRE(plan.refusal == BeltSettleRefusal::NONE);
    mock.gcode_script("G28");

    lines.clear();
    std::string error;
    mock.send_jsonrpc(
        "printer.gcode.script", nlohmann::json{{"script", plan.gcode}},
        [](const nlohmann::json&) {}, [&error](const MoonrakerError& e) { error = e.message; });
    CHECK(error.find("Move out of range") != std::string::npos);
    CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string& l) {
        return l.rfind("!! Move out of range", 0) == 0;
    }));
}

TEST_CASE_METHOD(BeltMockFixture, "the mock's resonance_tester names its accelerometer",
                 "[sweep_preview][target]") {
    CHECK(mock.resonance_tester_settings()["accel_chip"] == "adxl345");
    CHECK(parse_resonance_tester_config(mock_settings(mock)).accel_chip == "adxl345");
}

namespace {

/// Every console line one gcode produces, pumping until the CSV line or the budget.
std::vector<std::string> run_long(BeltMockFixture& f, const std::string& gcode) {
    f.lines.clear();
    f.mock.gcode_script(gcode);
    for (int i = 0; i < 20000; ++i) {
        lv_tick_inc(5);
        lv_timer_handler();
        if (!f.lines.empty() && parse_written_csv_path(f.lines.back())) {
            break;
        }
    }
    return f.lines;
}

size_t testing_lines_before_calculating(const std::vector<std::string>& lines,
                                        std::vector<float>* freqs = nullptr) {
    size_t n = 0;
    for (const auto& line : lines) {
        if (line.rfind("Calculating the best", 0) == 0) {
            break;
        }
        if (const auto hz = parse_testing_frequency(line)) {
            ++n;
            if (freqs) {
                freqs->push_back(*hz);
            }
        }
    }
    return n;
}

void check_response(const helix::calibration::MockSweepResponse& r, float peak, float hwhm,
                    float drive, float drive_margin, std::array<float, 3> weights) {
    CHECK(r.peak_hz == Catch::Approx(peak));
    CHECK(r.half_width_hz == Catch::Approx(hwhm));
    CHECK(r.drive_mm_s2 == Catch::Approx(drive).margin(drive_margin));
    for (size_t i = 0; i < 3; ++i) {
        CHECK(r.axis_weight[i] == Catch::Approx(weights[i]).margin(1e-3));
    }
}

} // namespace

TEST_CASE_METHOD(BeltMockFixture, "the mock records what each sweep resonates like",
                 "[mock][sweep_preview]") {
    REQUIRE(run("TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_a SWEEPING_PERIOD=0"));
    check_response(mock.active_sweep_response(), 110.0f, 10.0f, 300.0f, 0.5f,
                   {0.671f, 0.671f, 0.316f});

    REQUIRE(parse_written_csv_path(run_long(*this, "SHAPER_CALIBRATE AXIS=X").back()));
    check_response(mock.active_sweep_response(), 53.8f, 8.0f, 0.248f, 0.001f,
                   {1.0f, 0.387f, 0.283f});

    REQUIRE(parse_written_csv_path(run_long(*this, "SHAPER_CALIBRATE AXIS=Y").back()));
    check_response(mock.active_sweep_response(), 48.2f, 8.0f, 0.248f, 0.001f,
                   {0.387f, 1.0f, 0.283f});
    MoonrakerClientMock::remove_shaper_csvs();
}

TEST_CASE_METHOD(BeltMockFixture, "a realtime shaper sweep prints every whole Hz",
                 "[mock][sweep_preview]") {
    SECTION("realtime") {
        mock.set_shaper_sweep_realtime(true);
        const auto lines = run_long(*this, "SHAPER_CALIBRATE AXIS=X");
        REQUIRE_FALSE(lines.empty());
        REQUIRE(parse_written_csv_path(lines.back()));
        std::vector<float> freqs;
        CHECK(testing_lines_before_calculating(lines, &freqs) == 131);
        REQUIRE(freqs.size() == 131);
        for (size_t i = 0; i < freqs.size(); ++i) {
            CHECK(freqs[i] == static_cast<float>(5 + i));
        }
    }
    SECTION("compact by default") {
        const auto lines = run_long(*this, "SHAPER_CALIBRATE AXIS=X");
        REQUIRE_FALSE(lines.empty());
        REQUIRE(parse_written_csv_path(lines.back()));
        CHECK(testing_lines_before_calculating(lines) == 20);
    }
    MoonrakerClientMock::remove_shaper_csvs();
}
