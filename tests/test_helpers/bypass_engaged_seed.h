// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_state.h"
#include "lvgl/lvgl.h"

namespace helix::test {

/// AmsState's subjects, registered for XML, with bypass engaged for a scope. The
/// bypass tile then draws its spool row, the tallest thing it ever draws. Construct
/// it before building the tile: a binding whose subject is missing at parse time is
/// dropped, and the row's visibility then follows only its other bindings.
class BypassEngagedSeed {
  public:
    BypassEngagedSeed() {
        AmsState::instance().init_subjects(true);
        lv_subject_set_int(AmsState::instance().get_bypass_active_subject(), 1);
    }
    ~BypassEngagedSeed() {
        lv_subject_set_int(AmsState::instance().get_bypass_active_subject(), 0);
    }
    BypassEngagedSeed(const BypassEngagedSeed&) = delete;
    BypassEngagedSeed& operator=(const BypassEngagedSeed&) = delete;
};

} // namespace helix::test
