// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_api_belt_resonance.cpp
 * @brief Unit tests for the BeltResonanceCollector behind test_belt_resonance()
 *
 * Drives TEST_RESONANCES OUTPUT=resonances through the mock client and checks
 * the collector's contract: progress from the printer's configured range, the
 * parsed curves on Klipper's "Resonances data written to" line, error lines and
 * unreadable result files failing the run, and the cancel handle silencing
 * every later callback.
 */

#include "ui_update_queue.h"

#include "../../include/belt_tension_types.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../lvgl/lvgl.h"
#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

/**
 * @brief Fixture for belt resonance API testing with the mock client
 *
 * Same shape as InputShaperTestFixture: the mock answers TEST_RESONANCES by
 * playing console lines on an lv_timer, so every wait is a
 * lv_tick_inc/lv_timer_handler_safe pump.
 */
class BeltApiFixture : public LVGLTestFixture {
  public:
    BeltApiFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state_.init_subjects(false); // Don't register XML bindings in tests
        // execute_gcode() halted gate would otherwise reject every command.
        state_.set_klippy_state_sync(helix::KlippyState::READY);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, state_);
        mock_client_.set_belt_line_interval_ms(1);
    }

    ~BeltApiFixture() {
        // Run what the collectors queued while this fixture's PrinterState is
        // still alive, then drop the API before the mock's timers go.
        helix::ui::UpdateQueue::instance().drain_until_empty();
        api_.reset();
        MoonrakerClientMock::remove_belt_csvs();
    }

    /// Pump LVGL until @p flag is set; false when the bound ran out instead.
    static bool pump_until(std::atomic<bool>& flag) {
        for (int i = 0; i < 2000 && !flag.load(); ++i) {
            lv_tick_inc(100);
            lv_timer_handler_safe();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return flag.load();
    }

  protected:
    MoonrakerClientMock mock_client_;
    PrinterState state_;
    std::unique_ptr<MoonrakerAPI> api_;
};

TEST_CASE_METHOD(BeltApiFixture, "test_belt_resonance delivers both curves", "[belt][api]") {
    std::atomic<bool> done{false};
    calibration::BeltResponse got;
    int last_percent = -1;
    auto cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_a", [&](int pct, float) { last_percent = pct; },
        [&](const calibration::BeltResponse& r) {
            got = r;
            done = true;
        },
        [&](const MoonrakerError& e) { FAIL(e.message); });
    pump_until(done);
    REQUIRE(done);
    CHECK(last_percent == 100);
    REQUIRE_FALSE(got.xyz.empty());
    auto peak = calibration::find_peak_frequency(got.xyz, 20.0f, got.xyz.back().first);
    CHECK(peak.frequency == Catch::Approx(mock_client_.belt_peak_hz('A')).margin(1.0f));

    // The mock writes psd_x = psd_y = 0.45 x psd_xyz, each rounded to %.3e.
    REQUIRE(got.in_plane.size() == got.xyz.size());
    for (size_t i = 0; i < got.xyz.size(); ++i) {
        CHECK(got.in_plane[i].first == got.xyz[i].first);
        CHECK(got.in_plane[i].second == Catch::Approx(0.9f * got.xyz[i].second).epsilon(1e-3));
    }
}

TEST_CASE_METHOD(BeltApiFixture, "the sweep is sent pulse-only", "[belt][api]") {
    // SWEEPING_PERIOD=0 overrides a [resonance_tester] that defaults to the
    // sweeping excitation, which smooths over the faults a belt check hunts.
    mock_client_.clear_gcode_script_history();
    std::atomic<bool> done{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_a", nullptr, [&](const calibration::BeltResponse&) { done = true; },
        [&](const MoonrakerError& e) { FAIL(e.message); });
    pump_until(done);
    const auto history = mock_client_.gcode_script_history();
    CHECK(std::find(history.begin(), history.end(),
                    "TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_a "
                    "SWEEPING_PERIOD=0") != history.end());
}

TEST_CASE_METHOD(BeltApiFixture, "a windowed sweep sends FREQ_START and FREQ_END", "[belt][api]") {
    mock_client_.clear_gcode_script_history();
    std::atomic<bool> done{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_a", nullptr, [&](const calibration::BeltResponse&) { done = true; },
        [&](const MoonrakerError& e) { FAIL(e.message); },
        calibration::BeltSweepWindow{88.0f, 120.0f});
    pump_until(done);
    const auto history = mock_client_.gcode_script_history();
    CHECK(std::find(history.begin(), history.end(),
                    "TEST_RESONANCES AXIS=1,-1 FREQ_START=88 FREQ_END=120 OUTPUT=resonances "
                    "NAME=helix_belt_a SWEEPING_PERIOD=0") != history.end());
}

