// SPDX-License-Identifier: GPL-3.0-or-later
#include "motor_ripple_runner.h"

#include "belt_stream_client.h"
#include "core/http_executor.h"
#include "core/update_queue.h"
#include "i_moonraker_api.h"
#include "motor_ripple_analysis.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <ctime>
#include <mutex>
#include <vector>

namespace helix::calibration::ripple {

namespace {

constexpr size_t kMinCaptureSamples = 64;

/// What the stream thread writes. Shared with the batch callback so that no
/// stream-thread code ever holds the runner itself.
struct SampleBuffer {
    std::mutex m;
    std::vector<AccelSample> samples;
    uint64_t dirty_batches = 0;
};

bool active(MotorRippleRunner::Phase p) {
    using P = MotorRippleRunner::Phase;
    return p == P::Preparing || p == P::Measuring || p == P::Restoring;
}

} // namespace

struct MotorRippleRunner::Impl : std::enable_shared_from_this<MotorRippleRunner::Impl> {
    struct Pass {
        bool compensated = false;
        MotorId motor = MotorId::StepperX;
    };

    Impl(IMoonrakerAPI& a, SourceFactory f, Executor e)
        : api(a), factory(std::move(f)), executor(std::move(e)) {}

    IMoonrakerAPI& api;
    SourceFactory factory;
    Executor executor;
    Request req;
    Callbacks cb;
    Phase phase = Phase::Idle;
    uint64_t generation = 0;

    std::vector<Pass> passes;
    std::vector<float> speeds;
    size_t pass_i = 0;
    size_t speed_i = 0;
    bool retried = false;
    bool compensation_on = false;
    float sample_rate = 0.0f;
    std::map<MotorId, MotorSweep> off;
    std::map<MotorId, MotorSweep> on;

    std::unique_ptr<IAccelBatchSource> source;
    std::string source_chip;
    std::shared_ptr<SampleBuffer> buffer = std::make_shared<SampleBuffer>();
    uint64_t segment_dirty_start = 0;

    void set_phase(Phase p) {
        phase = p;
        if (cb.on_phase) {
            cb.on_phase(p);
        }
    }

    float electrical_mm(MotorId m) const {
        const auto it = req.geometry.find(m);
        return it == req.geometry.end() ? 0.0f : electrical_cycle_mm(it->second);
    }

    /// Send a script; `on_ok` runs on the main thread if the run has not moved
    /// on since. A failure fails the run.
    void send(const std::string& script, uint32_t timeout_ms, std::function<void()> on_ok) {
        std::weak_ptr<Impl> weak = shared_from_this();
        const uint64_t gen = generation;
        api.execute_gcode(
            script,
            [weak, gen, on_ok]() {
                helix::ui::queue_update("MotorRipple::sent", [weak, gen, on_ok]() {
                    auto self = weak.lock();
                    if (self && self->generation == gen && on_ok) {
                        on_ok();
                    }
                });
            },
            [weak, gen](const MoonrakerError& err) {
                const std::string why = err.message;
                helix::ui::queue_update("MotorRipple::send_failed", [weak, gen, why]() {
                    auto self = weak.lock();
                    if (self && self->generation == gen) {
                        self->fail("The printer refused a test move: " + why);
                    }
                });
            },
            timeout_ms, /*silent=*/true);
    }

    std::string cleanup_script() const {
        std::string s;
        if (req.mode == Mode::CompensationCompare && req.toggle.present()) {
            s += req.toggle.disable_gcode + "\n";
        }
        s += restore_gcode(req.plan);
        if (!req.tool_return_gcode.empty()) {
            s += "\n" + req.tool_return_gcode;
        }
        return s;
    }

    void stop_source() {
        if (source) {
            source->stop();
            source.reset();
        }
        source_chip.clear();
    }

    bool ensure_source(MotorId motor) {
        const auto it = req.accel_chip.find(motor);
        const std::string chip = it == req.accel_chip.end() ? std::string() : it->second;
        if (source && chip == source_chip) {
            return true;
        }
        stop_source();
        source = factory ? factory() : nullptr;
        if (!source) {
            return false;
        }
        std::shared_ptr<SampleBuffer> buf = buffer;
        std::weak_ptr<Impl> weak = shared_from_this();
        const bool ok = source->start(
            req.klippy_socket, chip,
            [buf](const AccelBatch& b) {
                std::lock_guard<std::mutex> lock(buf->m);
                buf->samples.insert(buf->samples.end(), b.samples.begin(), b.samples.end());
                if (!b.contiguous()) {
                    ++buf->dirty_batches;
                }
            },
            [weak](const std::string& why) {
                helix::ui::queue_update("MotorRipple::stream_error", [weak, why]() {
                    if (auto self = weak.lock()) {
                        self->fail("The accelerometer stream stopped: " + why);
                    }
                });
            });
        if (!ok) {
            source.reset();
            return false;
        }
        source_chip = chip;
        return true;
    }

