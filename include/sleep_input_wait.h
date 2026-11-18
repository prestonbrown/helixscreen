// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "display_backend.h"
#include "lvgl.h"

#include <cstdint>
#include <vector>

namespace helix {

/**
 * @brief Waits on input and queued work instead of polling while the display sleeps
 *
 * Awake, every input device is read by its 33 ms LVGL timer, which wakes the main loop
 * about 30 times a second. While asleep, begin() pauses the read timers of the devices
 * whose events arrive on a known fd, and wait() sleeps on those fds and the main-loop wake
 * fd (main_loop_wake.h) until one turns readable or the timeout passes. A device that
 * turned readable is read at once, so a wake touch reaches the sleep controller as soon
 * as the kernel delivers it. Devices without a known fd keep their timers.
 */
class SleepInputWait {
  public:
    /// Pauses the read timer of each source that is still an LVGL input device and whose
    /// timer is running. Calling it again first ends the previous wait.
    void begin(const std::vector<InputWakeSource>& sources);

    /// Resumes the timers begin() paused, for the devices LVGL still has.
    void end();

    [[nodiscard]] bool active() const {
        return m_active;
    }

    /// Sleeps up to @p timeout_ms, returning early when a paused device or the main-loop
    /// wake fd turns readable. Reads each paused device that did. Returns true when woken
    /// before the timeout.
    bool wait(uint32_t timeout_ms);

  private:
    std::vector<InputWakeSource> m_paused;
    bool m_active = false;
};

/// True when @p indev is one of LVGL's input devices right now.
bool indev_is_live(const lv_indev_t* indev);

} // namespace helix
