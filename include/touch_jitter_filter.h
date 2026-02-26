// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC
//
// Touch jitter filter — suppresses small coordinate changes during
// stationary taps to prevent noisy touch controllers (e.g., Goodix GT9xx)
// from triggering LVGL scroll detection.

#pragma once

#include <lvgl.h>

struct TouchJitterFilter {
    /// Threshold in screen pixels (squared for fast comparison). 0 = disabled.
    int threshold_sq = 0;
    int last_x = 0;
    int last_y = 0;
    bool tracking = false;
    bool broken_out = false; ///< True once intentional movement detected; disables filtering

    /// Apply jitter filtering to touch coordinates.
    /// Suppresses movement within the dead zone until the first intentional movement
    /// exceeds the threshold. After breakout, all coordinates pass through unfiltered
    /// for the rest of the touch (smooth scrolling/dragging). On release, snaps to
    /// last stable position and resets for the next touch.
    void apply(lv_indev_state_t state, int32_t& x, int32_t& y) {
        if (threshold_sq <= 0)
            return;

        if (state == LV_INDEV_STATE_PRESSED) {
            if (!tracking) {
                last_x = x;
                last_y = y;
                tracking = true;
                broken_out = false;
            } else if (!broken_out) {
                int dx = x - last_x;
                int dy = y - last_y;
                if (dx * dx + dy * dy <= threshold_sq) {
                    x = last_x;
                    y = last_y;
                } else {
                    broken_out = true;
                }
            }
            // After breakout: pass through unfiltered (smooth drag/scroll)
        } else {
            if (tracking) {
                if (!broken_out) {
                    // Tap (never broke out): snap to initial press position
                    x = last_x;
                    y = last_y;
                }
                tracking = false;
                broken_out = false;
            }
        }
    }
};
