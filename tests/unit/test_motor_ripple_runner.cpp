// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_test_fixture.h"
#include "belt_stream_client.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "motor_ripple_analysis.h"
#include "motor_ripple_runner.h"
#include "printer_state.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;
using namespace helix::calibration::ripple;

namespace {

constexpr float kRate = 3200.0f;
constexpr float kElecMm = 0.8f;

/// Ripple whose amplitude peaks where its frequency meets a 216 Hz resonance.
float synth_amp(float freq_hz) {
    const float d = (freq_hz - 216.0f) / 20.0f;
    return 2500.0f / (1.0f + d * d) + 50.0f;
}

/// A stream the test drives: emit() pushes one speed's capture synchronously.
class ScriptedSource final : public IAccelBatchSource {
  public:
    explicit ScriptedSource(int& alive) : alive_(alive) {
        ++alive_;
    }
    ~ScriptedSource() override {
        --alive_;
    }
    bool start(const std::string&, const std::string&, BatchCallback on_batch,
               ErrorCallback on_error) override {
        on_batch_ = std::move(on_batch);
        on_error_ = std::move(on_error);
        running = true;
        return true;
    }
    void stop() override {
        running = false;
    }
    [[nodiscard]] float sample_rate_hz() const override {
        return kRate;
    }

    void emit(float speed, bool dirty) {
        AccelBatch b;
        const int n = static_cast<int>(2.5f * kRate);
        for (int i = 0; i < n; ++i) {
            const float t = t_ + static_cast<float>(i) / kRate;
            float x = 0.0f;
            for (int k : kHarmonics) {
                const float f = ripple_freq_hz(speed, k, kElecMm);
                x += synth_amp(f) * std::sin(6.2831853f * f * t);
            }
            b.samples.push_back({t, x, 0.0f, 9810.0f});
        }
        t_ += 3.0f;
        b.overflows = dirty ? 1 : 0;
        on_batch_(b);
    }
    void fail(const std::string& why) {
        on_error_(why);
    }

    bool running = false;

  private:
    int& alive_;
    BatchCallback on_batch_;
    ErrorCallback on_error_;
    float t_ = 0.0f;
};

/// Records every script; a speed script makes the source emit before it acks.
class SweepAPI : public MoonrakerAPI {
  public:
    using MoonrakerAPI::MoonrakerAPI;

    void execute_gcode(const std::string& gcode, SuccessCallback on_success, ErrorCallback,
                       uint32_t = 0, bool = false, SuccessCallback = nullptr, bool = true,
                       bool = false) override {
        scripts.push_back(gcode);
        if (gcode.find("G4 P250") != std::string::npos) {
            ++speed_scripts;
            if (hold_speeds) {
                held = std::move(on_success);
                return;
            }
            float x = 0, y = 0, f = 0;
            const auto second = gcode.find("\nG1 ");
            std::sscanf(gcode.c_str() + second + 1, "G1 X%f Y%f F%f", &x, &y, &f);
            if (source) {
                source->emit(f / 60.0f * std::sqrt(2.0f), dirty_remaining > 0);
            }
            if (dirty_remaining > 0) {
                --dirty_remaining;
            }
        }
        if (on_success) {
            on_success();
        }
    }

