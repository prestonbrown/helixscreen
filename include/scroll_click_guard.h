// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <lvgl.h>

namespace helix {

/**
 * @brief Ignores the press a capacitive panel can report as a finger lifts off a scroll
 *
 * The contact area shrinks as the finger leaves the glass, and some controllers
 * report that as release, press, release. The stray press lands on whatever the
 * scroll moved under the finger and LVGL clicks it. While enabled, a press that
 * starts within `cooldown_ms` of a scrolling touch ending reads as RELEASED.
 *
 * A touch counts as scrolling once it moves `scroll_limit_px` or more along
 * either axis from where it went down, the distance at which LVGL starts a
 * scroll. Taps, long presses and a press that starts after the
 * cooldown pass unchanged; so does everything when disabled.
 *
 * Main thread only, fed from a pointer's read callback.
 */
struct ScrollClickGuard {
    static constexpr uint32_t DEFAULT_COOLDOWN_MS = 80;
    static constexpr int MIN_COOLDOWN_MS = 20;
    static constexpr int MAX_COOLDOWN_MS = 500;

    bool enabled = false;
    uint32_t cooldown_ms = DEFAULT_COOLDOWN_MS;
    int32_t scroll_limit_px = 10;

    /**
     * @brief Build a guard from the saved settings and their environment overrides
     *
     * HELIX_SCROLL_GUARD ("1"/"true" enables, any other value disables) and
     * HELIX_SCROLL_GUARD_COOLDOWN_MS win over the saved values when set. The
     * cooldown is clamped to MIN_COOLDOWN_MS..MAX_COOLDOWN_MS.
     *
     * @param env_enabled  value of HELIX_SCROLL_GUARD, nullptr when unset
     * @param env_cooldown value of HELIX_SCROLL_GUARD_COOLDOWN_MS, nullptr when unset
     */
    static ScrollClickGuard from_settings(bool saved_enabled, int saved_cooldown_ms,
                                          const char* env_enabled, const char* env_cooldown,
                                          int scroll_limit_px) {
        ScrollClickGuard guard;
        guard.enabled = saved_enabled;
        if (env_enabled) {
            guard.enabled =
                std::strcmp(env_enabled, "1") == 0 || std::strcmp(env_enabled, "true") == 0;
        }
        const int cooldown = env_cooldown ? std::atoi(env_cooldown) : saved_cooldown_ms;
        guard.cooldown_ms =
            static_cast<uint32_t>(std::clamp(cooldown, MIN_COOLDOWN_MS, MAX_COOLDOWN_MS));
        guard.scroll_limit_px = scroll_limit_px;
        return guard;
    }

    /**
     * @brief Feed one pointer sample; may turn a press into a release
     *
     * @param now_ms the LVGL tick the sample was read at
     * @return true when this sample's press was suppressed
     */
    bool filter(lv_indev_state_t& state, lv_point_t point, uint32_t now_ms) {
        if (!enabled) {
            return false;
        }
        if (state == LV_INDEV_STATE_PRESSED) {
            if (!m_pressed) {
                if (m_cooling && now_ms - m_released_at < cooldown_ms) {
                    state = LV_INDEV_STATE_RELEASED;
                    return true;
                }
                m_cooling = false;
                m_pressed = true;
                m_scrolled = false;
                m_origin = point;
            } else if (std::abs(point.x - m_origin.x) >= scroll_limit_px ||
                       std::abs(point.y - m_origin.y) >= scroll_limit_px) {
                m_scrolled = true;
            }
            return false;
        }
        if (m_pressed) {
            m_pressed = false;
            m_cooling = m_scrolled;
            m_released_at = now_ms;
        }
        return false;
    }

  private:
    bool m_pressed = false;
    bool m_scrolled = false;
    bool m_cooling = false;
    lv_point_t m_origin{};
    uint32_t m_released_at = 0;
};

} // namespace helix
