// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The pure half of the in-app timelapse player: what ffmpeg is asked for, how
// its raw output is cut into frames, when each frame is due, and which way a
// tap on a video opens it. Nothing here touches LVGL or spawns a process, so
// every rule is testable without a display or an ffmpeg binary.

#include "platform_capabilities.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace helix::timelapse {

// --- Routing ---

/// What a tap on a timelapse video does on this host.
enum class VideoOpenAction {
    PlayInApp,        ///< The video is on this disk: decode it here
    DownloadThenPlay, ///< Moonraker is on another host: fetch the file, then decode it here
    PhoneQr,          ///< No in-app player: show a QR code of the video's URL for a phone
    NoPlayerToast,    ///< No player and no URL a phone could reach: say so
};

struct VideoOpenInputs {
    bool ffmpeg_available = false; ///< ffmpeg is on this host's PATH
    bool board_can_decode = false; ///< inapp_player_supported() for this board
    bool moonraker_local = false;  ///< the video is on this host's disk
    bool print_active = false;     ///< a print holds the machine; its CPU is not ours to spend
    std::string phone_url;         ///< phone_video_url()'s answer; empty when none
};

/// The one rule both entry points (the library's card tap and History) follow.
[[nodiscard]] VideoOpenAction choose_video_open_action(const VideoOpenInputs& in);

/// What each video card's badge shows for @p action: 0 none, 1 play here, 2 phone.
[[nodiscard]] int tap_badge_for(VideoOpenAction action);

/// Whether a board of @p tier decodes video beside Klipper: every tier but
/// EMBEDDED, whose boards hand the video to a phone instead.
[[nodiscard]] bool inapp_player_supported(PlatformTier tier);

// --- What the video is ---

struct VideoInfo {
    int width = 0;
    int height = 0;
    double fps = 0.0;        ///< frames per second; 0 when unknown
    double duration_s = 0.0; ///< 0 when unknown
};

/// The ffprobe command that prints a video's size, frame rate and duration.
[[nodiscard]] std::vector<std::string> ffprobe_info_args(const std::string& path);

/// Parse ffprobe_info_args()'s output (key=value lines). nullopt without a size.
[[nodiscard]] std::optional<VideoInfo> parse_ffprobe_info(const std::string& output);

/// The ffmpeg command whose stderr header describes the video; used where
/// ffprobe is not installed. It exits non-zero (no output file): only its
/// header is read.
[[nodiscard]] std::vector<std::string> ffmpeg_header_args(const std::string& path);

/// Parse ffmpeg's "Duration:" and first "Video:" stream lines. nullopt without a size.
[[nodiscard]] std::optional<VideoInfo> parse_ffmpeg_header(const std::string& stderr_text);

/// The frame rate the player paces by: @p fps when it is sane, else 30.
[[nodiscard]] double effective_fps(double fps);

// --- What ffmpeg is asked to produce ---

/// Raw pixel layouts the player asks ffmpeg for, each matching an LVGL color format.
enum class RawPixelFormat {
    Rgb565,   ///< ffmpeg rgb565le, LV_COLOR_FORMAT_RGB565
    Rgb888,   ///< ffmpeg bgr24, LV_COLOR_FORMAT_RGB888 (B,G,R byte order)
    Xrgb8888, ///< ffmpeg bgra, LV_COLOR_FORMAT_XRGB8888 (B,G,R,X byte order)
};

/// The layout matching a display of @p bits_per_pixel: 16 gives RGB565, 24 RGB888,
/// anything else the 32-bit layout.
[[nodiscard]] RawPixelFormat raw_format_for_display_bpp(int bits_per_pixel);

[[nodiscard]] const char* ffmpeg_pix_fmt_name(RawPixelFormat format);
[[nodiscard]] int bytes_per_pixel(RawPixelFormat format);

struct FrameSize {
    int width = 0;
    int height = 0;
    [[nodiscard]] bool empty() const {
        return width <= 0 || height <= 0;
    }
};

/// The largest size with @p src's aspect that fits @p box, both sides even
/// (the scaler and the chroma planes want even sizes). Empty for degenerate input.
[[nodiscard]] FrameSize fit_frame(FrameSize src, FrameSize box);