    void next_capture() {
        if (pass_i >= passes.size()) {
            finish();
            return;
        }
        const Pass p = passes[pass_i];
        if (p.compensated && !compensation_on) {
            compensation_on = true;
            send(req.toggle.enable_gcode, 0, [this]() { next_capture(); });
            return;
        }
        if (!ensure_source(p.motor)) {
            fail("Could not open the accelerometer stream.");
            return;
        }
        const float speed = speeds[speed_i];
        const StepPlan step = plan_step(req.plan, p.motor, speed);
        if (step.refusal != SweepRefusal::None) {
            fail("The printer's travel cannot hold a test move.");
            return;
        }
        if (phase != Phase::Measuring) {
            set_phase(Phase::Measuring);
        }
        {
            std::lock_guard<std::mutex> lock(buffer->m);
            buffer->samples.clear();
            segment_dirty_start = buffer->dirty_batches;
        }
        spdlog::debug("[MotorRipple] {} motor {} at {:.1f} mm/s",
                      p.compensated ? "compensated" : "plain",
                      p.motor == MotorId::StepperX ? "stepper_x" : "stepper_y", speed);
        send(step.gcode, step.timeout_ms, [this]() { end_capture(); });
    }

    void end_capture() {
        std::vector<AccelSample> samples;
        bool dirty = false;
        {
            std::lock_guard<std::mutex> lock(buffer->m);
            samples.swap(buffer->samples);
            dirty = buffer->dirty_batches != segment_dirty_start;
        }
        const float speed = speeds[speed_i];
        if (dirty || samples.size() < kMinCaptureSamples) {
            if (!retried) {
                retried = true;
                spdlog::info("[MotorRipple] Capture at {:.1f} mm/s had gaps, retrying", speed);
                next_capture();
                return;
            }
            fail(fmt::format("The accelerometer dropped samples twice at {:.0f} mm/s.", speed));
            return;
        }
        retried = false;
        const float span = samples.back().time - samples.front().time;
        float rate = span > 0.0f ? static_cast<float>(samples.size() - 1) / span : 0.0f;
        if (!(rate > 0.0f) && source) {
            rate = source->sample_rate_hz();
        }
        sample_rate = rate;
        const float elec = electrical_mm(passes[pass_i].motor);
        const uint64_t gen = generation;
        std::weak_ptr<Impl> weak = shared_from_this();
        auto work = [weak, gen, rate, speed, elec, samples = std::move(samples)]() {
            SpeedResult r = analyze_capture(samples, rate, speed, elec);
            helix::ui::queue_update("MotorRipple::analysed", [weak, gen, r]() {
                auto self = weak.lock();
                if (self && self->generation == gen) {
                    self->on_analysed(r);
                }
            });
        };
        if (executor) {
            executor(std::move(work));
        } else {
            (void)http::HttpExecutor::fast().submit(std::move(work));
        }
    }

    void on_analysed(const SpeedResult& r) {
        const Pass p = passes[pass_i];
        auto& sweeps = p.compensated ? on : off;
        auto& sweep = sweeps[p.motor];
        sweep.motor = p.motor;
        sweep.speeds.push_back(r);
        const size_t index = pass_i * speeds.size() + speed_i;
        if (cb.on_speed) {
            cb.on_speed(p.compensated, p.motor, r, index, passes.size() * speeds.size());
        }
        if (++speed_i >= speeds.size()) {
            speed_i = 0;
            ++pass_i;
        }
        next_capture();
    }

    void finish() {
        stop_source();
        set_phase(Phase::Restoring);
        ++generation;
        send(cleanup_script(), 0, [this]() {
            SweepResult result;
            result.sample_rate_hz = sample_rate;
            result.ceiling_hz = ceiling_hz(sample_rate);
            result.unix_time = static_cast<int64_t>(std::time(nullptr));
            for (auto& [m, s] : off) {
                result.sweeps.push_back(s);
            }
            for (auto& [m, s] : on) {
                result.compensated_sweeps.push_back(s);
            }
            const auto& banded =
                result.compensated_sweeps.empty() ? result.sweeps : result.compensated_sweeps;
            for (const auto& s : banded) {
                const auto b = find_bands(s, electrical_mm(s.motor));
                result.bands.insert(result.bands.end(), b.begin(), b.end());
            }
            set_phase(Phase::Done);
            if (cb.on_done) {
                cb.on_done(result);
            }
        });
    }

