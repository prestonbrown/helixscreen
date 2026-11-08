// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace helix::ui {

/// Refuses taps on a target that just appeared under the finger.
///
/// A tap that raises a target can land the target under the finger, so a
/// double tap's second press would act on it. A release acts only when its
/// press came at least the lockout after the target's first frame: the press,
/// not the release, decides, since a double tap's second press can fall inside
/// the lockout and lift after it. On a slow board the first frame lands well
/// after the target is created, so the lockout runs from the frame.
///
/// Pure: the caller feeds it ticks from its target's events.
class TapLockout {
  public:
    static constexpr uint32_t kDefaultMs = 300;

    explicit TapLockout(uint32_t ms = kDefaultMs) : ms_(ms) {}

    /// The target was created at @p tick; the lockout runs from it until a
    /// frame shows the target.
    void reset(uint32_t tick) {
        start_tick_ = tick;
        drawn_ = false;
        press_latched_ = false;
    }

    /// LV_EVENT_DRAW_POST on the target: the first call fixes the start of the
    /// lockout.
    void on_drawn(uint32_t tick) {
        if (!drawn_) {
            drawn_ = true;
            start_tick_ = tick;
        }
    }

    /// The finger that raised the target lifted at @p tick: a bounce of that
    /// lift is no tap, so the lockout runs from here.
    void restart(uint32_t tick) {
        start_tick_ = tick;
    }

    void on_press(uint32_t tick) {
        press_tick_ = tick;
        press_latched_ = true;
    }

    void on_press_lost() {
        press_latched_ = false;
    }

    /// Whether a release acts: it lifted over the target, and its press (or
    /// @p now, for a click with no press behind it) came at least the lockout
    /// after the first frame. Consumes the press.
    [[nodiscard]] bool release_acts(bool released_over_target, uint32_t now) {
        const uint32_t pressed_at = press_latched_ ? press_tick_ : now;
        press_latched_ = false;
        // Signed: a press read before the first frame stamped the clock is
        // inside the lockout too.
        const auto since_shown = static_cast<int32_t>(pressed_at - start_tick_);
        return released_over_target && since_shown >= static_cast<int32_t>(ms_);
    }

  private:
    uint32_t ms_;
    uint32_t start_tick_ = 0;
    bool drawn_ = false;
    uint32_t press_tick_ = 0;
    bool press_latched_ = false;
};

} // namespace helix::ui