/// Bytes in one packed frame of @p size in @p format.
[[nodiscard]] size_t frame_bytes(FrameSize size, RawPixelFormat format);

/// The ffmpeg command that decodes @p path from @p start_s seconds into packed
/// raw frames of @p out on stdout, audio dropped. The seek sits before -i so
/// ffmpeg jumps to the nearest keyframe instead of decoding from the start.
[[nodiscard]] std::vector<std::string> ffmpeg_decode_args(const std::string& path, double start_s,
                                                          FrameSize out, RawPixelFormat format);

/// The second a scrub bar at @p permille (0..1000) of a @p duration_s video
/// asks for, clamped inside the video.
[[nodiscard]] double seek_seconds_for(int permille, double duration_s);

/// Where a scrub bar shows @p position_s of a @p duration_s video, 0..1000.
[[nodiscard]] int permille_for(double position_s, double duration_s);

/// The time the elapsed label shows: where the finger holds the scrub bar
/// (@p scrub_permille of @p duration_s) while @p scrubbing, else the
/// presented frame's own time @p presented_s.
[[nodiscard]] double elapsed_label_seconds(bool scrubbing, int scrub_permille, double presented_s,
                                           double duration_s);

/// "m:ss", or "h:mm:ss" from an hour up. Negative times read as 0:00.
[[nodiscard]] std::string format_clock(double seconds);

// --- Pacing and presentation ---

struct PaceDecision {
    double wait_s = 0.0;   ///< sleep this long before handing the frame over
    bool reanchor = false; ///< too far behind: restart the clock at this frame
};

/// When to hand frame @p index (counted from the clock's anchor) to the UI at
/// @p fps, given @p elapsed_s since the anchor: @p lead_s before it is due, so
/// it is waiting when its moment comes. A frame late by more than @p max_late_s
/// restarts the clock rather than racing to catch up.
[[nodiscard]] PaceDecision pace_frame(int64_t index, double fps, double elapsed_s,
                                      double lead_s = 0.0, double max_late_s = 0.25);

/// Which of the @p count pending frames to show at @p now_s, given each one's
/// due time in @p due_s (oldest first): the newest already due, or -1 when
/// none is due yet. Every older due frame is skipped: the UI fell behind it.
[[nodiscard]] int pick_due_frame(const double* due_s, size_t count, double now_s);

/// How often the UI looks for a due frame at @p fps: a quarter of the frame
/// period, kept between 4 and 10 ms, so a frame is never more than a fraction
/// of a period late and two frames never fall between looks.
[[nodiscard]] uint32_t present_poll_ms(double fps);

/// Seconds on the monotonic clock both sides of the handoff share.
[[nodiscard]] double monotonic_seconds();

// --- Raw stream to frames ---

/**
 * @brief Cuts ffmpeg's packed rawvideo stream into whole frames
 *
 * The stream has no headers: every frame_bytes() run of bytes is one frame.
 * A reader fills tail() up to want() bytes and calls commit() with how many
 * arrived; commit() is true when that completed a frame, which then sits in
 * frame() until the next commit.
 */
class FrameAssembler {
  public:
    explicit FrameAssembler(size_t frame_bytes);

    [[nodiscard]] uint8_t* tail();
    [[nodiscard]] size_t want() const;
    /// @p n bytes were written at tail(); true when they completed a frame.
    bool commit(size_t n);
    /// The completed frame; the reader may swap its storage out.
    [[nodiscard]] std::vector<uint8_t>& frame() {
        return buf_;
    }
    /// Bytes of an incomplete frame, as at the end of a cut-off stream.
    [[nodiscard]] size_t partial() const {
        return filled_ == frame_bytes_ ? 0 : filled_;
    }
    /// Feed a whole chunk; @p on_frame runs for every frame it completes.
    template <typename F> void feed(const uint8_t* data, size_t n, F&& on_frame) {
        while (n > 0) {
            const size_t take = n < want() ? n : want();
            std::copy(data, data + take, tail());
            data += take;
            n -= take;
            if (commit(take)) {
                on_frame(buf_);
            }
        }
    }

