// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl.h"

#include <functional>
#include <memory>
#include <utility>

namespace helix {

/// Run @p fn on the main thread once @p delay_ms has passed. Main thread only.
/// Captures nothing of a caller's object, so it outlives whatever started it;
/// @p fn must guard anything it reaches itself.
inline void run_after_ms(uint32_t delay_ms, std::function<void()> fn) {
    auto* holder = new std::function<void()>(std::move(fn));
    lv_timer_t* timer = lv_timer_create( // TIMER_DTOR_OK: one-shot, owns its own payload
        [](lv_timer_t* t) {
            std::unique_ptr<std::function<void()>> owned(
                static_cast<std::function<void()>*>(lv_timer_get_user_data(t)));
            (*owned)();
        },
        delay_ms, holder);
    lv_timer_set_repeat_count(timer, 1);
}

} // namespace helix
