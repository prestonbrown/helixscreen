// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "src/ui/panel_widgets/clock_face_view.h"

#include "ui_utils.h"

#include "screensaver_analog_clock.h"
#include "screensaver_clock.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <chrono>

namespace helix {

namespace {

/// Slack past the minute, so the wake lands after the clock has turned rather than on it.
constexpr uint32_t kMinuteSlackMs = 20;

/// Milliseconds into the current wall-clock second.
uint32_t ms_into_second() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count() % 1000);
}

ui::ClockInput plain(ui::ClockInput in) {
    // A tile's face is a plain clock: no print on it.
    in.print = ui::ClockPrint{};
    return in;
}

} // namespace

ClockFaceView::~ClockFaceView() {
    hide();
}

ui::AnalogFace ClockFaceView::analog_face() const {
    return ui::AnalogFace{static_cast<ui::AnalogStyle>(static_cast<int>(face_) -
                                                       static_cast<int>(ClockFace::CHRONOGRAPH)),
                          clock_.reading()};
}

bool ClockFaceView::show(lv_obj_t* parent, ClockFace face, int32_t w, int32_t h,
                         const ClockFaceTempo& tempo, const ui::ClockInput& in, ui::Rgb backdrop,
                         bool run_timer) {
    hide();
    if (face == ClockFace::TEXT || parent == nullptr || w <= 0 || h <= 0) {
        return false;
    }
    if (!canvas_.create(parent, w, h, ui::kLvDrawCanvasFormat)) {
        return false;
    }
    canvas_ref_.reset(canvas_.obj());
    // DECLARATIVE_OK: a canvas the face paints pixel by pixel, sized to the tile at runtime.
    lv_obj_set_clickable(canvas_.obj(), false);
    lv_obj_set_event_bubble(canvas_.obj(), true);
    // Centered in the host's content box, outside its layout: a dial is narrower than a wide tile.
    lv_obj_set_ignore_layout(canvas_.obj(), true);
    lv_obj_center(canvas_.obj());
    face_ = face;
    tempo_ = tempo;
    backdrop_ = backdrop;
    // A canvas with alpha paints the surround in a key and then clears it to transparent, so
    // the card shows through, Glass's gradient included. The key is the card's color with its
    // lowest blue bit flipped: antialiased edges blend toward the card's own color, so no dark
    // fringe, and no face pixel is taken for the surround. RGB565 has no alpha and keeps the
    // card's color as an opaque surround.
    keyed_ = lv_color_format_has_alpha(canvas_.format());
    key_ = backdrop;
    key_.b ^= 1u;
    const ui::Rgb surround = keyed_ ? key_ : backdrop;

    const ui::ClockInput input = plain(in);
    ui::FrameTarget frame = canvas_.frame();
    std::vector<ui::DirtyRect> dirty;
    if (face == ClockFace::FLIP || face == ClockFace::NIXIE) {
        cards_.reset();
        cards_.step(0, input);
        markers_.reset();
        markers_.set_drop_unfit(true);
        if (face == ClockFace::FLIP) {
            flip_ = std::make_unique<ui::FlipClockSim>();
            flip_->set_drift(false);
            flip_->set_backdrop(surround);
            flip_->init(frame, tempo.level, cards_.face());
            markers_.draw_flip(canvas_, *flip_, cards_.face(), under_, dirty);
        } else {
            nixie_ = std::make_unique<ui::NixieClockSim>();
            nixie_->set_drift(false);
            nixie_->set_backdrop(surround);
            nixie_->init(frame, tempo.level, cards_.face());
            markers_.draw_nixie(canvas_, *nixie_, cards_.face(), under_, dirty);
        }
    } else {
        clock_.reset();
        clock_.step(0, input);
        analog_ = std::make_unique<ui::AnalogClockSim>();
        ui::AnalogClockOptions options;
        options.drift = false;
        options.seconds = tempo.seconds;
        options.side_column = false;
        options.backdrop = surround;
        // A text box no font can fill legibly is left out, frame and all.
        options.min_text_px = theme_manager_get_font_height(ui::clock_font_fitting(1));
        analog_->set_options(options);
        analog_->init(frame, tempo.level, analog_face());
        text_canvas_.reset(lv_canvas_create(parent));
        // DECLARATIVE_OK: an offscreen canvas over the sim's dial, which LVGL draws text into.
        lv_obj_set_hidden(text_canvas_.get(), true);
        write_analog_texts();
        analog_->repaint();
        analog_->compose(frame, dirty);
    }
    clear_surround({ui::DirtyRect::whole(w, h)});
    lv_obj_invalidate(canvas_.obj());

    last_tick_ = lv_tick_get();
    if (run_timer) {
        timer_.reset(lv_timer_create(timer_cb, tempo.period_ms, this));
        schedule(tempo_.seconds);
    }
    spdlog::debug("[ClockFaceView] {} at {}x{}, rung {}, seconds {}, {} KB", clock_face_id(face), w,
                  h, tempo.level, tempo.seconds, heap_bytes() / 1024);
    return true;
}

void ClockFaceView::write_analog_texts() {
    if (analog_) {
        ui::analog_write_dial_texts(text_canvas_.get(), text_buf_, *analog_, clock_.reading(),
                                    /*drop_unfit=*/true);
    }
}