  private:
    size_t frame_bytes_;
    std::vector<uint8_t> buf_;
    size_t filled_ = 0;
};

/**
 * @brief The handoff between the decoder thread and the UI's presentation timer
 *
 * The decoder publishes each frame a little before it is due, with its due
 * time; the UI's timer shows the newest frame whose time has come. Neither side
 * waits on the other: a full queue drops its oldest frame, and a UI that looks
 * late skips the frames it missed (pick_due_frame()). Thread-safe.
 */
class FrameExchange {
  public:
    /// Where every published frame went. Each lands in exactly one bucket:
    /// published == shown + skipped_late + replaced_late + replaced_early +
    /// discarded + queued.
    struct Stats {
        uint64_t published = 0;
        uint64_t shown = 0;
        uint64_t skipped_late = 0; ///< passed over for a newer due frame: the UI looked late
        uint64_t replaced_late =
            0; ///< pushed out of a full queue after it was due: the UI is behind
        uint64_t replaced_early =
            0; ///< pushed out of a full queue before it was due: the decoder ran ahead
        uint64_t discarded = 0; ///< thrown away unshown by clear() (a seek, or the player closing)
        uint64_t queued = 0;    ///< waiting when the stats were read

        /// Frames lost while playing: every bucket but discarded and queued.
        [[nodiscard]] uint64_t dropped() const {
            return skipped_late + replaced_late + replaced_early;
        }
    };

    /// Frames waiting at most; more than this and the oldest is dropped. The
    /// decoder queues one period ahead, so three slots hold the frame being
    /// shown, the next, and one more: only a UI more than a whole period late
    /// loses a frame, the same bar pick_due_frame() skips at.
    static constexpr size_t kSlots = 3;

    /// Queue @p frame, due at @p due_s, at @p now_s (both monotonic_seconds()),
    /// decoded by run @p generation; @p frame comes back holding a buffer to
    /// reuse. A frame from a run older than begin_generation()'s is discarded.
    void publish(std::vector<uint8_t>& frame, FrameSize size, int64_t index, double due_s,
                 double now_s, uint64_t generation = 0);

    /// A seek started run @p generation: discard the queued frames, and every
    /// later frame from an older run, so nothing from before the seek shows.
    void begin_generation(uint64_t generation);

    /// Call @p fn(pixels, size, index) on the frame pick_due_frame() chooses at
    /// @p now_s, under the lock. False when no frame is due.
    template <typename F> bool consume_due(double now_s, F&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        double due[kSlots];
        for (size_t i = 0; i < queue_.size(); ++i) {
            due[i] = queue_[i].due_s;
        }
        const int pick = pick_due_frame(due, queue_.size(), now_s);
        if (pick < 0) {
            return false;
        }
        for (int i = 0; i < pick; ++i) {
            ++stats_.skipped_late;
            recycle_front();
        }
        Slot& slot = queue_.front();
        ++stats_.shown;
        fn(static_cast<const uint8_t*>(slot.pixels.data()), slot.size, slot.index);
        recycle_front();
        return true;
    }

    /// Frames queued and not yet shown.
    [[nodiscard]] size_t pending() const;
    /// The queued frames' due times, oldest first, for diagnostics and tests.
    [[nodiscard]] std::vector<double> due_times() const;
    /// Push every queued frame's due time back by @p seconds (a pause ended).
    void shift_due(double seconds);
    [[nodiscard]] Stats stats() const;
    /// Forget the queued frames (a seek makes them stale, or the player
    /// closed); they count as discarded.
    void clear();

  private:
    struct Slot {
        std::vector<uint8_t> pixels;
        FrameSize size;
        int64_t index = 0;
        double due_s = 0.0;
    };
    void recycle_front();

    mutable std::mutex mutex_;
    std::vector<Slot> queue_;                ///< oldest first, at most kSlots
    std::vector<std::vector<uint8_t>> free_; ///< buffers handed back to the decoder
    uint64_t generation_ = 0;                ///< frames from older runs are discarded
    Stats stats_;
};

} // namespace helix::timelapse
