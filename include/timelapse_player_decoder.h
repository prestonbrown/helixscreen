// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "helix_features.h"

#if HELIX_HAS_TIMELAPSE_VIEWER

#include "timelapse_playback.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace helix::timelapse {

/// The video's size, rate and duration: ffprobe when installed, else ffmpeg's
/// own header. BLOCKING; ends early once @p cancel turns true.
std::optional<VideoInfo> probe_video(const std::string& path, const std::atomic<bool>& cancel);

/**
 * @brief Decodes one video through an ffmpeg child into paced raw frames
 *
 * A worker thread (joined by stop() and the destructor) probes the video,
 * runs ffmpeg with its stdout on a pipe, cuts the stream into frames and
 * publishes each into exchange() one frame period before it is due, stamped
 * with its due time; the UI's own timer shows it then. The worker paces
 * against the monotonic clock and never waits on the UI. Pause stops reading, so
 * ffmpeg blocks on the full pipe and costs nothing; a seek replaces the child
 * with one started at the new time. When the stream ends the worker waits for
 * a seek (replay) or stop. Every child is reaped before the next starts and
 * before stop() returns.
 *
 * Every Events callback runs on the worker thread: marshal to the UI with
 * ui_queue_update() or a LifetimeToken's defer().
 */
class PlayerDecoder {
  public:
    struct Events {
        /// The probe answered; frames will be @p out in size.
        std::function<void(const VideoInfo& info, FrameSize out)> on_info;
        /// Run @p generation's stream ended. @p failed: no video could be
        /// decoded at all. A report whose generation is older than
        /// generation() is from before a seek and is stale.
        std::function<void(bool failed, uint64_t generation)> on_end;
    };

    struct Config {
        std::string path;
        FrameSize box; ///< the area frames are fitted into
        RawPixelFormat format = RawPixelFormat::Xrgb8888;
        /// Overrides probe_video(); tests use it to avoid ffprobe.
        std::function<std::optional<VideoInfo>(const std::atomic<bool>& cancel)> probe;
        /// Overrides ffmpeg_decode_args(); tests use it to stand in a fake stream.
        std::function<std::vector<std::string>(double start_s, FrameSize out)> decode_argv;
    };

    PlayerDecoder() = default;
    ~PlayerDecoder();
    PlayerDecoder(const PlayerDecoder&) = delete;
    PlayerDecoder& operator=(const PlayerDecoder&) = delete;

    /// Start the worker. False when it is already running or could not start.
    bool start(Config config, Events events);
    void pause();
    void resume();
    /// Restart decoding at @p seconds; from an ended stream this replays.
    void seek(double seconds);
    /// Stop the worker, kill and reap the child, and join. Idempotent.
    void stop();

    [[nodiscard]] FrameExchange& exchange() {
        return exchange_;
    }
    /// The current decode run: 0 from start(), one more for every seek.
    [[nodiscard]] uint64_t generation() const {
        return generation_.load();
    }
    /// The running ffmpeg child, or -1 between children.
    [[nodiscard]] pid_t child_pid() const {
        return child_pid_.load();
    }

  private:
    void run();
    /// Decode one child's stream from @p start_s; false when it ended on its own.
    bool decode_from(double start_s, double fps, FrameSize out, uint64_t generation,
                     bool& got_frame);
    /// Wait while paused; true when the run should stop reading (stop or seek).
    bool wait_while_paused(std::unique_lock<std::mutex>& lock);

    Config config_;
    Events events_;
    FrameExchange exchange_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    bool paused_ = false;
    double paused_at_s_ = 0.0;     ///< monotonic_seconds() when the pause began
    double pending_shift_s_ = 0.0; ///< paused time the reading loop has not taken up
    std::optional<double> seek_to_;

    std::atomic<uint64_t> generation_{0};
    std::atomic<pid_t> child_pid_{-1};
    std::thread worker_;
};

} // namespace helix::timelapse

#endif // HELIX_HAS_TIMELAPSE_VIEWER