    /// Stop measuring and put the printer back, then report `terminal`.
    void wind_down(Phase terminal, const std::string& reason) {
        if (!active(phase) || phase == Phase::Restoring) {
            return;
        }
        stop_source();
        ++generation;
        set_phase(Phase::Restoring);
        auto done = [this, terminal, reason]() {
            set_phase(terminal);
            if (terminal == Phase::Failed && cb.on_failed) {
                cb.on_failed(reason);
            }
        };
        std::weak_ptr<Impl> weak = shared_from_this();
        const uint64_t gen = generation;
        api.execute_gcode(
            cleanup_script(),
            [weak, gen, done]() {
                helix::ui::queue_update("MotorRipple::restored", [weak, gen, done]() {
                    auto self = weak.lock();
                    if (self && self->generation == gen) {
                        done();
                    }
                });
            },
            [weak, gen, done](const MoonrakerError& err) {
                spdlog::warn("[MotorRipple] Restore after the run failed: {}", err.message);
                helix::ui::queue_update("MotorRipple::restore_failed", [weak, gen, done]() {
                    auto self = weak.lock();
                    if (self && self->generation == gen) {
                        done();
                    }
                });
            },
            0, /*silent=*/true);
    }

    void fail(const std::string& reason) {
        spdlog::warn("[MotorRipple] Run failed: {}", reason);
        wind_down(Phase::Failed, reason);
    }
};

MotorRippleRunner::MotorRippleRunner(IMoonrakerAPI& api, SourceFactory sources, Executor analysis)
    : impl_(std::make_shared<Impl>(api, std::move(sources), std::move(analysis))) {}

MotorRippleRunner::~MotorRippleRunner() {
    if (!impl_) {
        return;
    }
    impl_->stop_source();
    if (impl_->phase == Phase::Preparing || impl_->phase == Phase::Measuring) {
        ++impl_->generation;
        impl_->api.execute_gcode(impl_->cleanup_script(), nullptr, nullptr, 0, /*silent=*/true);
    }
    impl_.reset();
}

bool MotorRippleRunner::start(Request req, Callbacks cb) {
    if (active(impl_->phase)) {
        return false;
    }
    const auto motors = motors_to_measure(req.plan);
    const auto speeds = sweep_speeds(req.plan.max_velocity_mm_s);
    if (motors.empty() || speeds.empty()) {
        return false;
    }
    for (MotorId m : motors) {
        const auto g = req.geometry.find(m);
        if (g == req.geometry.end() || !(electrical_cycle_mm(g->second) > 0.0f)) {
            return false;
        }
        if (plan_step(req.plan, m, speeds.front()).refusal != SweepRefusal::None) {
            return false;
        }
    }
    const bool compare = req.mode == Mode::CompensationCompare;
    if (compare && !req.toggle.present()) {
        return false;
    }

    Impl& s = *impl_;
    ++s.generation;
    s.req = std::move(req);
    s.cb = std::move(cb);
    s.speeds = speeds;
    s.passes.clear();
    for (bool comp : compare ? std::vector<bool>{false, true} : std::vector<bool>{false}) {
        for (MotorId m : motors) {
            s.passes.push_back({comp, m});
        }
    }
    s.pass_i = 0;
    s.speed_i = 0;
    s.retried = false;
    s.compensation_on = false;
    s.off.clear();
    s.on.clear();
    s.set_phase(Phase::Preparing);

    std::string prologue;
    if (!s.req.tool_select_gcode.empty()) {
        prologue += s.req.tool_select_gcode + "\n";
    }
    prologue += begin_gcode(s.req.plan);
    if (compare) {
        prologue += "\n" + s.req.toggle.disable_gcode;
    }
    Impl* raw = impl_.get();
    s.send(prologue, 0, [raw]() { raw->next_capture(); });
    return true;
}

void MotorRippleRunner::cancel() {
    impl_->wind_down(Phase::Cancelled, {});
}

MotorRippleRunner::Phase MotorRippleRunner::phase() const {
    return impl_->phase;
}

} // namespace helix::calibration::ripple
