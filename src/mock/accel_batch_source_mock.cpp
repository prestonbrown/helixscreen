// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "accel_batch_source_mock.h"

#include "belt_stream_client.h"
#include "hv/EventLoopThread.h"

#include <spdlog/spdlog.h>

#include <cmath>

namespace helix::calibration {

namespace {

constexpr float kMockRateHz = 3053.0f;

SweepSynthSpec mock_spec(const SweepPlan& plan, const MockSweepResponse& response) {
    SweepSynthSpec spec;
    spec.plan = plan;
    spec.drive_start_s = MockAccelBatchSource::kDriveLeadS;
    spec.rate_hz = kMockRateHz;
    spec.noise_mm_s2 = kMockNoiseFraction * response.drive_mm_s2;
    return spec;
}

} // namespace

std::array<float, 3> mock_sweep_axis_gain(const MockSweepResponse& response, float drive_hz) {
    float level = kMockFloor;
    if (std::isfinite(response.peak_hz)) {
        const float d = (drive_hz - response.peak_hz) / response.half_width_hz;
        level += 1.0f / (1.0f + d * d);
    }
    const float amplitude = response.drive_mm_s2 * std::sqrt(level);
    return {amplitude * response.axis_weight[0], amplitude * response.axis_weight[1],
            amplitude * response.axis_weight[2]};
}

MockAccelBatchSource::MockAccelBatchSource(SweepPlan plan, MockSweepResponse response,
                                           double sim_speed)
    : synth_(mock_spec(plan, response),
             [response](float drive_hz) { return mock_sweep_axis_gain(response, drive_hz); }),
      sim_speed_(sim_speed > 0.0 ? sim_speed : 1.0) {}

MockAccelBatchSource::~MockAccelBatchSource() {
    stop();
}

bool MockAccelBatchSource::start(const std::string& /*socket_path*/, const std::string& accel_chip,
                                 BatchCallback on_batch, ErrorCallback /*on_error*/) {
    if (loop_thread_) {
        return false;
    }
    on_batch_ = std::move(on_batch);
    running_.store(true);
    spdlog::info("[MockAccelBatchSource] Streaming a synthetic sweep for {} at {:.1f}x", accel_chip,
                 sim_speed_);
    loop_thread_ = std::make_unique<hv::EventLoopThread>();
    loop_thread_->start(true, [this]() -> int {
        loop_thread_->loop()->setInterval(static_cast<int>(kBatchIntervalMs),
                                          [this](hv::TimerID) { emit_batch(); });
        return 0;
    });
    return true;
}

void MockAccelBatchSource::stop() {
    running_.store(false);
    if (loop_thread_) {
        loop_thread_->stop();
        loop_thread_->join();
        loop_thread_.reset();
    }
}

float MockAccelBatchSource::sample_rate_hz() const {
    return running_.load() ? kMockRateHz : 0.0f;
}

void MockAccelBatchSource::emit_batch() {
    if (!running_.load() || !on_batch_) {
        return;
    }
    const auto count =
        static_cast<size_t>(std::lround(kMockRateHz * (kBatchIntervalMs / 1000.0) * sim_speed_));
    AccelBatch batch;
    batch.samples = synth_.next(count);
    on_batch_(batch);
}

} // namespace helix::calibration
