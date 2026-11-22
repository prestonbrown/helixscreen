// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The pure half of the in-app timelapse player: what ffmpeg is asked for, how
// its raw output is cut into frames, when each frame is due, and which way a
// tap on a video opens it. Nothing here touches LVGL or spawns a process, so
// every rule is testable without a display or an ffmpeg binary.

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
    std::string phone_url;         ///< phone_video_url()'s answer; empty when none
};

/// The one rule both entry points (the library's card tap and History) follow.
[[nodiscard]] VideoOpenAction choose_video_open_action(const VideoOpenInputs& in);

/// What each video card's badge shows for @p action: 0 none, 1 play here, 2 phone.
[[nodiscard]] int tap_badge_for(VideoOpenAction action);

/// Whether a board with @p ram_mb of memory and @p cpu_cores cores decodes
/// video beside Klipper: the boards PlatformTier calls EMBEDDED (under 512 MB,
/// or one core) hand the video to a phone instead.
[[nodiscard]] bool inapp_player_supported(size_t ram_mb, int cpu_cores);

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

/// "m:ss", or "h:mm:ss" from an hour up. Negative times read as 0:00.
[[nodiscard]] std::string format_clock(double seconds);

// --- Pacing ---

struct PaceDecision {
    double wait_s = 0.0;   ///< sleep this long before showing the frame
    bool reanchor = false; ///< too far behind: restart the clock at this frame
};

/// When frame @p index (counted from the clock's anchor) is due at @p fps,
/// given @p elapsed_s since the anchor. A frame late by more than
/// @p max_late_s restarts the clock rather than racing to catch up.
[[nodiscard]] PaceDecision pace_frame(int64_t index, double fps, double elapsed_s,
                                      double max_late_s = 0.25);

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
 * @brief The one-frame handoff between the decoder thread and the UI
 *
 * The decoder publishes each finished frame by swapping its buffer in; the UI
 * consumes the newest. A frame the UI never picked up before the next arrived
 * is dropped, never queued, so a slow UI shows fewer frames instead of falling
 * further behind. Thread-safe.
 */
class FrameExchange {
  public:
    struct Stats {
        uint64_t published = 0;
        uint64_t shown = 0;
        uint64_t dropped = 0;
    };

    /// Swap @p frame in as the newest; @p frame comes back holding a buffer to
    /// reuse. True when the UI had nothing pending: the caller then owes it one
    /// wake-up, and later publishes ride on that one.
    bool publish(std::vector<uint8_t>& frame, FrameSize size, int64_t index);

    /// Call @p fn(pixels, size, index) on the newest unseen frame, under the
    /// lock. False when nothing new arrived since the last consume.
    template <typename F> bool consume(F&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!fresh_) {
            return false;
        }
        fresh_ = false;
        ++stats_.shown;
        fn(static_cast<const uint8_t*>(ready_.data()), size_, index_);
        return true;
    }

    [[nodiscard]] Stats stats() const;
    /// Forget any pending frame (a seek makes it stale); stats are kept.
    void clear();

  private:
    mutable std::mutex mutex_;
    std::vector<uint8_t> ready_;
    FrameSize size_;
    int64_t index_ = 0;
    bool fresh_ = false;
    Stats stats_;
};

} // namespace helix::timelapse
