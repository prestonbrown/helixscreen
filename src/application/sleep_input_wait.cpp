// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sleep_input_wait.h"

#include "main_loop_wake.h"

#include <algorithm>
#include <cerrno>
#include <poll.h>
#include <utility>

namespace helix {

bool indev_is_live(const lv_indev_t* indev) {
    for (lv_indev_t* i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i)) {
        if (i == indev) {
            return true;
        }
    }
    return false;
}

void SleepInputWait::begin(const std::vector<InputWakeSource>& sources) {
    end();
    for (const InputWakeSource& s : sources) {
        if (s.fd < 0 || !indev_is_live(s.indev)) {
            continue;
        }
        lv_timer_t* timer = lv_indev_get_read_timer(s.indev);
        if (!timer || lv_timer_get_paused(timer)) {
            continue;
        }
        lv_timer_pause(timer);
        m_paused.push_back(s);
    }
    m_active = true;
}

void SleepInputWait::end() {
    for (const InputWakeSource& s : m_paused) {
        if (!indev_is_live(s.indev)) {
            continue;
        }
        if (lv_timer_t* timer = lv_indev_get_read_timer(s.indev)) {
            lv_timer_resume(timer);
        }
    }
    m_paused.clear();
    m_active = false;
}

bool SleepInputWait::wait(uint32_t timeout_ms) {
    // A device LVGL deleted has closed its fd, which a later open may reuse.
    m_paused.erase(std::remove_if(m_paused.begin(), m_paused.end(),
                                  [](const InputWakeSource& s) { return !indev_is_live(s.indev); }),
                   m_paused.end());

    std::vector<pollfd> fds;
    fds.reserve(m_paused.size() + 1);
    for (const InputWakeSource& s : m_paused) {
        fds.push_back({s.fd, POLLIN, 0});
    }
    const int loop_fd = main_loop_wake::fd();
    if (loop_fd >= 0) {
        fds.push_back({loop_fd, POLLIN, 0});
    }

    int n = poll(fds.data(), fds.size(), static_cast<int>(timeout_ms));
    if (n < 0 && errno == EINTR) {
        n = 0;
    }
    if (n <= 0) {
        return false;
    }
    // A source whose fd is closed or failed would answer every poll at once, so it is
    // dropped and its device handed back to its read timer, which sees the failure.
    std::vector<lv_indev_t*> readable;
    std::vector<InputWakeSource> kept;
    kept.reserve(m_paused.size());
    for (size_t i = 0; i < m_paused.size(); ++i) {
        const InputWakeSource& s = m_paused[i];
        if ((fds[i].revents & (POLLNVAL | POLLERR | POLLHUP)) != 0) {
            if (lv_timer_t* timer = lv_indev_get_read_timer(s.indev)) {
                lv_timer_resume(timer);
            }
            continue;
        }
        kept.push_back(s);
        if (fds[i].revents != 0) {
            readable.push_back(s.indev);
        }
    }
    m_paused = std::move(kept);
    // A read can wake the display, whose callback ends this wait and resumes every timer.
    for (lv_indev_t* indev : readable) {
        if (!m_active) {
            break;
        }
        lv_indev_read(indev);
    }
    if (loop_fd >= 0 && fds.back().revents != 0) {
        main_loop_wake::drain();
    }
    return true;
}

} // namespace helix
