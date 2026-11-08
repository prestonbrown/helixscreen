// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "screensaver_bounce.h"

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
};

} // namespace helix

#endif // HELIX_ENABLE_SCREENSAVER
