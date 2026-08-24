// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_safety_limits_null_endstop.cpp
 * @brief Regression tests for update_safety_limits_from_printer() null handling
 *
 * Klipper emits `position_endstop: null` in configfile.settings for any printer
 * configured with `endstop_pin: probe:z_virtual_endstop` — i.e. most
 * probe-equipped machines. The parse used to read it with a bare
 * `.get<double>()`, which throws type_error.302 on that null.
 *
 * The whole callback body is a single try block whose catch still calls
 * on_success(), so the throw was invisible: every temperature limit parsed
 * AFTER the endstop line silently kept its compiled default while the
 * connection reported success. These tests pin that the temperature-limit loop
 * still runs when the endstop is null.
 *
 * The endstop value itself is deliberately NOT asserted: 0.0mm is a legal
 * endstop, so the fix writes nothing at all rather than substituting a sentinel
 * a reader could not distinguish from a real value — and PrinterState exposes
 * it only through get_configured_z_offset_microns(), which prefers the probe
 * offset whenever a probe exists. The consequential behaviour (the temperature
 * limits survive a null endstop) is what these tests pin.
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../include/ui_update_queue.h"
#include "../../lvgl/lvgl.h"
#include "../ui_test_utils.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct LVGLInitializerSafetyLimits {
    LVGLInitializerSafetyLimits() {
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

static LVGLInitializerSafetyLimits lvgl_init;

/// Extruder max_temp used by these tests. Must exceed
/// SafetyLimits::max_temperature_celsius (400.0) so that parsing it produces an
/// observable change — that is the signal for "the temperature loop ran".
constexpr double PROBE_EXTRUDER_MAX_TEMP = 500.0;

class SafetyLimitsFixture {
  public:
    SafetyLimitsFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state_.init_subjects(false);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, state_);
        mock_client_.set_extruder_max_temp(PROBE_EXTRUDER_MAX_TEMP);
    }
    ~SafetyLimitsFixture() {
        // Ordered teardown: drain before each owner dies, so no queued callback
        // outlives the state it captured. Whatever is still queued when this
        // body returns gets executed by the NEXT test's HelixTestFixture ctor
        // drain, notifying freed subjects — a SIGSEGV in lv_subject_notify
        // blamed on whichever unrelated test constructs a fixture next. Same
        // rule ~XMLTestFixture follows for its own PrinterState.

        // notify_build_volume_changed() queues a lambda capturing `this` and
        // &build_volume_version_; run it while api_ is still alive.
        helix::ui::UpdateQueue::instance().drain();
        api_.reset();

        // update_safety_limits_from_printer() lands its results through
        // ui_queue_update(); those callbacks capture state_'s subjects, which
        // die the moment this body returns.
        helix::ui::UpdateQueue::instance().drain();
        state_.deinit_subjects();
    }

    /// Drive update_safety_limits_from_printer to completion.
    /// Returns true if the success callback fired.
    bool run_update() {
        std::atomic<bool> done{false};
        api_->update_safety_limits_from_printer([&]() { done.store(true); },
                                                [&](const MoonrakerError&) { done.store(true); });
        for (int i = 0; i < 100 && !done.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return done.load();
    }

    MoonrakerClientMock mock_client_;
    PrinterState state_;
    std::unique_ptr<MoonrakerAPI> api_;
};

} // namespace

TEST_CASE_METHOD(SafetyLimitsFixture, "numeric position_endstop parses the temperature limits",
                 "[safety_limits]") {
    // Baseline: with a numeric endstop the parse always reached the temperature
    // loop, so this passed before the fix too. It exists to prove the assertion
    // below is measuring the null case specifically and not a broken harness.
    mock_client_.set_stepper_z_endstop_null(false);
    REQUIRE(run_update());

    CHECK(api_->get_safety_limits().max_temperature_celsius ==
          Catch::Approx(PROBE_EXTRUDER_MAX_TEMP));
}

TEST_CASE_METHOD(SafetyLimitsFixture,
                 "null position_endstop does not abort the temperature-limit parse",
                 "[safety_limits]") {
    // The regression. Before the fix a bare .get<double>() threw type_error.302
    // on the null endstop, skipping the temperature loop entirely —
    // max_temperature_celsius stayed at the compiled 400.0 default. Revert the
    // fix and this reads 400.0 instead of PROBE_EXTRUDER_MAX_TEMP.
    mock_client_.set_stepper_z_endstop_null(true);
    REQUIRE(run_update());

    CHECK(api_->get_safety_limits().max_temperature_celsius ==
          Catch::Approx(PROBE_EXTRUDER_MAX_TEMP));
}

TEST_CASE_METHOD(SafetyLimitsFixture, "null position_endstop still reports success",
                 "[safety_limits]") {
    // Documents the trap that made the original bug invisible: the catch calls
    // on_success() too, so a firing success callback never proved the parse
    // completed. Kept so nobody mistakes success for correctness again — this
    // one passed both before and after the fix, by design.
    mock_client_.set_stepper_z_endstop_null(true);
    CHECK(run_update());
}
