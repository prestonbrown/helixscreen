// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "belt_tension_types.h"
#include "json_fwd.h"
#include "resonance_console.h"
#include "sweep_response_estimator.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @file live_sweep_preview.h
 * @brief The response curve of a running sweep, drawn as it is measured
 *
 * Cosmetic only. Klipper's CSV replaces the preview when it lands, and every
 * verdict, peak and recommendation still comes from the CSV.
 *
 * @par Threading
 * LiveSweepPreview is a main-thread object. Its source delivers batches on the
 * source's own loop thread, where the accumulator runs and nothing else;
 * finished points reach the main thread through a LifetimeToken::defer, and
 * stop() expires the token before it joins the loop, so a point already queued
 * is dropped rather than delivered to a torn-down owner.
 *
 * @par The preview never keeps the chip running
 * A dump client is a consumer of the chip like the resonance tester's own, so
 * the stream is stopped when the last point is emitted, on finish(), stop(),
 * a source error, and every owner teardown.
 *
 * @par Header weight
 * The panel headers that hold a LiveSweepPreview are parsed by the ESP32 build,
 * so this header names no libhv type: AccelBatch is forward-declared and the
 * BeltStreamClient-backed source lives in live_sweep_preview.cpp.
 */

namespace helix {
class IMoonrakerAPI;
}

namespace helix::calibration {

struct AccelBatch;

/// Nice value the stream's loop thread takes on its first batch: klippy moves
/// the toolhead during the sweep, and a starved klippy fails the move.
inline constexpr int kPreviewLoopNice = 10;

/// Stream seconds finish() waits for the last point before it stops the stream.
inline constexpr double kPreviewFinishGraceS = 2.0;

/// How far below the plan's end the sweep's last console line can read.
/// Klipper prints a line each time the drive passes a whole Hz, so the last one
/// is floor(max_freq), or one below when the sweep's float steps stop just
/// short; the plan ends at ceil(max_freq).
inline constexpr float kPreviewLastLineSlackHz = 2.0f;

/// Stream seconds without a lock after which one debug line says so.
inline constexpr double kPreviewNoLockLogS = 10.0;

/// The motion a sweep drives the accelerometer with.
enum class SweepAxis {
    X,        ///< SHAPER_CALIBRATE AXIS=X
    Y,        ///< SHAPER_CALIBRATE AXIS=Y
    DIAGONAL, ///< TEST_RESONANCES AXIS=1,1 or 1,-1 (a belt path)
};

/**
 * @brief The [resonance_tester] chip that records this sweep
 *
 * X: accel_chip_x, else accel_chip. Y: accel_chip_y, else accel_chip.
 * DIAGONAL: accel_chip when neither per-axis chip is set; the per-axis chip
 * when both are set and equal; otherwise nullopt (two chips record one sweep,
 * so no single stream carries it). Values are trimmed; an empty result is nullopt.
 */
[[nodiscard]] std::optional<std::string> accel_chip_for_sweep(const ResonanceTesterConfig& cfg,
                                                              SweepAxis axis);

/// The ramp Klipper sweeps: the window floored and ceiled as
/// test_belt_resonance sends it, else the config's range; the config's rate.
/// nullopt when the result is not SweepPlan::valid().
[[nodiscard]] std::optional<SweepPlan> sweep_plan_for(const ResonanceTesterConfig& cfg,
                                                      std::optional<BeltSweepWindow> window);

/// What one query of the printer says about streaming its sweeps.
struct LiveSweepFacts {
    std::string socket_path;       ///< klippy's UDS, "" when Moonraker did not say
    bool socket_reachable = false; ///< BeltStreamClient::socket_reachable(socket_path)
    ResonanceTesterConfig cfg;
};

/// Everything a preview needs to attach to one sweep.
struct LiveSweepTarget {
    std::string socket_path;
    std::string accel_chip;
    SweepPlan plan;
};

/**
 * @brief The single gate for a live preview
 *
 * nullopt unless the socket path is non-empty and reachable, the sweep has one
 * chip (accel_chip_for_sweep) and a valid plan (sweep_plan_for). No platform
 * tier enters it: every board that can reach klippy's socket gets a preview.
 */
[[nodiscard]] std::optional<LiveSweepTarget>
live_sweep_target(const LiveSweepFacts& facts, SweepAxis axis,
                  std::optional<BeltSweepWindow> window = std::nullopt);

/// Points laid out for an index-placed chart axis.
struct PreviewSeries {
    std::vector<float> freqs;
    std::vector<float> amps; ///< NaN where no clean point falls
    float max_amp = 0.0f;    ///< largest finite amp, 0 when none
};

/**
 * @brief Lay the points out over [axis_lo_hz, axis_hi_hz]
 *
 * n = lround((hi - lo) / kStepHz) + 1 slots, slot i at lo + i (hi - lo) / (n - 1),
 * so the first and last slots sit on the axis ends the way the chart places
 * them. A slot takes the value (sweep_column_value) of the point nearest it
 * when that point is within kStepHz / 2; otherwise NaN. Empty when hi <= lo or
 * either end is not finite.
 */
[[nodiscard]] PreviewSeries layout_preview(const std::vector<SweepPoint>& points,
                                           SweepColumn column, float axis_lo_hz, float axis_hi_hz);

/// klippy_uds_address from a /server/config response, with or without
/// Moonraker's "result" wrapper; "" when absent or not a string.
[[nodiscard]] std::string parse_klippy_uds_address(const nlohmann::json& data);

/// Ask Moonraker for klippy's socket path. on_done runs on the API's callback
/// thread with "" on any error.
void query_klippy_uds_address(IMoonrakerAPI& api, std::function<void(std::string)> on_done);

/// The socket path, its reachability and [resonance_tester], in one call.
/// on_done runs once, on the API's callback thread; a failed leg leaves its
/// defaults ("" / false / ResonanceTesterConfig{}).
void query_live_sweep_facts(IMoonrakerAPI& api, std::function<void(LiveSweepFacts)> on_done);

/// Where batches come from. start/stop are main-thread calls; the callbacks
/// fire on the source's own thread. The shapes match BeltStreamClient's.
class IAccelBatchSource {
  public:
    using BatchCallback = std::function<void(const AccelBatch&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    virtual ~IAccelBatchSource() = default;
    virtual bool start(const std::string& socket_path, const std::string& accel_chip,
                       BatchCallback on_batch, ErrorCallback on_error) = 0;
    /// Idempotent; joins the source's thread.
    virtual void stop() = 0;
    [[nodiscard]] virtual float sample_rate_hz() const = 0;
};

/// The mock printer's synthetic stream (MoonrakerClientMock::make_accel_batch_source)
/// when RuntimeConfig::should_mock_moonraker(), else klippy's dump endpoint
/// through an unchanged BeltStreamClient.
[[nodiscard]] std::unique_ptr<IAccelBatchSource> make_accel_batch_source(const SweepPlan& plan);

class LiveSweepPreview {
  public:
    enum class State {
        IDLE,      ///< not armed
        ARMED,     ///< waiting for the sweep's first console line
        STREAMING, ///< the source is running
        DONE,      ///< the stream ended (last point, finish(), source error); points stay
    };