bool ClockFaceView::step(uint32_t dt_ms, const ui::ClockInput& in) {
    if (!showing() || canvas_ref_.get() == nullptr) {
        return false;
    }
    const ui::ClockInput input = plain(in);
    ui::FrameTarget frame = canvas_.frame();
    dirty_.clear();
    bool moving = false;
    if (flip_ || nixie_) {
        cards_.step(dt_ms, input);
        if (flip_) {
            flip_->step(dt_ms, frame, cards_.face(), dirty_);
            under_.assign(dirty_.begin(), dirty_.end());
            markers_.draw_flip(canvas_, *flip_, cards_.face(), under_, dirty_);
            moving = flip_->flipping();
        } else {
            nixie_->step(dt_ms, frame, cards_.face(), dirty_);
            under_.assign(dirty_.begin(), dirty_.end());
            markers_.draw_nixie(canvas_, *nixie_, cards_.face(), under_, dirty_);
            moving = nixie_->fading() || nixie_->cycling();
        }
    } else if (analog_) {
        clock_.step(dt_ms, input);
        if (analog_->prepare(dt_ms, frame, analog_face())) {
            write_analog_texts();
        }
        analog_->compose(frame, dirty_);
        moving = tempo_.seconds;
    }
    clear_surround(dirty_);
    canvas_.invalidate(dirty_);
    return moving;
}

void ClockFaceView::clear_surround(const std::vector<ui::DirtyRect>& areas) {
    if (!keyed_ || canvas_.data() == nullptr) {
        return;
    }
    // ARGB8888 is B, G, R, A in memory; the surround's color stays for the next blend over it.
    for (const ui::DirtyRect& area : areas) {
        const ui::DirtyRect in = area.clipped(canvas_.width(), canvas_.height());
        if (in.empty()) {
            continue;
        }
        for (int32_t y = in.y1; y <= in.y2; ++y) {
            uint8_t* px = canvas_.data() + static_cast<size_t>(y) * canvas_.stride() +
                          static_cast<size_t>(in.x1) * 4;
            for (int32_t x = in.x1; x <= in.x2; ++x, px += 4) {
                if (px[0] == key_.b && px[1] == key_.g && px[2] == key_.r) {
                    px[3] = 0;
                }
            }
        }
    }
}

void ClockFaceView::schedule(bool moving) {
    lv_timer_t* t = timer_.get();
    if (t == nullptr) {
        return;
    }
    uint32_t period = tempo_.period_ms;
    if (!moving) {
        const ui::ClockInput now = ui::saver_clock_now();
        period = clock_ms_to_next_minute(now.seconds_of_day, ms_into_second()) + kMinuteSlackMs;
    }
    lv_timer_set_period(t, period);
    lv_timer_reset(t);
}

void ClockFaceView::timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<ClockFaceView*>(lv_timer_get_user_data(timer));
    if (self == nullptr || !self->showing()) {
        return;
    }
    const uint32_t dt = lv_tick_elaps(self->last_tick_);
    self->last_tick_ = lv_tick_get();
    const bool moving = self->step(dt, ui::saver_clock_now());
    self->schedule(moving);
}

void ClockFaceView::pause() {
    timer_.reset();
}

void ClockFaceView::resume(const ui::ClockInput& in) {
    if (!showing() || timer_) {
        return;
    }
    const uint32_t dt = lv_tick_elaps(last_tick_);
    last_tick_ = lv_tick_get();
    const bool moving = step(dt, in);
    timer_.reset(lv_timer_create(timer_cb, tempo_.period_ms, this));
    schedule(moving || tempo_.seconds);
}

void ClockFaceView::hide() {
    timer_.reset();
    const bool lvgl = lv_is_initialized();
    // A canvas already deleted with its parent is not touched.
    if (canvas_ref_.get() == nullptr || !lvgl) {
        canvas_.forget_obj();
    }
    // Hidden before its buffer is freed, then deleted: nothing draws from freed pixels. The text
    // canvas over the dial is hidden too before the dial goes.
    canvas_.release();
    if (lvgl) {
        if (text_canvas_.get() != nullptr) {
            lv_obj_set_hidden(text_canvas_.get(), true);
            helix::ui::safe_delete_deferred(text_canvas_);
        }
        if (canvas_ref_.get() != nullptr) {
            helix::ui::safe_delete_deferred(canvas_ref_);
        }
    }
    text_canvas_.reset();
    canvas_ref_.reset();
    analog_.reset();
    flip_.reset();
    nixie_.reset();
    markers_.reset();
    dirty_.clear();
    under_.clear();
    face_ = ClockFace::TEXT;
}

void ClockFaceView::release_dying() {
    canvas_.forget_obj();
    canvas_ref_.reset();
    text_canvas_.reset();
    hide();
}

size_t ClockFaceView::heap_bytes() const {
    size_t bytes = canvas_.buffer_size();
    if (analog_) {
        bytes += analog_->heap_bytes();
    }
    if (flip_) {
        bytes += flip_->heap_bytes();
    }
    if (nixie_) {
        bytes += nixie_->heap_bytes();
    }
    return bytes;
}

} // namespace helix

#endif // HELIX_ENABLE_SCREENSAVER
