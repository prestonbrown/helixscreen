// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "preheat_widget.h"

namespace helix {

// Friend access to PreheatWidget internals that production code reaches only
// through the tool-target button's click callback. The header declares this
// class inside namespace helix, so the definition must live in the same
// namespace - and in ONE place: two test translation units each defining their
// own version of the class would be an ODR violation.
//
//  - cycle(): the real cycle_tool_target(). Production calls it from
//    tool_target_cb(), which needs an attached widget tree, a live split
//    button and s_active_instance set. The function itself reads nothing but
//    ToolState, so a test that calls it directly still exercises every branch.
//  - tool_target() / set_tool_target(): the value the cycle walks. It is also
//    what handle_apply() hands collect_preheat_heaters() and what
//    update_tool_target_label() renders, so it is the whole observable result.
//
// Follows the tests/test_helpers/ TestAccess pattern ([L088]) rather than
// adding _for_testing() accessors to the production API.
class PreheatWidgetTestAccess {
  public:
    static void cycle(PreheatWidget& widget) {
        widget.cycle_tool_target();
    }

    static int tool_target(const PreheatWidget& widget) {
        return widget.tool_target_;
    }

    static void set_tool_target(PreheatWidget& widget, int target) {
        widget.tool_target_ = target;
    }
};

} // namespace helix
