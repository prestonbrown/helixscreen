// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "helix_features.h"

#if HELIX_HAS_TIMELAPSE_VIEWER

#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "core/subject.h"
#include "core/timer_guard.h"
#include "lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"
#include "timelapse_player_decoder.h"

#include <memory>
#include <string>

namespace helix::ui {

/**
 * @brief Plays one timelapse video on the app's own screen
 *
 * A PlayerDecoder runs ffmpeg into raw frames of the display's own pixel
 * format, fitted to the screen, and queues each a little before it is due; a
 * timer on the main thread copies the newest due frame into the one draw
 * buffer the lv_image shows. Leaving the overlay, or destroying it at shutdown, stops the
 * decoder: its worker is joined and its ffmpeg child reaped.
 *
 * @threading The decoder's events arrive on its worker and reach this class
 *            only through decoder_lifetime_ tokens, deferred to the main thread.
 */
class TimelapsePlayerOverlay : public OverlayBase {
  public:
    TimelapsePlayerOverlay();
    ~TimelapsePlayerOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;

    [[nodiscard]] const char* get_name() const override {
        return "Timelapse Player";
    }
    [[nodiscard]] const char* xml_component() const override {
        return "timelapse_player_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    lv_obj_t* create(lv_obj_t* parent) override;
    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;
    void cleanup() override;

    /// Open the player on the video at @p path, titled @p title.
    void play(const std::string& path, const std::string& title);

    /// Frames shown and dropped so far, for logs and perf runs.
    [[nodiscard]] helix::timelapse::FrameExchange::Stats frame_stats();

  private:
    enum State { kLoading = 0, kPlaying = 1, kPaused = 2, kEnded = 3, kFailed = 4 };

    void start_decoder();
    void stop_decoder();
    /// The presentation timer: runs while frames can arrive (playing, or ended
    /// with frames still queued), stopped while paused. Cancelled from
    /// cleanup() and the destructor as well as from every state change.
    void start_present_timer();
    void cancel_present_timer();
    void present_frame();
    void ensure_draw_buf(helix::timelapse::FrameSize size);
    void free_draw_buf();
    void on_info(const helix::timelapse::VideoInfo& info);
    void on_end(bool failed);
    void set_state(State state);
    void show_position(double seconds);

    void toggle_controls();
    void set_controls(bool shown);
    void arm_hide_timer();
    void play_pause();
    void replay();
    void scrub_pressed();
    void scrub_moved();
    void scrub_released();

    void on_ui_destroyed() override {
        video_frame_ = nullptr;
    }

    std::string path_;
    std::string title_;
    helix::timelapse::RawPixelFormat format_ = helix::timelapse::RawPixelFormat::Xrgb8888;
    std::unique_ptr<helix::timelapse::PlayerDecoder> decoder_;
    helix::AsyncLifetimeGuard decoder_lifetime_;

    WidgetRef video_frame_;
    lv_draw_buf_t* draw_buf_ = nullptr;

    double fps_ = 30.0;
    double duration_s_ = 0.0;
    long shown_second_ = -1;
    bool scrubbing_ = false;
    State state_ = kLoading;
    LvglTimerGuard hide_timer_;
    LvglTimerGuard present_timer_;

    SubjectManager subjects_;
    helix::Subject state_subject_{};
    helix::Subject controls_subject_{};
    helix::Subject position_subject_{};
    helix::Subject elapsed_subject_{};
    char elapsed_buf_[16] = {};
    helix::Subject total_subject_{};
    char total_buf_[16] = {};
    helix::Subject title_subject_{};
    char title_buf_[128] = {};
};

TimelapsePlayerOverlay& get_global_timelapse_player();

} // namespace helix::ui

#endif // HELIX_HAS_TIMELAPSE_VIEWER

namespace helix::ui {

/// Play the video at @p path on this screen; a no-op on builds without the viewer.
void open_timelapse_player(const std::string& path, const std::string& title);

} // namespace helix::ui
