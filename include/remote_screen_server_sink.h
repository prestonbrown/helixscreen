// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "remote_screen_sink.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace helix {

/// How long after the last request a viewer counts as present: frames are captured, and a
/// page viewer holds the display awake.
constexpr int64_t kViewerIdleMs = 10000;
/// How long the first request after idle waits for a full repaint to land.
constexpr int64_t kFirstFrameWaitMs = 500;

/// Encode rate ceilings. Host work during toolhead motion is what trips "timer too
/// close" on the small boards, so printing caps lower, and lower still on them.
struct SnapshotRateCap {
    int idle_fps;
    int printing_fps;
};

SnapshotRateCap snapshot_rate_cap(bool constrained_host);
/// Little memory or a single core: the class of board where printing caps at 1 fps.
bool remote_screen_host_is_constrained();
std::chrono::milliseconds min_encode_interval(SnapshotRateCap cap, bool printing);

/// The one line on the frame served in place of the screen while a secret is on it.
constexpr const char* kScreenHiddenMessage = "Screen hidden while the pairing code is shown";

/// Draw the placeholder frame into @p rgb as RGB888, @p w x @p h: a plain gray fill with
/// kScreenHiddenMessage centered on it (left out when the frame is too small to hold it).
void draw_screen_placeholder_rgb888(int w, int h, std::vector<uint8_t>& rgb);

struct EncodedSnapshot {
    std::shared_ptr<const std::vector<uint8_t>> jpeg;
    uint64_t generation = 0;
    int w = 0;
    int h = 0;
};

/**
 * @brief The latest rendered screen, shared between the flush hook and the HTTP server.
 *
 * The flush hook (main thread) copies dirty rects into a shadow frame; the HTTP
 * thread turns the shadow into a JPEG on request. Nothing is allocated until a
 * viewer asks, and everything is freed once the viewer has been gone for
 * kViewerIdleMs. A static screen costs one encode in total: the JPEG is cached
 * with the frame generation it was made from.
 *
 * Thread-safe. The mutex is held for one rect's copy and for the RGB888
 * conversion, never across the JPEG encode.
 */
class ScreenFrameStore {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// An empty @p clock means `steady_clock::now`.
    explicit ScreenFrameStore(Clock clock = {});

    /// True within kViewerIdleMs of the last request of any kind. Cheap: called on every flush.
    bool wants_frames() const;

    /// The HelixScreen viewer page polled a snapshot: a person is looking at the screen.
    /// Webcam integrations (Obico, Home Assistant, timelapse, a view-only stream) fetch the
    /// same routes unmarked and only ever reach note_request().
    void note_page_view();
    /// True within kViewerIdleMs of the last note_page_view(). Lock-free, safe from any thread.
    bool page_viewer_present() const;

    /// Copy one dirty rect in, (re)allocating the shadow at the frame's display size
    /// and pixel format. The generation advances on the refresh's last rect, so a
    /// waiter never sees a screen painted only partway.
    void on_frame(const RemoteScreenFrame& f);

    enum class Need { Ready, FullRedraw };
    /// Mark a viewer as present. FullRedraw means the shadow cannot be trusted to
    /// hold the whole screen, and the caller should invalidate the display: always
    /// while nothing has been captured, and once after a resize.
    Need note_request();

    /// The display has just been invalidated whole, on the UI thread. The next refresh
    /// to complete covers every pixel and advances full_generation().
    void note_full_invalidate();
    /// Completed refreshes that followed a note_full_invalidate(). A refresh already
    /// under way when the repaint was asked for does not count: it covers only part
    /// of the screen.
    uint64_t full_generation() const;
    /// Wait until full_generation() passes @p full_gen. False on timeout or abort_waits().
    bool wait_for_full_refresh_after(uint64_t full_gen, std::chrono::milliseconds timeout);
    /// Wake every waiter now; they return false.
    void abort_waits();

    uint64_t generation() const;

    /// The captured screen's size, {0, 0} when nothing is captured.
    std::pair<int, int> frame_size() const;

    /// The current screen as a JPEG, or nullopt when nothing has been captured. Reuses
    /// the cached JPEG when the generation is unchanged or the last encode is newer
    /// than min_encode_interval(cap, printing).
    ///
    /// The placeholder frame (draw_screen_placeholder_rgb888) stands in for the screen
    /// until it is known clean: after a frame drawn with a secret Shown or Unknown, the
    /// screen is clean again only once a full repaint (note_full_invalidate) drawn
    /// with it Absent has completed. FullRedraw is asked for to get one.
    std::optional<EncodedSnapshot> snapshot(bool printing, SnapshotRateCap cap);

    /// The shadow holds no pixels drawn while a secret was on the display.
    bool screen_clean() const;

    /// Free every buffer if no viewer is present. True if it freed anything.
    bool release_if_idle();
    /// Free every buffer unconditionally.
    void release();

    size_t allocated_bytes() const;

  private:
    std::chrono::steady_clock::time_point now() const;
    /// @p last (steady_clock ticks, INT64_MIN = never) is within kViewerIdleMs of now().
    bool within_viewer_idle(int64_t last) const;
    void release_locked();

    Clock clock_;

    /// steady_clock ticks of the last request; min() = never.
    std::atomic<int64_t> last_request_{INT64_MIN};
    /// steady_clock ticks of the last viewer page poll; min() = never.
    std::atomic<int64_t> last_page_view_{INT64_MIN};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t abort_epoch_ = 0;

    std::vector<uint8_t> shadow_;
    int shadow_w_ = 0;
    int shadow_h_ = 0;
    int shadow_bpp_ = 0; ///< bytes per pixel: 2 or 4
    bool has_frame_ = false;
    bool full_redraw_needed_ = true;
    uint64_t generation_ = 0;
    bool awaiting_full_ = false;
    /// A rect of the refresh in progress was copied.
    bool refresh_copied_ = false;
    uint64_t full_generation_ = 0;

    /// The shadow holds no pixels drawn while a secret was on the display. An empty
    /// shadow is clean.
    bool clean_ = true;
    /// Frames are Absent again; clean_ turns true once full_generation_ passes
    /// clean_after_full_.
    bool clean_pending_ = false;
    uint64_t clean_after_full_ = 0;

    std::vector<uint8_t> rgb_;
    /// The placeholder frame, encoded once per size.
    std::shared_ptr<const std::vector<uint8_t>> placeholder_jpeg_;
    int placeholder_w_ = 0;
    int placeholder_h_ = 0;
    std::shared_ptr<const std::vector<uint8_t>> cached_jpeg_;
    uint64_t cached_generation_ = 0;
    int cached_w_ = 0;
    int cached_h_ = 0;
    std::chrono::steady_clock::time_point last_encode_{};

    /// One-shot diagnostics: the first frames' geometry, and the first rejected rect.
    static constexpr int kDiagFrames = 12;
    int diag_frames_ = 0;
    bool reject_warned_ = false;

    /// Serializes encodes, which read rgb_ outside mutex_.
    std::mutex encode_mutex_;
};

/// The RemoteScreenSink that feeds a ScreenFrameStore.
class ScreenServerSink : public RemoteScreenSink {
  public:
    explicit ScreenServerSink(std::shared_ptr<ScreenFrameStore> store);

    bool start() override;
    void stop() override;
    bool wants_frames() const override;
    void on_frame(const RemoteScreenFrame& frame) override;
    const char* name() const override;

  private:
    std::shared_ptr<ScreenFrameStore> store_;
};

} // namespace helix
