// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "screensaver_bounce.h"
#include "screensaver_motion.h"

#include <cmath>

#if HELIX_ENABLE_SCREENSAVER

namespace helix {

// Test-only seam. The bounce keeps its speed and resize path private, and the tests
// pin how that path rescales motion (the screen's narrow axis sets the speed). Keeping
// these out of the production header satisfies the "no _for_testing methods in headers"
// lint (mirrors screensaver_test_access.h).
class BounceTestAccess {
  public:
    /// Runs the resize/fit path on its own, without an LVGL frame loop flipping the
    /// screen size underneath the saver.
    static void rebase(BouncingPrinterScreensaver& saver, int screen_w, int screen_h) {
        saver.rebase(screen_w, screen_h);
    }

    /// Path speed in px/s after the last rebase.
    static float speed(const BouncingPrinterScreensaver& saver) {
        return saver.speed_;
    }

    /// Dimensions of the decoded sprite source, or 0x0 before a start.
    static int32_t source_w(const BouncingPrinterScreensaver& saver) {
        return saver.src_w_;
    }
    static int32_t source_h(const BouncingPrinterScreensaver& saver) {
        return saver.src_h_;
    }

    /// Points the path at the top-left corner along the diagonal, so the sprite hits it
    /// `lead_s` seconds from now and then the opposite corner.
    static void aim_at_corner(BouncingPrinterScreensaver& saver, float lead_s) {
        const float t = static_cast<float>(saver.elapsed_ms_) / 1000.0f;
        const float diag = std::hypot(saver.range_x_, saver.range_y_);
        saver.vx_ = -saver.speed_ * saver.range_x_ / diag;
        saver.vy_ = -saver.speed_ * saver.range_y_ / diag;
        saver.x0_ = -saver.vx_ * (t + lead_s);
        saver.y0_ = -saver.vy_ * (t + lead_s);
        saver.prev_fold_x_ =
            helix::ui::screensaver::fold_index(saver.x0_ + saver.vx_ * t, saver.range_x_);
        saver.prev_fold_y_ =
            helix::ui::screensaver::fold_index(saver.y0_ + saver.vy_ * t, saver.range_y_);
    }
};

} // namespace helix

#endif // HELIX_ENABLE_SCREENSAVER
