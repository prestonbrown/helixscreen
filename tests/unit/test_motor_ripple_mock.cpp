// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_test_fixture.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "motor_ripple_analysis.h"
#include "motor_ripple_runner.h"
#include "printer_state.h"
#include "ripple_batch_source_mock.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration::ripple;

namespace {

/// The mock printer end to end: the runner's scripts go through the mock's own
/// gcode handler, which feeds the mock stream.
std::optional<SweepResult> run_on_mock(float rate, MotorRippleRunner::Mode mode) {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    state.set_klippy_state_sync(helix::KlippyState::READY);
    client.connect("ws://mock/websocket", []() {}, []() {});
    MoonrakerAPI api(client, state);
    MotorRippleRunner runner(
        api, [rate]() { return std::make_unique<MockRippleBatchSource>(rate); },
        [](std::function<void()> work) { work(); });

    MotorRippleRunner::Request req;
    req.mode = mode;
    req.plan.kinematics = Kinematics::CoreXY;
    req.plan.x_max = 200.0f; // inside every mock persona's travel
    req.plan.y_max = 200.0f;
    req.plan.has_xy_limits = true;
    req.plan.max_velocity_mm_s = 500.0f;
    req.plan.max_accel_mm_s2 = 10000.0f;
    req.plan.minimum_cruise_ratio = 0.5f;
    req.geometry[MotorId::StepperX] = {40.0f, 1.0f, 200};
    req.geometry[MotorId::StepperY] = {40.0f, 1.0f, 200};
    req.toggle.enable_gcode = "STEPPER_RESONANCE_DAMP_ENABLE";
    req.toggle.disable_gcode = "STEPPER_RESONANCE_DAMP_DISABLE";
    std::optional<SweepResult> out;
    MotorRippleRunner::Callbacks cb;
    cb.on_done = [&out](const SweepResult& r) { out = r; };
    cb.on_failed = [](const std::string& why) { UNSCOPED_INFO("run failed: " << why); };
    REQUIRE(runner.start(req, cb));
    helix::ui::UpdateQueue::instance().drain_until_empty(2000);
    client.disconnect();
    return out;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "the mock printer's ripple meets its resonance at 86 mm/s",
                 "[ripple][mock]") {
    const auto r = run_on_mock(3200.0f, MotorRippleRunner::Mode::Sweep);
    REQUIRE(r.has_value());
    const auto bands = merge_bands(r->bands);
    const RippleBand* b = band_containing(bands, 86.0f);
    REQUIRE(b != nullptr);
    CHECK(b->harmonic == 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "the mock's compensation cuts its ripple", "[ripple][mock]") {
    const auto r = run_on_mock(3200.0f, MotorRippleRunner::Mode::CompensationCompare);
    REQUIRE(r.has_value());
    REQUIRE(r->sweeps.size() == 2);
    REQUIRE(r->compensated_sweeps.size() == 2);
    float off = 0.0f;
    float on = 0.0f;
    for (const auto& s : r->sweeps[0].speeds) {
        off = std::max(off, s.ripple[1].value_or(0.0f));
    }
    for (const auto& s : r->compensated_sweeps[0].speeds) {
        on = std::max(on, s.ripple[1].value_or(0.0f));
    }
    CHECK(on < off * 0.4f);
}

TEST_CASE_METHOD(LVGLTestFixture, "a slow mock accelerometer leaves high ripples unmeasured",
                 "[ripple][mock]") {
    const auto r = run_on_mock(387.0f, MotorRippleRunner::Mode::Sweep);
    REQUIRE(r.has_value());
    const float elec = 0.8f;
    for (const auto& s : r->sweeps[0].speeds) {
        if (ripple_freq_hz(s.speed_mm_s, 2, elec) > 0.45f * 387.0f) {
            CHECK_FALSE(s.ripple[1].has_value());
        }
        if (ripple_freq_hz(s.speed_mm_s, 4, elec) > 0.45f * 387.0f) {
            CHECK_FALSE(s.ripple[2].has_value());
        }
    }
}
