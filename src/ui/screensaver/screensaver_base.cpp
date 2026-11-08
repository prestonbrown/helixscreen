// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_base.h"

#include "refresh_period_hold.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>

namespace helix::ui {

void SaverBase::start() {
    if (active_) {
        spdlog::debug("[Screensaver] Type {} already active, ignoring start()",
                      static_cast<int>(type()));
        return;
    }
    lv_display_t* disp = lv_display_get_default();
    if (!disp) {
        spdlog::warn("[Screensaver] No display available, cannot start type {}",
                     static_cast<int>(type()));
        return;
    }
    screen_w_ = lv_display_get_horizontal_resolution(disp);
    screen_h_ = lv_display_get_vertical_resolution(disp);

    overlay_.create();
    if (const std::optional<lv_color_format_t> cf = canvas_format()) {
        if (!canvas_.create(overlay_.obj(), screen_w_, screen_h_, *cf)) {
            overlay_.destroy();
            return;
        }
        // The opaque canvas covers the whole overlay.
        overlay_.make_transparent();
    }

    rng_.seed(fixed_seed_.value_or(static_cast<uint32_t>(time(nullptr))));
    level_ = start_level_;
    print_.reset();
    step_print_reading(0);
    if (!on_start()) {
        spdlog::warn("[Screensaver] Type {} refused to start", static_cast<int>(type()));
        canvas_.release();
        overlay_.destroy();
        return;
    }

    start_print_card();

    const uint32_t period_ms = level_period_ms(level_);
    timer_.start(period_ms, [this](uint32_t dt_ms) { run_frame(dt_ms); });
    // The display refreshes as often as the saver draws.
    helix::active_refresh_period_hold().follow(period_ms);
    active_ = true;
    spdlog::info("[Screensaver] Type {} running at level {} ({} ms frames, {}x{})",
                 static_cast<int>(type()), level_, period_ms, screen_w_, screen_h_);
}

void SaverBase::stop() {
    if (!active_) {
        return;
    }
    spdlog::info("[Screensaver] Stopping type {}", static_cast<int>(type()));
    timer_.cancel();
    stop_print_card();
    canvas_.release();
    overlay_.destroy();
    on_stop();
    print_.reset();
    active_ = false;
}

void SaverBase::set_start_level(size_t level) {
    start_level_ = std::min(level, ladder_size() - 1);
}

void SaverBase::request_level(size_t level) {
    if (!active_) {
        return;
    }
    on_level_request(std::min(level, ladder_size() - 1));
}

void SaverBase::on_level_request(size_t level) {
    apply_level(level);
}

void SaverBase::apply_level(size_t level) {
    level_ = std::min(level, ladder_size() - 1);
    const uint32_t period_ms = level_period_ms(level_);
    timer_.set_period(period_ms);
    helix::active_refresh_period_hold().follow(period_ms);
}

uint32_t SaverBase::level_period_ms(size_t level) const {
    const size_t clamped = std::min(level, ladder_size() - 1);
    // HELIX_SCREENSAVER_REFR_PERIOD_MS replaces level 0's period for manual testing.
    const uint32_t configured_ms = helix::active_refresh_period_hold().period();
    return clamped == 0 && configured_ms != 0 ? configured_ms : ladder_period_ms(clamped);
}

void SaverBase::run_frame(uint32_t dt_ms) {
    if (!active_) {
        return;
    }
    step_print_reading(dt_ms);
    dirty_.clear();
    on_frame(dt_ms, dirty_);
    if (!dirty_.empty()) {
        canvas_.invalidate(dirty_);
    }
    step_print_card(dt_ms, print_input_);
}

void SaverBase::step_print_reading(uint32_t dt_ms) {
    print_input_ = read_input();
    print_.step(dt_ms, print_input_);
}

void SaverBase::start_print_card() {
    const ScreensaverInfo* info = find_screensaver(type());
    card_.enabled = info && info->print_display == PrintDisplay::CARD;
    if (!card_.enabled) {
        return;
    }
    card_.layout = layout_print_card(screen_w_, screen_h_);
    card_.home = print_card_home(screen_w_, screen_h_, card_.layout);
    if (!card_.canvas.create(overlay_.obj(), card_.layout.w, card_.layout.h,
                             kSaverBuildCanvasFormat)) {
        card_.enabled = false;
        return;
    }
    lv_obj_t* obj = card_.canvas.obj();
    // DECLARATIVE_OK: a procedural canvas on a C++-built overlay; RGB565 has no alpha, so LVGL
    // rounds the card's corners.
    lv_obj_set_style_radius(obj, card_.layout.pad, 0);
    lv_obj_set_style_clip_corner(obj, true, 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); // DECLARATIVE_OK: shown by the print's state
    card_.painter.init(card_.layout);
    card_.drift.set_range(card_.home, screen_w_, screen_h_);
    card_.shown = false;
    card_.painted = false;
    step_print_card(0, print_input_);
}

void SaverBase::keep_print_card_on_top() {
    if (lv_obj_t* obj = card_.canvas.obj()) {
        lv_obj_move_foreground(obj);
    }
}

void SaverBase::step_print_card(uint32_t dt_ms, const ClockInput& in) {
    if (!card_.enabled) {
        return;
    }
    const ClockMode mode = print_.reading().mode;
    const bool visible = print_card_visible(PrintDisplay::CARD, in.show_print_progress, mode);
    const bool moved = card_.drift.step(dt_ms);
    lv_obj_t* obj = card_.canvas.obj();
    if (!visible) {
        if (card_.shown) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); // DECLARATIVE_OK: per-frame print state
            card_.shown = false;
        }
        return;
    }
    if (!card_.shown || moved) {
        lv_obj_set_pos(obj, card_.home.x1 + card_.drift.dx, card_.home.y1 + card_.drift.dy);
    }
    const PrintCardFace face = helix::ui::print_card_face(print_.reading());
    if (!card_.painted || face != card_.face) {
        card_.face = face;
        FrameTarget frame = card_.canvas.frame();
        card_.painter.draw(frame, face);
        card_.painted = true;
        lv_obj_invalidate(obj);
    }
    if (!card_.shown) {
        // Above whatever the scene added to the overlay since it started.
        lv_obj_move_foreground(obj);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN); // DECLARATIVE_OK: per-frame print state
        card_.shown = true;
    }
}

void SaverBase::stop_print_card() {
    if (card_.enabled) {
        card_.canvas.release();
        card_.painter.clear();
    }
    card_.enabled = false;
    card_.shown = false;
    card_.painted = false;
    card_.drift.reset();
    card_.face = {};
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