TEST_CASE_METHOD(BeltApiFixture, "a fractional window is rounded inward to whole Hz",
                 "[belt][api]") {
    // A K1C's max_freq is 133.33: rounding the end up would sweep past it.
    mock_client_.clear_gcode_script_history();
    std::atomic<bool> done{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,1", "helix_belt_b", nullptr, [&](const calibration::BeltResponse&) { done = true; },
        [&](const MoonrakerError& e) { FAIL(e.message); },
        calibration::BeltSweepWindow{98.6f, 133.33f});
    pump_until(done);
    const auto history = mock_client_.gcode_script_history();
    CHECK(std::find(history.begin(), history.end(),
                    "TEST_RESONANCES AXIS=1,1 FREQ_START=99 FREQ_END=133 OUTPUT=resonances "
                    "NAME=helix_belt_b SWEEPING_PERIOD=0") != history.end());
}

TEST_CASE_METHOD(BeltApiFixture, "a sweep without a window sends today's G-code", "[belt][api]") {
    mock_client_.clear_gcode_script_history();
    std::atomic<bool> done{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,1", "helix_belt_b", nullptr, [&](const calibration::BeltResponse&) { done = true; },
        [&](const MoonrakerError& e) { FAIL(e.message); });
    pump_until(done);
    const auto history = mock_client_.gcode_script_history();
    CHECK(std::find(history.begin(), history.end(),
                    "TEST_RESONANCES AXIS=1,1 OUTPUT=resonances NAME=helix_belt_b "
                    "SWEEPING_PERIOD=0") != history.end());
    CHECK(std::none_of(history.begin(), history.end(), [](const std::string& g) {
        return g.find("FREQ_START") != std::string::npos;
    }));
}

TEST_CASE_METHOD(BeltApiFixture, "windowed progress scales against the window", "[belt][api]") {
    // The printer's range runs to 152 Hz, so 104 Hz reads 50% only against the window.
    mock_client_.set_resonance_sweep_range(88.0, 152.0, 16.0);
    std::vector<std::pair<float, int>> seen;
    std::atomic<bool> done{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_a", [&](int pct, float f) { seen.emplace_back(f, pct); },
        [&](const calibration::BeltResponse&) { done = true; },
        [&](const MoonrakerError& e) { FAIL(e.message); },
        calibration::BeltSweepWindow{88.0f, 120.0f});
    pump_until(done);
    REQUIRE(seen.size() >= 3);
    for (const auto& [freq, pct] : seen) {
        if (freq == Catch::Approx(88.0f))
            CHECK(pct == 0);
        if (freq == Catch::Approx(104.0f))
            CHECK(pct == 50);
        if (freq == Catch::Approx(120.0f))
            CHECK(pct == 100);
    }
    CHECK(std::any_of(seen.begin(), seen.end(),
                      [](const auto& s) { return s.first == Catch::Approx(104.0f); }));
    CHECK(seen.front().second == 0);
    CHECK(seen.back().second == 100);
}

TEST_CASE_METHOD(BeltApiFixture, "progress stays clamped when firmware ignores the window",
                 "[belt][api]") {
    // The mock sweeps 5..135 Hz regardless of the window.
    mock_client_.set_resonance_sweep_range(5.0, 135.0, 130.0);
    std::vector<int> percents;
    std::atomic<bool> done{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_a", [&](int pct, float) { percents.push_back(pct); },
        [&](const calibration::BeltResponse&) { done = true; },
        [&](const MoonrakerError& e) { FAIL(e.message); },
        calibration::BeltSweepWindow{88.0f, 120.0f});
    pump_until(done);
    REQUIRE(percents.size() >= 2);
    CHECK(percents.front() == 0);
    CHECK(percents.back() == 100);
    CHECK(std::all_of(percents.begin(), percents.end(), [](int p) { return p >= 0 && p <= 100; }));
}

TEST_CASE_METHOD(BeltApiFixture, "an error line fails the run", "[belt][api]") {
    mock_client_.set_belt_failure(BeltMockFailure::ERROR);
    std::atomic<bool> failed{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_a", nullptr,
        [&](const calibration::BeltResponse&) { FAIL("no curve expected"); },
        [&](const MoonrakerError& e) {
            CHECK(e.message.find("adxl345") != std::string::npos);
            failed = true;
        });
    pump_until(failed);
    CHECK(failed);
}

TEST_CASE_METHOD(BeltApiFixture, "a stale file from an earlier run is never read", "[belt][api]") {
    // Leave a valid file where this run's output would go, then fail the run.
    const auto stale =
        MoonrakerClientMock::belt_csv_path("axis=1.000,-1.000,0.000", "helix_belt_a");
    { std::ofstream(stale) << "freq,psd_x,psd_y,psd_z,psd_xyz\n50.0,1,1,1,3\n"; }
    mock_client_.set_belt_failure(BeltMockFailure::ERROR);
    std::atomic<bool> failed{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_a", nullptr,
        [&](const calibration::BeltResponse&) { FAIL("stale file read"); },
        [&](const MoonrakerError&) { failed = true; });
    pump_until(failed);
    CHECK(failed);
}