    std::vector<std::string> scripts;
    int speed_scripts = 0;
    int dirty_remaining = 0;
    bool hold_speeds = false;
    SuccessCallback held;
    ScriptedSource* source = nullptr;
};

class RunnerFixture : public LVGLTestFixture {
  public:
    RunnerFixture() : client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        state.set_klippy_state_sync(helix::KlippyState::READY);
        api = std::make_unique<SweepAPI>(client, state);
        runner = std::make_unique<MotorRippleRunner>(
            *api,
            [this]() {
                auto s = std::make_unique<ScriptedSource>(sources_alive);
                api->source = s.get();
                return s;
            },
            [](std::function<void()> work) { work(); });
    }
    ~RunnerFixture() override {
        runner.reset();
        drain();
    }
    // Each speed is two queued hops (script ack, analysis), so a whole run needs
    // hundreds of passes.
    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty(1000);
    }

    MotorRippleRunner::Request request(MotorRippleRunner::Mode mode) {
        MotorRippleRunner::Request r;
        r.mode = mode;
        r.plan.kinematics = Kinematics::CoreXY;
        r.plan.x_max = 300.0f;
        r.plan.y_max = 305.0f;
        r.plan.has_xy_limits = true;
        r.plan.probe_point = std::array<float, 3>{150.0f, 150.0f, 20.0f};
        r.plan.max_velocity_mm_s = 500.0f;
        r.plan.max_accel_mm_s2 = 30000.0f;
        r.plan.max_z = 280.0f;
        r.plan.minimum_cruise_ratio = 0.5f;
        r.geometry[MotorId::StepperX] = {40.0f, 1.0f, 200};
        r.geometry[MotorId::StepperY] = {40.0f, 1.0f, 200};
        r.accel_chip[MotorId::StepperX] = "adxl345";
        r.accel_chip[MotorId::StepperY] = "adxl345";
        r.klippy_socket = "/tmp/klippy_uds";
        r.toggle.enable_gcode = "DAMP_ON";
        r.toggle.disable_gcode = "DAMP_OFF";
        return r;
    }

    MotorRippleRunner::Callbacks callbacks() {
        MotorRippleRunner::Callbacks cb;
        cb.on_phase = [this](MotorRippleRunner::Phase p) { phases.push_back(p); };
        cb.on_speed = [this](bool, MotorId, const SpeedResult&, size_t, size_t total) {
            ++speeds;
            total_speeds = total;
        };
        cb.on_done = [this](const SweepResult& r) { result = r; };
        cb.on_failed = [this](const std::string& why) { failure = why; };
        return cb;
    }

    [[nodiscard]] size_t index_of(const std::string& needle, size_t from = 0) const {
        for (size_t i = from; i < api->scripts.size(); ++i) {
            if (api->scripts[i].find(needle) != std::string::npos) {
                return i;
            }
        }
        return api->scripts.size();
    }
    [[nodiscard]] bool last_script_restores() const {
        return !api->scripts.empty() &&
               api->scripts.back().find("RESTORE_GCODE_STATE NAME=helix_ripple_sweep") !=
                   std::string::npos;
    }

    MoonrakerClientMock client;
    helix::PrinterState state;
    std::unique_ptr<SweepAPI> api;
    std::unique_ptr<MotorRippleRunner> runner;
    std::vector<MotorRippleRunner::Phase> phases;
    int speeds = 0;
    int sources_alive = 0;
    size_t total_speeds = 0;
    std::optional<SweepResult> result;
    std::optional<std::string> failure;
};

} // namespace

TEST_CASE_METHOD(RunnerFixture, "a sweep measures every speed on both motors, then restores",
                 "[ripple][runner]") {
    REQUIRE(runner->start(request(MotorRippleRunner::Mode::Sweep), callbacks()));
    drain();
    REQUIRE(result.has_value());
    CHECK_FALSE(failure.has_value());
    CHECK(speeds == 74);
    CHECK(total_speeds == 74);
    CHECK(api->speed_scripts == 74);
    CHECK(index_of("SAVE_GCODE_STATE NAME=helix_ripple_sweep") < index_of("G4 P250"));
    CHECK(last_script_restores());
    CHECK(index_of("DAMP_") == api->scripts.size()); // a plain sweep never touches compensation
    CHECK(result->ceiling_hz == Catch::Approx(0.45f * kRate).margin(5.0f));
    REQUIRE(result->sweeps.size() == 2);
    CHECK(result->compensated_sweeps.empty());
    // The synthetic resonance puts the 2x on 216 Hz at 86.4 mm/s.
    CHECK(band_containing(result->bands, 86.0f) != nullptr);
    CHECK(runner->phase() == MotorRippleRunner::Phase::Done);
    CHECK(sources_alive == 0);
}

