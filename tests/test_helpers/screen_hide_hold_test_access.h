// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "screen_hide_hold.h"

namespace helix {

// Test-only seam. The shared hold is a process-wide static that no production path
// resets, so a test that fails between acquire and release leaves it held for every
// test that runs after it.
class ScreenHideHoldTestAccess {
  public:
    // Drops every outstanding acquire and unhides only the screen the hold hid, under
    // the same checks as release().
    static void reset(ScreenHideHold& hold) {
        hold.m_count = 0;
        hold.show_hidden_screen();
    }
};

} // namespace helix
