// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_timelapse_player.h"

#if HELIX_HAS_TIMELAPSE_VIEWER

#include "ui_callback_helpers.h"
#include "ui_nav.h"
#include "ui_panel_common.h"
#include "ui_utils.h"

#include "core/static_panel_registry.h"
#include "lv_draw_buf_guard.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace {

// How long the bars stay up after the last touch while the video plays.
constexpr uint32_t kControlsHideMs = 3000;

lv_color_format_t lv_format_for(helix::timelapse::RawPixelFormat format) {
    switch (format) {
    case helix::timelapse::RawPixelFormat::Rgb565:
        return LV_COLOR_FORMAT_RGB565;
    case helix::timelapse::RawPixelFormat::Rgb888:
        return LV_COLOR_FORMAT_RGB888;
    case helix::timelapse::RawPixelFormat::Xrgb8888:
        break;
    }
    return LV_COLOR_FORMAT_XRGB8888;
}

} // namespace

namespace helix::ui {

TimelapsePlayerOverlay& get_global_timelapse_player() {
    return helix::lazy_global<TimelapsePlayerOverlay>("TimelapsePlayerOverlay");
}

void open_timelapse_player(const std::string& path, const std::string& title) {
    get_global_timelapse_player().play(path, title);
}

TimelapsePlayerOverlay::TimelapsePlayerOverlay() = default;

TimelapsePlayerOverlay::~TimelapsePlayerOverlay() {
    cancel_present_timer();
    stop_decoder();
    free_draw_buf();
    subjects_.deinit_all();
}

void TimelapsePlayerOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_INT(state_subject_, kLoading, "timelapse_player_state", subjects_);
        UI_MANAGED_SUBJECT_INT(controls_subject_, 1, "timelapse_player_controls", subjects_);
        UI_MANAGED_SUBJECT_INT(position_subject_, 0, "timelapse_player_position", subjects_);
        UI_MANAGED_SUBJECT_STRING(elapsed_subject_, elapsed_buf_, "0:00",
                                  "timelapse_player_elapsed", subjects_);
        UI_MANAGED_SUBJECT_STRING(total_subject_, total_buf_, "0:00", "timelapse_player_total",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(title_subject_, title_buf_, "", "timelapse_player_title",
                                  subjects_);
    });
}

void TimelapsePlayerOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_tl_player_tap", [](lv_event_t*) { get_global_timelapse_player().toggle_controls(); }},
        {"on_tl_player_play_pause",
         [](lv_event_t*) { get_global_timelapse_player().play_pause(); }},
        {"on_tl_player_replay", [](lv_event_t*) { get_global_timelapse_player().replay(); }},
        {"on_tl_player_scrub_pressed",
         [](lv_event_t*) { get_global_timelapse_player().scrub_pressed(); }},
        {"on_tl_player_scrub_released",
         [](lv_event_t*) { get_global_timelapse_player().scrub_released(); }},
    });
}

lv_obj_t* TimelapsePlayerOverlay::create(lv_obj_t* parent) {
    // Built straight from XML: the view has no overlay_panel header to set up.
    parent_screen_ = parent;
    cleanup_called_ = false;
    overlay_root_ = helix::ui::create_xml_hidden(parent, xml_component());
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }
    // Full screen, over the nav dock: frames are fitted to the whole display.
    helix::nav::set_overlay_width_unmanaged(overlay_root_);
    video_frame_ = helix::ui::find_required(overlay_root_, "video_frame", get_name());
    return overlay_root_;
}

void TimelapsePlayerOverlay::play(const std::string& path, const std::string& title) {
    path_ = path;
    title_ = title;
    show(lv_screen_active());
    if (overlay_root_) {
        // Above any modal the library left open, like the other full-screen views.
        helix::ui::bring_to_front(overlay_root_);
    }
}

void TimelapsePlayerOverlay::on_activate() {
    OverlayBase::on_activate();
    start_decoder();
}

void TimelapsePlayerOverlay::on_deactivating(DeactivateReason) {
    stop_decoder();
}

void TimelapsePlayerOverlay::cleanup() {
    cancel_present_timer();
    stop_decoder();
    if (video_frame_) {
        lv_image_set_src(video_frame_, nullptr);
    }
    free_draw_buf();
    OverlayBase::cleanup();
}

helix::timelapse::FrameExchange::Stats TimelapsePlayerOverlay::frame_stats() {
    return decoder_ ? decoder_->exchange().stats() : helix::timelapse::FrameExchange::Stats{};
}

// ============================================================================
// DECODER
// ============================================================================