TEST_CASE_METHOD(RunnerFixture, "a comparison sweeps with compensation off, then on, then off",
                 "[ripple][runner]") {
    REQUIRE(runner->start(request(MotorRippleRunner::Mode::CompensationCompare), callbacks()));
    drain();
    REQUIRE(result.has_value());
    const size_t off = index_of("DAMP_OFF");
    const size_t on = index_of("DAMP_ON");
    CHECK(off < index_of("G4 P250"));
    CHECK(on > index_of("G4 P250"));
    CHECK(index_of("DAMP_OFF", on) < api->scripts.size());
    CHECK(last_script_restores());
    CHECK(api->speed_scripts == 148);
    CHECK(result->compensated_sweeps.size() == 2);
}

TEST_CASE_METHOD(RunnerFixture, "a capture with dropped samples is retried once, then fails",
                 "[ripple][runner]") {
    api->dirty_remaining = 1;
    REQUIRE(runner->start(request(MotorRippleRunner::Mode::Sweep), callbacks()));
    drain();
    CHECK(result.has_value());
    CHECK(api->speed_scripts == 75);

    result.reset();
    runner = std::make_unique<MotorRippleRunner>(
        *api,
        [this]() {
            auto s = std::make_unique<ScriptedSource>(sources_alive);
            api->source = s.get();
            return s;
        },
        [](std::function<void()> work) { work(); });
    api->scripts.clear();
    api->speed_scripts = 0;
    api->dirty_remaining = 2;
    REQUIRE(runner->start(request(MotorRippleRunner::Mode::Sweep), callbacks()));
    drain();
    CHECK_FALSE(result.has_value());
    CHECK(failure.has_value());
    CHECK(api->speed_scripts == 2);
    CHECK(last_script_restores());
    CHECK(runner->phase() == MotorRippleRunner::Phase::Failed);
}

TEST_CASE_METHOD(RunnerFixture, "cancel mid-speed stops the sweep and restores the printer",
                 "[ripple][runner]") {
    api->hold_speeds = true;
    REQUIRE(runner->start(request(MotorRippleRunner::Mode::CompensationCompare), callbacks()));
    drain();
    REQUIRE(api->speed_scripts == 1);
    runner->cancel();
    drain();
    CHECK(last_script_restores());
    CHECK(api->scripts.back().find("DAMP_OFF") != std::string::npos);
    api->held(); // the in-flight speed finishing late sends nothing more
    drain();
    CHECK(api->speed_scripts == 1);
    CHECK(last_script_restores());
    CHECK(runner->phase() == MotorRippleRunner::Phase::Cancelled);
    CHECK_FALSE(result.has_value());
}

TEST_CASE_METHOD(RunnerFixture, "destroying a running runner restores and calls nothing back",
                 "[ripple][runner]") {
    api->hold_speeds = true;
    REQUIRE(runner->start(request(MotorRippleRunner::Mode::Sweep), callbacks()));
    drain();
    const size_t phases_before = phases.size();
    runner.reset();
    CHECK(last_script_restores());
    api->held();
    drain();
    CHECK(phases.size() == phases_before);
    CHECK_FALSE(result.has_value());
    CHECK_FALSE(failure.has_value());
}

TEST_CASE_METHOD(RunnerFixture, "a stalled stream fails the run with the printer restored",
                 "[ripple][runner]") {
    api->hold_speeds = true;
    REQUIRE(runner->start(request(MotorRippleRunner::Mode::Sweep), callbacks()));
    drain();
    api->source->fail("no batch for 2 s");
    drain();
    CHECK(failure.has_value());
    CHECK(last_script_restores());
    CHECK(runner->phase() == MotorRippleRunner::Phase::Failed);
}

TEST_CASE_METHOD(RunnerFixture, "a refused plan or a busy runner does not start",
                 "[ripple][runner]") {
    auto req = request(MotorRippleRunner::Mode::Sweep);
    req.plan.kinematics = Kinematics::Unsupported;
    CHECK_FALSE(runner->start(req, callbacks()));
    CHECK(api->scripts.empty());

    api->hold_speeds = true;
    REQUIRE(runner->start(request(MotorRippleRunner::Mode::Sweep), callbacks()));
    drain();
    CHECK_FALSE(runner->start(request(MotorRippleRunner::Mode::Sweep), callbacks()));
}
