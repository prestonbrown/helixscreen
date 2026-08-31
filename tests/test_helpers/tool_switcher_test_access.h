// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "src/ui/panel_widgets/tool_switcher_widget.h"

namespace helix {

// Friend access to ToolSwitcherWidget's print gate. Both members are reachable
// in production only by tapping a pill on an attached home-panel widget, which
// needs a mounted XML tree, a sized grid cell and a live touch event — none of
// which say anything useful about the policy being asserted.
//
//  - tool_change_refusal(): the predicate itself, so the four
//    (print state x backend self-homes) cells can be pinned directly.
//  - handle_tool_selected(): the funnel every tap reaches, so a refusal can be
//    checked for reaching the user rather than only for returning early.
//
// The header forward-declares this class inside namespace helix, so the
// definition must live in the same namespace — and in ONE place, or two test
// translation units defining their own copy would be an ODR violation.
// Follows the tests/test_helpers/ TestAccess pattern ([L088]).
class ToolSwitcherTestAccess {
  public:
    static AmsError refusal(const ToolSwitcherWidget& widget) {
        return widget.tool_change_refusal();
    }

    static void select_tool(ToolSwitcherWidget& widget, int tool_index) {
        widget.handle_tool_selected(tool_index);
    }

    /// The pill list refresh_print_gating() greys. Writable so a test can hand
    /// it plain buttons instead of standing up a mounted XML tree.
    static std::vector<lv_obj_t*>& pills(ToolSwitcherWidget& widget) {
        return widget.pill_buttons_;
    }

    /// Compact-mode label refresh_print_gating() greys. Nullable read so a
    /// teardown test can assert it was dropped alongside the pills.
    static lv_obj_t* compact_label(const ToolSwitcherWidget& widget) {
        return widget.compact_label_;
    }
};

} // namespace helix