void TimelapsePlayerOverlay::start_decoder() {
    stop_decoder();
    if (path_.empty()) {
        set_state(kFailed);
        return;
    }
    lv_display_t* disp = lv_display_get_default();
    if (!disp) {
        set_state(kFailed);
        return;
    }
    const int bpp = static_cast<int>(lv_color_format_get_bpp(lv_display_get_color_format(disp)));
    format_ = helix::timelapse::raw_format_for_display_bpp(bpp);

    lv_subject_set_string(&title_subject_, title_.c_str());
    lv_subject_set_string(&elapsed_subject_, "0:00");
    lv_subject_set_string(&total_subject_, "0:00");
    lv_subject_set_int(&position_subject_, 0);
    shown_second_ = -1;
    scrubbing_ = false;
    set_state(kLoading);
    set_controls(true);

    helix::timelapse::PlayerDecoder::Config config;
    config.path = path_;
    config.box = {static_cast<int>(lv_display_get_horizontal_resolution(disp)),
                  static_cast<int>(lv_display_get_vertical_resolution(disp))};
    config.format = format_;

    helix::timelapse::PlayerDecoder::Events events;
    auto tok = decoder_lifetime_.token();
    events.on_info = [this, tok](const helix::timelapse::VideoInfo& info,
                                 helix::timelapse::FrameSize) {
        tok.defer("TimelapsePlayer::info", [this, info]() { on_info(info); });
    };
    events.on_end = [this, tok](bool failed) {
        tok.defer("TimelapsePlayer::end", [this, failed]() { on_end(failed); });
    };

    decoder_ = std::make_unique<helix::timelapse::PlayerDecoder>();
    spdlog::info("[{}] Playing {} ({}x{} {})", get_name(), path_, config.box.width,
                 config.box.height, helix::timelapse::ffmpeg_pix_fmt_name(format_));
    decoder_->start(std::move(config), std::move(events));
    start_present_timer();
}

void TimelapsePlayerOverlay::start_present_timer() {
    cancel_present_timer();
    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            static_cast<TimelapsePlayerOverlay*>(lv_timer_get_user_data(t))->present_frame();
        },
        helix::timelapse::present_poll_ms(fps_), this);
    present_timer_.reset(timer);
}

void TimelapsePlayerOverlay::cancel_present_timer() {
    // lv_timer_cancel_safe() underneath: safe from the timer's own callback and
    // after lv_deinit().
    present_timer_.reset();
}

void TimelapsePlayerOverlay::stop_decoder() {
    hide_timer_.reset();
    cancel_present_timer();
    // Queued frames and events become no-ops before the decoder goes away.
    decoder_lifetime_.invalidate();
    if (!decoder_) {
        return;
    }
    const auto stats = decoder_->exchange().stats();
    decoder_->stop();
    decoder_.reset();
    spdlog::info("[{}] Stopped: {} frames decoded, {} shown, {} dropped ({} the UI was late for, "
                 "{} before due)",
                 get_name(), stats.published, stats.shown, stats.dropped, stats.dropped_late,
                 stats.dropped_early);
}

void TimelapsePlayerOverlay::on_info(const helix::timelapse::VideoInfo& info) {
    fps_ = helix::timelapse::effective_fps(info.fps);
    duration_s_ = info.duration_s;
    lv_subject_set_string(&total_subject_, helix::timelapse::format_clock(duration_s_).c_str());
    set_state(kPlaying);
    arm_hide_timer();
    // Look for frames at this video's own rate.
    start_present_timer();
}

void TimelapsePlayerOverlay::on_end(bool failed) {
    if (failed) {
        spdlog::warn("[{}] Could not decode {}", get_name(), path_);
        cancel_present_timer();
        set_state(kFailed);
        return;
    }
    // The last frames may still be queued; the timer shows them, then stops.
    set_state(kEnded);
}

void TimelapsePlayerOverlay::ensure_draw_buf(helix::timelapse::FrameSize size) {
    const lv_color_format_t cf = lv_format_for(format_);
    if (draw_buf_ && static_cast<int>(draw_buf_->header.w) == size.width &&
        static_cast<int>(draw_buf_->header.h) == size.height && draw_buf_->header.cf == cf) {
        return;
    }
    if (video_frame_) {
        lv_image_set_src(video_frame_, nullptr);
    }
    free_draw_buf();
    draw_buf_ = lv_draw_buf_create(static_cast<uint32_t>(size.width),
                                   static_cast<uint32_t>(size.height), cf, LV_STRIDE_AUTO);
    if (draw_buf_ && video_frame_) {
        lv_image_set_src(video_frame_, draw_buf_);
    }
}

void TimelapsePlayerOverlay::free_draw_buf() {
    helix::safe_draw_buf_destroy(draw_buf_, "tl_player");
}

