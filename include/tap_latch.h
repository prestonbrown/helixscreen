// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lvgl.h>

namespace helix {

/**
 * @brief Edge-latches pointer presses sampled from a polled read callback
 *
 * A poll loop that only tests `data.state == LV_INDEV_STATE_PRESSED` samples a
 * *level*, so it sees a tap only if a poll happens to land between the press
 * and the release. LVGL's evdev driver makes that worse: `_evdev_read()` drains
 * the whole fd in one call and reports only the resulting final state, so a
 * press and its release that both arrive between two polls collapse into a
 * single RELEASED sample and the tap is lost entirely.
 *
 * TapLatch turns those samples into an edge:
 *
 * - Any PRESSED sample latches (a press was directly observed).
 * - A RELEASED sample whose coordinate moved since the previous sample also
 *   latches. The evdev driver only updates its coordinates while a contact is
 *   down, so a released position that is not where it was last poll is evidence
 *   that a contact came and went inside the drain window.
 *
 * The coordinate rule is what recovers a collapsed press+release, and it is
 * only valid for a contact-driven device. A mouse reports motion with no button
 * held, so on pointer devices that hover, construct with
 * `detect_collapsed_taps = false` and the latch degrades to press-level
 * detection. Two taps on the exact same pixel within one latch lifetime are
 * also indistinguishable from one; reset() between phases keeps that bounded.
 *
 * Not thread safe. Fed and consumed from the main thread only.
 */
class TapLatch {
  public:
    explicit TapLatch(bool detect_collapsed_taps = true)
        : m_detect_collapsed(detect_collapsed_taps) {}

    /// Feed one sample straight from an indev read callback.
    void feed(const lv_indev_data_t& data) {
        if (data.state == LV_INDEV_STATE_PRESSED) {
            m_latched = true;
        } else if (m_detect_collapsed && m_have_point &&
                   (data.point.x != m_point.x || data.point.y != m_point.y)) {
            m_latched = true;
            m_collapsed = true;
        }
        m_point = data.point;
        m_have_point = true;
    }

    /// True once a press has been seen since the last reset()/consume().
    bool latched() const {
        return m_latched;
    }

    /// True if the latched press was recovered from a collapsed read (diagnostics).
    bool from_collapsed_read() const {
        return m_collapsed;
    }

    /// Read and clear the latch, keeping the coordinate baseline.
    bool consume() {
        bool was = m_latched;
        m_latched = false;
        m_collapsed = false;
        return was;
    }

    /// Clear the latch and the coordinate baseline.
    ///
    /// The next sample re-establishes the baseline and cannot latch on
    /// coordinates alone, so a stale position left by an earlier tap never
    /// reads as a fresh one.
    void reset() {
        m_latched = false;
        m_collapsed = false;
        m_have_point = false;
        m_point = lv_point_t{};
    }

  private:
    bool m_detect_collapsed;
    bool m_latched = false;
    bool m_collapsed = false;
    bool m_have_point = false;
    lv_point_t m_point{};
};

} // namespace helix
