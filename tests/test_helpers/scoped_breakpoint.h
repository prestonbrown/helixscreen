// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_breakpoint.h"

#include "core/subject.h"
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
        // In a test process that never ran theme_manager_init() the subject is not
        // live yet and refuses a set; initialize it instead.
        if (auto* owner = helix::Subject::owner_of(subj_); owner && !owner->is_live()) {
            owner->init_int(to_int(bp));
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

/// Publish `ui_breakpoint` from the default display when it is down, as
/// theme_manager_init() does before the app attaches any widget. A widget that
/// observes the breakpoint (PrintStatusWidget) would otherwise have its observer
/// refused whenever its case runs before any theme fixture in the shard, or after
/// one that tore the theme down. A live subject is left alone.
inline void ensure_breakpoint_published() {
    lv_subject_t* const subj = theme_manager_get_breakpoint_subject();
    if (auto* owner = helix::Subject::owner_of(subj); owner && !owner->is_live()) {
        owner->init_int(to_int(breakpoint_for(responsive_dimension(lv_display_get_default()))));
    }
}

} // namespace helix::test
