// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_frame_timer.h"

#include "ui_timer_guard.h" // lv_timer_cancel_safe

#include <utility>

namespace helix::ui {

SaverFrameTimer::~SaverFrameTimer() {
    cancel();
}

void SaverFrameTimer::start(uint32_t period_ms, FrameFn on_frame) {
    cancel();
    on_frame_ = std::move(on_frame);
    clock_.reset(lv_tick_get());
    timer_ = lv_timer_create(timer_cb, period_ms, this);
}

void SaverFrameTimer::set_period(uint32_t period_ms) {
    if (timer_) {
        lv_timer_set_period(timer_, period_ms);
    }
}

void SaverFrameTimer::cancel() {
    if (timer_) {
        // Neuters instead of unlinking and guards on lv_is_initialized(), which is what makes it
        // safe from the destructor, from inside lv_timer_handler and after lv_deinit (#750, #1173).
        lv_timer_cancel_safe(timer_);
        timer_ = nullptr;
    }
}

void SaverFrameTimer::timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<SaverFrameTimer*>(lv_timer_get_user_data(timer));
    if (!self || !self->on_frame_) {
        return;
    }
    self->on_frame_(self->clock_.advance(lv_tick_get()));
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
