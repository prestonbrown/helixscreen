// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_api_belt_resonance.cpp
 * @brief Unit tests for the BeltResonanceCollector behind test_belt_resonance()
 *
 * Drives TEST_RESONANCES OUTPUT=resonances through the mock client and checks
 * the collector's contract: progress from the printer's configured range, the
 * parsed curve on Klipper's "Resonances data written to" line, error lines and
 * unreadable result files failing the run, and the cancel handle silencing
 * every later callback.
 */

#include "../../include/belt_tension_types.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../lvgl/lvgl.h"
#include "../test_helpers/update_queue_test_access.h"
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

namespace {
struct LVGLInitializerBeltApi {
    LVGLInitializerBeltApi() {
        static bool initialized = false;
        if (!initialized) {
            lv_init_safe();
            lv_display_t* disp = lv_display_create(800, 480);
            alignas(64) static lv_color_t buf[800 * 10];
            lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
            initialized = true;
        }
    }
};

static LVGLInitializerBeltApi lvgl_init;

} // namespace

/**
 * @brief Fixture for belt resonance API testing with the mock client
 *
 * Same shape as InputShaperTestFixture: the mock answers TEST_RESONANCES by
 * playing console lines on an lv_timer, so every wait is a
 * lv_tick_inc/lv_timer_handler_safe pump.
 */
class BeltApiFixture {
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
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
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

TEST_CASE_METHOD(BeltApiFixture, "test_belt_resonance delivers the parsed curve", "[belt][api]") {
    std::atomic<bool> done{false};
    calibration::BeltCurve got;
    int last_percent = -1;
    auto cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_a", [&](int pct, float) { last_percent = pct; },
        [&](const calibration::BeltCurve& c) {
            got = c;
            done = true;
        },
        [&](const MoonrakerError& e) { FAIL(e.message); });
    pump_until(done);
    REQUIRE(done);
    CHECK(last_percent == 100);
    auto peak = calibration::find_peak_frequency(got, 20.0f, got.back().first);
    CHECK(peak.frequency == Catch::Approx(mock_client_.belt_peak_hz('A')).margin(1.0f));
}

TEST_CASE_METHOD(BeltApiFixture, "the sweep is sent pulse-only", "[belt][api]") {
    // SWEEPING_PERIOD=0 overrides a [resonance_tester] that defaults to the
    // sweeping excitation, which smooths over the faults a belt check hunts.
    mock_client_.clear_gcode_script_history();
    std::atomic<bool> done{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_a", nullptr, [&](const calibration::BeltCurve&) { done = true; },
        [&](const MoonrakerError& e) { FAIL(e.message); });
    pump_until(done);
    const auto history = mock_client_.gcode_script_history();
    CHECK(std::find(history.begin(), history.end(),
                    "TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_a "
                    "SWEEPING_PERIOD=0") != history.end());
}

TEST_CASE_METHOD(BeltApiFixture, "an error line fails the run", "[belt][api]") {
    mock_client_.set_belt_failure(BeltMockFailure::ERROR);
    std::atomic<bool> failed{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_a", nullptr,
        [&](const calibration::BeltCurve&) { FAIL("no curve expected"); },
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
        [&](const calibration::BeltCurve&) { FAIL("stale file read"); },
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
            [&](const calibration::BeltCurve&) { FAIL("no curve"); },
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
        [&](const calibration::BeltCurve&) { called = true; },
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
    std::function<void()> cancel;
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
        [&](const calibration::BeltCurve&) { called = true; },
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
        [&](const calibration::BeltCurve&) { done = true; },
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
        [&](const calibration::BeltCurve&) { FAIL("no curve expected"); },
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
        "1,-1", "helix_belt_a", nullptr, [&](const calibration::BeltCurve&) { done = true; },
        [&](const MoonrakerError&) { failed = true; });
    pump_until(done);
    REQUIRE(done);
    CHECK_FALSE(failed.load());
}
