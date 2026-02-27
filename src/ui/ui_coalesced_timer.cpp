// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_coalesced_timer.h"

namespace helix::ui {

CoalescedTimer::CoalescedTimer(uint32_t period_ms) : period_ms_(period_ms) {}

CoalescedTimer::~CoalescedTimer() {
    cancel();
}

CoalescedTimer::CoalescedTimer(CoalescedTimer&& other) noexcept
    : timer_(other.timer_), callback_(std::move(other.callback_)), period_ms_(other.period_ms_) {
    other.timer_ = nullptr;
    // Update the LVGL timer's user_data to point to this new location
    if (timer_) {
        lv_timer_set_user_data(timer_, this);
    }
}

CoalescedTimer& CoalescedTimer::operator=(CoalescedTimer&& other) noexcept {
    if (this != &other) {
        cancel();
        timer_ = other.timer_;
        callback_ = std::move(other.callback_);
        period_ms_ = other.period_ms_;
        other.timer_ = nullptr;
        if (timer_) {
            lv_timer_set_user_data(timer_, this);
        }
    }
    return *this;
}

void CoalescedTimer::schedule(std::function<void()> cb) {
    callback_ = std::move(cb);

    if (timer_) {
        lv_timer_reset(timer_);
    } else {
        timer_ = lv_timer_create(timer_cb, period_ms_, this);
        lv_timer_set_repeat_count(timer_, 1);
    }
}

void CoalescedTimer::cancel() {
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    callback_ = nullptr;
}

bool CoalescedTimer::pending() const {
    return timer_ != nullptr;
}

void CoalescedTimer::timer_cb(lv_timer_t* t) {
    auto* self = static_cast<CoalescedTimer*>(lv_timer_get_user_data(t));
    self->timer_ = nullptr; // One-shot: clear before invoking (allows re-schedule from callback)
    if (self->callback_) {
        auto cb = std::move(self->callback_);
        cb();
    }
}

} // namespace helix::ui
