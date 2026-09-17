// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_breakpoint.h"

#include "lvgl/lvgl.h"
#include "theme_manager.h"

namespace helix::test {

/// Set the global `ui_breakpoint` subject for a scope, then put it back.
///
/// The subject is process-global and there is exactly one of it. It is not a
/// convenience copy of the display size: PanelWidgetManager::populate_page()
/// and the theme's font ladder read THIS, not the display, so a test that
/// leaves it moved silently redefines the grid every later test builds — a
/// widget lands in a different track count, and the failure surfaces in a test
/// that never mentions resolution. Two such leaks (a forced Micro in
/// test_panel_widget_config and another in test_default_layout's TempCwdGuard)
/// cost four tests in the full suite while every one of them passed alone.
///
/// Set it through this guard, never with a bare lv_subject_set_int().
class ScopedBreakpoint {
  public:
    explicit ScopedBreakpoint(UiBreakpoint bp) {
        subj_ = theme_manager_get_breakpoint_subject();
        if (!subj_) {
            return;
        }
        // A zero-initialized subject in a test process that never ran
        // theme_manager_init() has no type yet; writing an int to it without
        // initializing first is undefined.
        if (subj_->type != LV_SUBJECT_TYPE_INT) {
            lv_subject_init_int(subj_, to_int(bp));
            original_ = to_int(bp);
            return;
        }
        original_ = lv_subject_get_int(subj_);
        lv_subject_set_int(subj_, to_int(bp));
    }

    ~ScopedBreakpoint() {
        if (subj_ && subj_->type == LV_SUBJECT_TYPE_INT) {
            lv_subject_set_int(subj_, original_);
        }
    }

    ScopedBreakpoint(const ScopedBreakpoint&) = delete;
    ScopedBreakpoint& operator=(const ScopedBreakpoint&) = delete;

  private:
    lv_subject_t* subj_ = nullptr;
    int32_t original_ = 0;
};

} // namespace helix::test