    /// Main thread. Every point delivered so far, in sweep order.
    using PointsCallback = std::function<void(const std::vector<SweepPoint>& points)>;
    using SourceFactory = std::function<std::unique_ptr<IAccelBatchSource>(const SweepPlan& plan)>;

    LiveSweepPreview();
    ~LiveSweepPreview();
    LiveSweepPreview(const LiveSweepPreview&) = delete;
    LiveSweepPreview& operator=(const LiveSweepPreview&) = delete;

    /// Replace make_accel_batch_source. Only in IDLE.
    void set_source_factory(SourceFactory factory);

    /// Arm for one sweep; nothing connects yet. Stops and replaces any earlier
    /// sweep and clears points().
    void arm(LiveSweepTarget target, PointsCallback on_points);

    /// The console's "Testing frequency" value. In ARMED it starts the stream,
    /// so the tester's own client is already running the chip; in STREAMING it
    /// updates the drive search hint. A value within kPreviewLastLineSlackHz of
    /// the plan's end means the motion is ending: a stream that never locked
    /// then finishes as finish() does, and a locked one runs on to its last
    /// point. Ignored in IDLE and DONE, and when hz is not finite and positive.
    void note_console_frequency(float hz);

    /// The sweep's motion is over: keep streaming until the last point or
    /// kPreviewFinishGraceS of further stream, then stop. Points stay.
    void finish();

    /// Back to IDLE; idempotent. Queued points are dropped; points() keeps what
    /// was delivered until the next arm().
    void stop();

    [[nodiscard]] State state() const;
    [[nodiscard]] const std::vector<SweepPoint>& points() const;

  private:
    void attach();
    /// Loop thread.
    void on_batch(const AccelBatch& batch);
    /// Loop thread.
    void on_source_error(const std::string& message);
    /// Main thread.
    void deliver(std::vector<SweepPoint> fresh);
    /// Main thread: expire the token, stop and drop the source and the
    /// accumulator, log the loop thread's CPU use. Keeps points_.
    void stop_source();

    State state_ = State::IDLE;
    SourceFactory factory_;
    std::optional<LiveSweepTarget> target_;
    PointsCallback on_points_;
    std::vector<SweepPoint> points_;

    /// Created by attach() before the source starts, touched only on the
    /// loop thread while streaming, destroyed after the source is stopped.
    std::unique_ptr<SweepResponseAccumulator> accumulator_;
    std::unique_ptr<IAccelBatchSource> source_;

    std::atomic<float> hint_hz_{0.0f};
    std::atomic<bool> finishing_{false};
    /// The console reported the sweep's last line.
    std::atomic<bool> console_ended_{false};
    /// Loop thread only.
    double finish_deadline_s_ = 0.0;
    bool stop_requested_ = false;
    bool no_lock_logged_ = false;
    bool niced_ = false;
    /// Loop thread CPU time at the first batch (loop thread only) and at the
    /// latest, for the stop log line: the loop thread's CPU cost of a sweep.
    int64_t loop_cpu_first_ns_ = -1;
    std::atomic<int64_t> loop_cpu_ns_{0};
    std::atomic<double> stream_time_s_{0.0};

    helix::AsyncLifetimeGuard lifetime_;
    /// Taken from lifetime_ on the main thread when the source starts; the
    /// loop thread defers through it, so stop() expiring it drops what it queued.
    std::optional<helix::LifetimeToken> stream_token_;
};

} // namespace helix::calibration