void TimelapsePlayerOverlay::present_frame() {
    if (!decoder_ || !video_frame_) {
        return;
    }
    int64_t index = -1;
    decoder_->exchange().consume_due(
        helix::timelapse::monotonic_seconds(),
        [&](const uint8_t* pixels, helix::timelapse::FrameSize size, int64_t frame_index) {
            ensure_draw_buf(size);
            if (!draw_buf_) {
                return;
            }
            // A draw unit may still be blending the previous frame from this buffer.
            lv_draw_wait_for_finish();
            const size_t row = static_cast<size_t>(size.width) *
                               static_cast<size_t>(helix::timelapse::bytes_per_pixel(format_));
            const uint32_t stride = draw_buf_->header.stride;
            uint8_t* dst = draw_buf_->data;
            if (stride == row) {
                std::memcpy(dst, pixels, row * static_cast<size_t>(size.height));
            } else {
                for (int y = 0; y < size.height; ++y) {
                    std::memcpy(dst + static_cast<size_t>(y) * stride, pixels + y * row, row);
                }
            }
            index = frame_index;
        });
    if (index < 0) {
        if (state_ == kEnded && decoder_->exchange().pending() == 0) {
            cancel_present_timer();
            if (duration_s_ > 0.0) {
                show_position(duration_s_);
            }
        }
        return;
    }
    lv_obj_invalidate(video_frame_);
    if (state_ == kLoading) {
        set_state(kPlaying);
    }
    show_position(static_cast<double>(index) / fps_);
}

void TimelapsePlayerOverlay::show_position(double seconds) {
    if (scrubbing_) {
        return;
    }
    lv_subject_set_int(&position_subject_, helix::timelapse::permille_for(seconds, duration_s_));
    const long second = static_cast<long>(seconds);
    if (second != shown_second_) {
        shown_second_ = second;
        lv_subject_set_string(&elapsed_subject_, helix::timelapse::format_clock(seconds).c_str());
    }
}

void TimelapsePlayerOverlay::set_state(State state) {
    state_ = state;
    lv_subject_set_int(&state_subject_, state);
    if (state != kPlaying) {
        // Paused, ended or failed: the controls are the only thing left to use.
        hide_timer_.reset();
        set_controls(true);
    }
}

// ============================================================================
// CONTROLS
// ============================================================================

void TimelapsePlayerOverlay::set_controls(bool shown) {
    lv_subject_set_int(&controls_subject_, shown ? 1 : 0);
}

void TimelapsePlayerOverlay::toggle_controls() {
    const bool shown = lv_subject_get_int(&controls_subject_) == 0;
    set_controls(shown);
    if (shown) {
        arm_hide_timer();
    } else {
        hide_timer_.reset();
    }
}

void TimelapsePlayerOverlay::arm_hide_timer() {
    hide_timer_.reset();
    if (state_ != kPlaying || lv_subject_get_int(&controls_subject_) == 0) {
        return;
    }
    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<TimelapsePlayerOverlay*>(lv_timer_get_user_data(t));
            if (self->state_ == kPlaying && !self->scrubbing_) {
                self->set_controls(false);
            }
            // Neuters this timer; LVGL deletes it after the callback returns.
            self->hide_timer_.reset();
        },
        kControlsHideMs, this);
    hide_timer_.reset(timer);
}

void TimelapsePlayerOverlay::play_pause() {
    if (!decoder_) {
        return;
    }
    if (state_ == kPlaying) {
        decoder_->pause();
        cancel_present_timer();
        set_state(kPaused);
    } else if (state_ == kPaused) {
        decoder_->resume();
        start_present_timer();
        set_state(kPlaying);
        arm_hide_timer();
    }
}

void TimelapsePlayerOverlay::replay() {
    if (!decoder_) {
        return;
    }
    decoder_->seek(0.0);
    start_present_timer();
    shown_second_ = -1;
    set_state(kPlaying);
    show_position(0.0);
    arm_hide_timer();
}

void TimelapsePlayerOverlay::scrub_pressed() {
    scrubbing_ = true;
    hide_timer_.reset();
}

void TimelapsePlayerOverlay::scrub_released() {
    scrubbing_ = false;
    if (!decoder_ || state_ == kLoading || state_ == kFailed) {
        return;
    }
    const double target =
        helix::timelapse::seek_seconds_for(lv_subject_get_int(&position_subject_), duration_s_);
    spdlog::debug("[{}] Seek to {:.2f}s", get_name(), target);
    decoder_->seek(target);
    start_present_timer();
    shown_second_ = -1;
    set_state(kPlaying);
    show_position(target);
    arm_hide_timer();
}

} // namespace helix::ui

#else // !HELIX_HAS_TIMELAPSE_VIEWER

#include <spdlog/spdlog.h>

void helix::ui::open_timelapse_player(const std::string& path, const std::string&) {
    spdlog::debug("[Timelapse Player] Compiled out (HELIX_HAS_TIMELAPSE_VIEWER=0); ignoring {}",
                  path);
}

#endif // HELIX_HAS_TIMELAPSE_VIEWER
