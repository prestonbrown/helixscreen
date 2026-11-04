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
    if (!on_start()) {
        spdlog::warn("[Screensaver] Type {} refused to start", static_cast<int>(type()));
        canvas_.release();
        overlay_.destroy();
        return;
    }

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
    canvas_.release();
    overlay_.destroy();
    on_stop();
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
    dirty_.clear();
    on_frame(dt_ms, dirty_);
    if (!dirty_.empty()) {
        canvas_.invalidate(dirty_);
    }
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
