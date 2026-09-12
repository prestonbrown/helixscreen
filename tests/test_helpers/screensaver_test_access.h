// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "screensaver_pipes.h"
#include "screensaver_starfield.h"

#ifdef HELIX_ENABLE_SCREENSAVER

// Test-only seam (prestonbrown/helixscreen#1591). The canvas screensavers own
// their draw buffers privately, and the stride contract — the allocation must
// cover the extent lv_canvas_set_buffer() records from its aligned stride — is
// only observable from the allocation size and pitch they keep. Keeping these
// out of the production headers satisfies the "no _for_testing methods in
// headers" lint (mirrors display_manager_test_access.h).
class StarfieldScreensaverTestAccess {
  public:
    static size_t draw_buf_size(const StarfieldScreensaver& ss) {
        return ss.draw_buf_size_;
    }

    static uint32_t draw_buf_stride(const StarfieldScreensaver& ss) {
        return ss.draw_buf_stride_;
    }
};

class PipesScreensaverTestAccess {
  public:
    static size_t draw_buf_size(const PipesScreensaver& ss) {
        return ss.draw_buf_size_;
    }
};

#endif // HELIX_ENABLE_SCREENSAVER