TEST_CASE_METHOD(BeltApiFixture, "missing and multi-chip files become errors", "[belt][api]") {
    auto expect_error_containing = [&](BeltMockFailure f, const char* needle) {
        mock_client_.set_belt_failure(f);
        std::atomic<bool> failed{false};
        auto cancel = api_->advanced().test_belt_resonance(
            "1,-1", "helix_belt_a", nullptr,
            [&](const calibration::BeltResponse&) { FAIL("no curve"); },
            [&](const MoonrakerError& e) {
                CHECK(e.message.find(needle) != std::string::npos);
                failed = true;
            });
        pump_until(failed);
        CHECK(failed);
    };
    expect_error_containing(BeltMockFailure::NO_FILE, "printer's own computer");
    expect_error_containing(BeltMockFailure::MULTICHIP, "More than one accelerometer");
}

TEST_CASE_METHOD(BeltApiFixture, "cancel suppresses every later callback", "[belt][api]") {
    bool called = false;
    auto cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_a", [&](int, float) { called = true; },
        [&](const calibration::BeltResponse&) { called = true; },
        [&](const MoonrakerError&) { called = true; });
    cancel();
    cancel(); // idempotent
    for (int i = 0; i < 500; ++i) {
        lv_tick_inc(2);
        lv_timer_handler_safe();
    }
    CHECK_FALSE(called);
}

TEST_CASE_METHOD(BeltApiFixture, "cancel holds against a line already being dispatched",
                 "[belt][api]") {
    // The client snapshots its handlers before invoking them, so a line can
    // still reach a collector that cancel() has just unregistered. This
    // handler sorts ahead of the collector's and cancels mid-dispatch of the
    // terminal line, the order a WebSocket-thread delivery racing a UI cancel
    // produces.
    bool called = false;
    bool cancelled = false;
    helix::SharedCallback<void()> cancel;
    mock_client_.register_method_callback(
        "notify_gcode_response", "a_cancels_first", [&](const json& msg) {
            if (cancel && msg["params"][0].get<std::string>().find("Resonances data written") !=
                              std::string::npos) {
                cancel();
                cancelled = true;
            }
        });
    cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_a", [](int, float) {},
        [&](const calibration::BeltResponse&) { called = true; },
        [&](const MoonrakerError&) { called = true; });
    for (int i = 0; i < 2000; ++i) {
        lv_tick_inc(2);
        lv_timer_handler_safe();
    }
    mock_client_.unregister_method_callback("notify_gcode_response", "a_cancels_first");
    REQUIRE(cancelled);
    CHECK_FALSE(called);
}

TEST_CASE_METHOD(BeltApiFixture, "progress follows the printer's configured range", "[belt][api]") {
    mock_client_.set_resonance_sweep_range(10.0, 60.0, 2.0);
    std::vector<float> freqs;
    std::atomic<bool> done{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,1", "helix_belt_b",
        [&](int pct, float f) {
            if (freqs.empty())
                CHECK(pct == 0);
            freqs.push_back(f);
        },
        [&](const calibration::BeltResponse&) { done = true; },
        [&](const MoonrakerError& e) { FAIL(e.message); });
    pump_until(done);
    REQUIRE_FALSE(freqs.empty());
    CHECK(freqs.front() == Catch::Approx(10.0f));
    CHECK(freqs.back() == Catch::Approx(60.0f));
}

TEST_CASE_METHOD(BeltApiFixture, "a non-transport RPC error fails the run", "[belt][api]") {
    mock_client_.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR,
                                        "Klipper rejected the script", "TEST_RESONANCES");
    std::atomic<bool> failed{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_a", nullptr,
        [&](const calibration::BeltResponse&) { FAIL("no curve expected"); },
        [&](const MoonrakerError& e) {
            CHECK(e.message.find("Klipper rejected the script") != std::string::npos);
            failed = true;
        });
    pump_until(failed);
    CHECK(failed);
}

TEST_CASE_METHOD(BeltApiFixture,
                 "a transport-lost RPC keeps listening and a later line completes the run",
                 "[belt][api]") {
    mock_client_.force_next_gcode_error(MoonrakerErrorType::CONNECTION_LOST,
                                        "Connection to printer lost", "TEST_RESONANCES");
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_a", nullptr, [&](const calibration::BeltResponse&) { done = true; },
        [&](const MoonrakerError&) { failed = true; });
    pump_until(done);
    REQUIRE(done);
    CHECK_FALSE(failed.load());
}
