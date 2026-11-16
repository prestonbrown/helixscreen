// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_stream_client.h"
#include "core/update_queue.h"
#include "hv/EventLoopThread.h"
#include "live_sweep_preview.h"
#include "sweep_synth.h"

#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/**
 * @file sweep_preview_fakes.h
 * @brief Batch sources for LiveSweepPreview tests
 *
 * ManualBatchSource hands the test the source's callbacks so batches and
 * errors arrive when the test says, on the test's own thread (standing in
 * for the loop thread). LoopBatchSource runs a real hv::EventLoopThread that
 * pushes SweepSynth batches, for the cases about thread crossings.
 */

namespace helix::calibration::test {

class ManualBatchSource;

/// What the factory and its sources saw.
struct SourceLog {
    int created = 0;
    int started = 0;
    int stopped = 0;
    std::string socket_path;
    std::string accel_chip;
    SweepPlan plan;
    ManualBatchSource* last = nullptr;
};

class ManualBatchSource final : public IAccelBatchSource {
  public:
    ManualBatchSource(SourceLog& log, float rate_hz) : log_(log), rate_hz_(rate_hz) {}

    bool start(const std::string& socket_path, const std::string& accel_chip,
               BatchCallback on_batch, ErrorCallback on_error) override {
        ++log_.started;
        log_.socket_path = socket_path;
        log_.accel_chip = accel_chip;
        on_batch_ = std::move(on_batch);
        on_error_ = std::move(on_error);
        running_ = true;
        return true;
    }

    void stop() override {
        if (running_) {
            running_ = false;
            ++log_.stopped;
        }
        if (log_.last == this) {
            log_.last = nullptr;
        }
    }

    ~ManualBatchSource() override {
        if (log_.last == this) {
            log_.last = nullptr;
        }
    }

    [[nodiscard]] float sample_rate_hz() const override {
        return rate_hz_;
    }

    void push(const AccelBatch& batch) {
        if (running_ && on_batch_) {
            on_batch_(batch);
        }
    }

    void fail(const std::string& message) {
        if (running_ && on_error_) {
            on_error_(message);
        }
    }

    [[nodiscard]] bool running() const {
        return running_;
    }

  private:
    SourceLog& log_;
    float rate_hz_;
    BatchCallback on_batch_;
    ErrorCallback on_error_;
    bool running_ = false;
};

inline LiveSweepPreview::SourceFactory manual_factory(SourceLog& log, float rate_hz = 3053.0f) {
    return [&log, rate_hz](const SweepPlan& plan) -> std::unique_ptr<IAccelBatchSource> {
        ++log.created;
        log.plan = plan;
        auto source = std::make_unique<ManualBatchSource>(log, rate_hz);
        log.last = source.get();
        return source;
    };
}

/// Consecutive SweepSynth batches of `batch_samples` each.
inline std::vector<AccelBatch> synth_batches(const SweepSynthSpec& spec, SweepAxisGain gain,
                                             size_t batch_samples, size_t count) {
    SweepSynth synth(spec, std::move(gain));
    std::vector<AccelBatch> out(count);
    for (auto& batch : out) {
        batch.samples = synth.next(batch_samples);
    }
    return out;
}

/**
 * @brief Feeds a ManualBatchSource a sweep that trails the console
 *
 * For tests that run the mock printer's transcript: after each "Testing
 * frequency" line, push samples until the drive reaches that line's value
 * + 0.5 Hz, so the console leads the motion the way Klipper's lookahead does.
 * Each new source (a new axis) gets a fresh synth over its own plan.
 */
class ConsoleLedFeeder {
  public:
    void feed(SourceLog& log, float latest_console_hz) {
        if (log.created != synth_for_source_) {
            synth_for_source_ = log.created;
            SweepSynthSpec spec;
            spec.plan = log.plan;
            spec.drive_start_s = 1.7;
            synth_ = std::make_unique<SweepSynth>(spec, [](float f) {
                const float d = (f - 53.8f) / 6.0f;
                const float level = 600.0f * std::sqrt(0.05f + 1.0f / (1.0f + d * d));
                return std::array<float, 3>{level, 0.6f * level, 0.2f * level};
            });
        }
        if (!synth_ || log.last == nullptr || latest_console_hz <= 0.0f) {
            return;
        }
        // Past the plan's end, keep the stream running for the last steps.
        const float end = log.plan.freq_end_hz;
        const float reach = latest_console_hz >= end ? end + 3.0f : latest_console_hz + 0.5f;
        const double until = 1.7 + (reach - log.plan.freq_start_hz) / log.plan.hz_per_sec;
        while (log.last != nullptr && synth_->time_s() < until) {
            AccelBatch batch;
            batch.samples = synth_->next(300);
            log.last->push(batch);
            helix::ui::UpdateQueue::instance().drain();
        }
    }

  private:
    int synth_for_source_ = 0;
    std::unique_ptr<SweepSynth> synth_;
};

/// A real loop thread pushing 300-sample SweepSynth batches every 5 ms.
class LoopBatchSource final : public IAccelBatchSource {
  public:
    LoopBatchSource(SweepSynthSpec spec, SweepAxisGain gain)
        : synth_(spec, std::move(gain)), rate_hz_(spec.rate_hz) {}

    ~LoopBatchSource() override {
        stop();
    }

    bool start(const std::string&, const std::string&, BatchCallback on_batch,
               ErrorCallback) override {
        on_batch_ = std::move(on_batch);
        loop_ = std::make_unique<hv::EventLoopThread>();
        loop_->start(true, [this]() -> int {
            loop_->loop()->setInterval(5, [this](hv::TimerID) {
                AccelBatch batch;
                batch.samples = synth_.next(300);
                ++batches_;
                on_batch_(batch);
            });
            return 0;
        });
        return true;
    }

    void stop() override {
        if (loop_) {
            loop_->stop();
            loop_->join();
            loop_.reset();
        }
    }

    [[nodiscard]] float sample_rate_hz() const override {
        return rate_hz_;
    }

    [[nodiscard]] int batches() const {
        return batches_.load();
    }

  private:
    SweepSynth synth_;
    float rate_hz_;
    BatchCallback on_batch_;
    std::unique_ptr<hv::EventLoopThread> loop_;
    std::atomic<int> batches_{0};
};

} // namespace helix::calibration::test
