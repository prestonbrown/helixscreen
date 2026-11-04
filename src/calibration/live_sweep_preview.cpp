// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "live_sweep_preview.h"

#include "app_globals.h"
#include "belt_stream_client.h"
#include "core/text_io.h"
#include "env_knobs.h"
#include "helix_thread.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "runtime_config.h"

#ifdef HELIX_ENABLE_MOCKS
#include "moonraker_client_mock.h"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>

#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace helix::calibration {

namespace {

std::optional<std::string> non_empty(const std::string& chip) {
    const auto trimmed = text_io::trim(chip);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    return std::string(trimmed);
}

} // namespace

std::optional<std::string> accel_chip_for_sweep(const ResonanceTesterConfig& cfg, SweepAxis axis) {
    const auto chip = non_empty(cfg.accel_chip);
    const auto chip_x = non_empty(cfg.accel_chip_x);
    const auto chip_y = non_empty(cfg.accel_chip_y);
    switch (axis) {
    case SweepAxis::X:
        return chip_x ? chip_x : chip;
    case SweepAxis::Y:
        return chip_y ? chip_y : chip;
    case SweepAxis::DIAGONAL:
        if (!chip_x && !chip_y) {
            return chip;
        }
        // Klipper's resonance tester reads equal per-axis chips as one chip.
        if (chip_x && chip_y && *chip_x == *chip_y) {
            return chip_x;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<SweepPlan> sweep_plan_for(const ResonanceTesterConfig& cfg,
                                        std::optional<BeltSweepWindow> window) {
    SweepPlan plan{cfg.min_freq, cfg.max_freq, cfg.hz_per_sec};
    if (window) {
        plan.freq_start_hz = std::floor(window->freq_start_hz);
        plan.freq_end_hz = std::ceil(window->freq_end_hz);
    }
    if (!plan.valid()) {
        return std::nullopt;
    }
    return plan;
}

std::optional<LiveSweepTarget> live_sweep_target(const LiveSweepFacts& facts, SweepAxis axis,
                                                 std::optional<BeltSweepWindow> window) {
    if (facts.socket_path.empty() || !facts.socket_reachable) {
        return std::nullopt;
    }
    auto chip = accel_chip_for_sweep(facts.cfg, axis);
    if (!chip) {
        return std::nullopt;
    }
    const auto plan = sweep_plan_for(facts.cfg, window);
    if (!plan) {
        return std::nullopt;
    }
    return LiveSweepTarget{facts.socket_path, std::move(*chip), *plan};
}

PreviewSeries layout_preview(const std::vector<SweepPoint>& points, SweepColumn column,
                             float axis_lo_hz, float axis_hi_hz) {
    PreviewSeries out;
    if (!std::isfinite(axis_lo_hz) || !std::isfinite(axis_hi_hz) || axis_hi_hz <= axis_lo_hz) {
        return out;
    }
    const double lo = axis_lo_hz;
    const double hi = axis_hi_hz;
    const auto n = static_cast<size_t>(std::lround((hi - lo) / sweep_preview::kStepHz)) + 1;
    const double spacing = n > 1 ? (hi - lo) / static_cast<double>(n - 1) : 0.0;
    const double reach = sweep_preview::kStepHz / 2.0;
    out.freqs.resize(n);
    out.amps.assign(n, std::numeric_limits<float>::quiet_NaN());

    for (size_t i = 0; i < n; ++i) {
        const double slot = lo + static_cast<double>(i) * spacing;
        out.freqs[i] = static_cast<float>(slot);

        // Points arrive in sweep order, so frequency ascending.
        const auto above =
            std::lower_bound(points.begin(), points.end(), slot, [](const SweepPoint& p, double f) {
                return static_cast<double>(p.freq_hz) < f;
            });
        const SweepPoint* nearest = nullptr;
        double nearest_distance = std::numeric_limits<double>::infinity();
        if (above != points.begin()) {
            const auto& below = *(above - 1);
            nearest = &below;
            nearest_distance = slot - static_cast<double>(below.freq_hz);
        }
        // Strictly closer: a tie keeps the lower frequency's point.
        if (above != points.end() &&
            static_cast<double>(above->freq_hz) - slot < nearest_distance) {
            nearest = &*above;
            nearest_distance = static_cast<double>(above->freq_hz) - slot;
        }
        if (nearest == nullptr || nearest_distance > reach) {
            continue;
        }
        const float amp = sweep_column_value(*nearest, column);
        out.amps[i] = amp;
        if (std::isfinite(amp)) {
            out.max_amp = std::max(out.max_amp, amp);
        }
    }
    return out;
}

std::string parse_klippy_uds_address(const nlohmann::json& data) {
    if (!data.is_object()) {
        return {};
    }
    const nlohmann::json* root = &data;
    if (const auto result = data.find("result"); result != data.end() && result->is_object()) {
        root = &*result;
    }
    const auto config = root->find("config");
    if (config == root->end() || !config->is_object()) {
        return {};
    }
    const auto server = config->find("server");
    if (server == config->end() || !server->is_object()) {
        return {};
    }
    const auto address = server->find("klippy_uds_address");
    if (address == server->end() || !address->is_string()) {
        return {};
    }
    return address->get_ref<const std::string&>();
}

void query_klippy_uds_address(IMoonrakerAPI& api, std::function<void(std::string)> on_done) {
    api.rest().get_server_config(
        [on_done](const RestResponse& resp) { on_done(parse_klippy_uds_address(resp.data)); },
        [on_done](const MoonrakerError& err) {
            spdlog::debug("[LiveSweepPreview] /server/config failed: {}", err.message);
            on_done({});
        });
}

void query_live_sweep_facts(IMoonrakerAPI& api, std::function<void(LiveSweepFacts)> on_done) {
    IMoonrakerAPI* api_ptr = &api;
    query_klippy_uds_address(api, [api_ptr, on_done](std::string path) {
        LiveSweepFacts facts;
        facts.socket_reachable = !path.empty() && BeltStreamClient::socket_reachable(path);
        facts.socket_path = std::move(path);
        query_resonance_tester_config(api_ptr->get_client(),
                                      [facts, on_done](ResonanceTesterConfig cfg) mutable {
                                          facts.cfg = std::move(cfg);
                                          on_done(std::move(facts));
                                      });
    });
}

namespace {

/// klippy's dump endpoint through an unchanged BeltStreamClient.
class StreamClientBatchSource final : public IAccelBatchSource {
  public:
    bool start(const std::string& socket_path, const std::string& accel_chip,
               BatchCallback on_batch, ErrorCallback on_error) override {
        return client_.start(socket_path, accel_chip, std::move(on_batch), std::move(on_error));
    }
    void stop() override {
        client_.stop();
    }
    [[nodiscard]] float sample_rate_hz() const override {
        return client_.sample_rate_hz();
    }

  private:
    BeltStreamClient client_;
};

/// HELIX_SWEEP_PREVIEW_OFF=1 turns the preview off for one run, for measuring
/// what it costs on the board.
bool preview_disabled() {
    if (!helix::env_flag("HELIX_SWEEP_PREVIEW_OFF")) {
        return false;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        spdlog::info("[LiveSweepPreview] HELIX_SWEEP_PREVIEW_OFF set: no live sweep preview");
    }
    return true;
}

} // namespace

std::unique_ptr<IAccelBatchSource> make_accel_batch_source(const SweepPlan& plan) {
#ifdef HELIX_ENABLE_MOCKS
    if (get_runtime_config()->should_mock_moonraker()) {
        if (MoonrakerClientMock* mock = get_moonraker_client_mock()) {
            return mock->make_accel_batch_source(plan);
        }
        spdlog::warn("[LiveSweepPreview] mock printer without a mock client: no live preview");
        return nullptr;
    }
#else
    (void)plan;
#endif
    return std::make_unique<StreamClientBatchSource>();
}

LiveSweepPreview::LiveSweepPreview() : factory_(make_accel_batch_source) {}

LiveSweepPreview::~LiveSweepPreview() {
    stop();
}

void LiveSweepPreview::set_source_factory(SourceFactory factory) {
    if (state_ != State::IDLE) {
        spdlog::warn("[LiveSweepPreview] source factory change ignored outside IDLE");
        return;
    }
    factory_ = factory ? std::move(factory) : SourceFactory(make_accel_batch_source);
}

void LiveSweepPreview::arm(LiveSweepTarget target, PointsCallback on_points) {
    stop();
    points_.clear();
    if (preview_disabled()) {
        return;
    }
    target_ = std::move(target);
    on_points_ = std::move(on_points);
    hint_hz_.store(0.0f);
    state_ = State::ARMED;
}

void LiveSweepPreview::note_console_frequency(float hz) {
    if (!std::isfinite(hz) || hz <= 0.0f) {
        return;
    }
    if (state_ == State::ARMED) {
        hint_hz_.store(hz);
        attach();
    } else if (state_ == State::STREAMING) {
        hint_hz_.store(hz);
        if (hz >= target_->plan.freq_end_hz - kPreviewLastLineSlackHz) {
            console_ended_.store(true);
        }
    }
}

void LiveSweepPreview::attach() {
    accumulator_ = std::make_unique<SweepResponseAccumulator>(target_->plan);
    finishing_.store(false);
    console_ended_.store(false);
    finish_deadline_s_ = 0.0;
    stop_requested_ = false;
    no_lock_logged_ = false;
    niced_ = false;
    loop_cpu_first_ns_ = -1;
    loop_cpu_ns_.store(0);
    stream_time_s_.store(0.0);

    source_ = factory_ ? factory_(target_->plan) : nullptr;
    if (!source_) {
        spdlog::info("[LiveSweepPreview] {}: no accelerometer stream, no live preview",
                     target_->accel_chip);
        accumulator_.reset();
        state_ = State::DONE;
        return;
    }

    stream_token_.emplace(lifetime_.token());
    state_ = State::STREAMING;
    const bool started = source_->start(
        target_->socket_path, target_->accel_chip,
        [this](const AccelBatch& batch) { on_batch(batch); },
        [this](const std::string& message) { on_source_error(message); });
    if (!started) {
        spdlog::info("[LiveSweepPreview] {}: stream did not start, no live preview",
                     target_->accel_chip);
        stop_source();
        state_ = State::DONE;
        return;
    }
    spdlog::debug("[LiveSweepPreview] {}: streaming {:.0f}-{:.0f} Hz at {:.2f} Hz/s",
                  target_->accel_chip, target_->plan.freq_start_hz, target_->plan.freq_end_hz,
                  target_->plan.hz_per_sec);
}

void LiveSweepPreview::on_batch(const AccelBatch& batch) {
    if (stop_requested_ || !accumulator_ || !source_) {
        return;
    }
    if (!niced_) {
        niced_ = true;
#ifdef __linux__
        const auto tid = static_cast<id_t>(::syscall(SYS_gettid));
        if (::setpriority(PRIO_PROCESS, tid, kPreviewLoopNice) != 0) {
            spdlog::debug("[LiveSweepPreview] could not renice the stream thread");
        }
#endif
    }
    const int64_t cpu_at_start = helix::thread_cpu_ns();
    if (loop_cpu_first_ns_ < 0) {
        loop_cpu_first_ns_ = cpu_at_start;
    }

    accumulator_->note_console_frequency(hint_hz_.load());
    auto fresh =
        accumulator_->add_samples(batch.samples, batch.contiguous(), source_->sample_rate_hz());
    const double stream_s = accumulator_->stream_time_s();
    stream_time_s_.store(stream_s);
    loop_cpu_ns_.store(helix::thread_cpu_ns() - loop_cpu_first_ns_);

    if (!fresh.empty()) {
        stream_token_->defer(
            "LiveSweepPreview::points",
            [this, fresh = std::move(fresh)]() mutable { deliver(std::move(fresh)); });
    }

    if (!no_lock_logged_ && !accumulator_->locked() && std::isnan(accumulator_->drive_start_s()) &&
        stream_s >= kPreviewNoLockLogS) {
        no_lock_logged_ = true;
        spdlog::debug("[LiveSweepPreview] no lock on the drive after {:.0f} s of stream", stream_s);
    }

    bool done = accumulator_->complete();
    // Points are placed only once the drive has locked, so when the motion ends
    // without one there is no last point to wait for.
    const bool never_locked = std::isnan(accumulator_->drive_start_s());
    if (finishing_.load() || (console_ended_.load() && never_locked)) {
        if (finish_deadline_s_ <= 0.0) {
            finish_deadline_s_ = stream_s + kPreviewFinishGraceS;
        } else if (stream_s >= finish_deadline_s_) {
            done = true;
        }
    }
    if (done) {
        // The loop thread cannot join itself: the stop runs on the main thread.
        stop_requested_ = true;
        stream_token_->defer("LiveSweepPreview::auto_stop", [this]() {
            stop_source();
            state_ = State::DONE;
        });
    }
}

void LiveSweepPreview::on_source_error(const std::string& message) {
    if (stop_requested_) {
        return;
    }
    stop_requested_ = true;
    stream_token_->defer("LiveSweepPreview::source_error", [this, message]() {
        spdlog::info("[LiveSweepPreview] {}: stream ended: {}",
                     target_ ? target_->accel_chip : std::string(), message);
        stop_source();
        state_ = State::DONE;
    });
}

void LiveSweepPreview::deliver(std::vector<SweepPoint> fresh) {
    points_.insert(points_.end(), fresh.begin(), fresh.end());
    // A copy: the callback may call stop() or arm(), which replace on_points_.
    auto callback = on_points_;
    if (callback) {
        callback(points_);
    }
}

void LiveSweepPreview::finish() {
    if (state_ == State::STREAMING) {
        finishing_.store(true);
    } else if (state_ == State::ARMED) {
        state_ = State::DONE;
    }
}

void LiveSweepPreview::stop() {
    stop_source();
    state_ = State::IDLE;
    target_.reset();
    on_points_ = nullptr;
}

void LiveSweepPreview::stop_source() {
    lifetime_.invalidate();
    if (source_) {
        source_->stop();
        const double cpu_ms = static_cast<double>(loop_cpu_ns_.load()) / 1e6;
        spdlog::info("[LiveSweepPreview] {}: {} points, loop thread {:.0f} ms CPU over {:.0f} s of "
                     "stream",
                     target_ ? target_->accel_chip : std::string(), points_.size(), cpu_ms,
                     stream_time_s_.load());
        source_.reset();
    }
    accumulator_.reset();
    stream_token_.reset();
}

LiveSweepPreview::State LiveSweepPreview::state() const {
    return state_;
}

const std::vector<SweepPoint>& LiveSweepPreview::points() const {
    return points_;
}

} // namespace helix::calibration
