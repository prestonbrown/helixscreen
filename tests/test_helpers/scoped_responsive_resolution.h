// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <optional>

namespace helix::test {

/// Resize the display for a scope AND move the responsive state with it.
///
/// `ScopedResolution` deliberately changes pixel dimensions only — the
/// content-fit sweep measures across geometries inside such a scope and needs
/// the token set to hold still. What a widget is laid out *against* is
/// different state: the process-global `ui_breakpoint` /`ui_breakpoint_v` /
/// `ui_is_portrait` subjects and the px/font token table, republished from the
/// display by `theme_manager_refresh_layout_constants()`. A test that wants the
/// tier to follow the pixels needs both, in both directions.
///
/// Pairing them by hand has a trap worth knowing: a trailing refresh written
/// inside the resolution guard's own scope still runs at the temporary size, so
/// it republishes the value it was meant to undo, and the tier is left behind
/// for every later test in the process — where it decides layout in a case that
/// never mentions resolution. This guard refreshes after the resolution is
/// back, so the restore restores.
class ScopedResponsiveResolution {
  public:
    ScopedResponsiveResolution(lv_display_t* disp, int32_t w, int32_t h) : disp_(disp) {
        res_.emplace(disp, w, h);
        theme_manager_refresh_layout_constants(disp_);
    }

    ~ScopedResponsiveResolution() {
        // Order is the whole point: dropping the inner guard puts the original
        // resolution back, and only then does the refresh read the size the
        // rest of the suite expects.
        res_.reset();
        theme_manager_refresh_layout_constants(disp_);
    }

    ScopedResponsiveResolution(const ScopedResponsiveResolution&) = delete;
    ScopedResponsiveResolution& operator=(const ScopedResponsiveResolution&) = delete;

  private:
    lv_display_t* disp_;
    std::optional<ScopedResolution> res_;
};

} // namespace helix::test
