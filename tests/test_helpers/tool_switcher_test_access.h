// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "src/ui/panel_widgets/tool_switcher_widget.h"

namespace helix {

// Friend access to ToolSwitcherWidget's print gate. Both members are reachable
// in production only by tapping the tile on an attached home-panel widget, which
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

    /// The verdict the widget last published.
    static std::string head_temp_text(const ToolSwitcherWidget& widget, int head) {
        return widget.head_temp_text(head);
    }
    static bool head_hot(const ToolSwitcherWidget& widget, int head) {
        return widget.head_hot(head);
    }

    static ToolSwitcherLayout layout(const ToolSwitcherWidget& widget) {
        return widget.layout_;
    }

    /// The tile the widget is attached to, or null once it is gone.
    static lv_obj_t* tile_root(const ToolSwitcherWidget& widget) {
        return widget.widget_obj_;
    }

    /// The funnel every Park tap reaches.
    static void park(ToolSwitcherWidget& widget) {
        widget.handle_park_requested();
    }

    /// Raise the compact form's tool picker, as a tap on the compact tile does.
    static void show_picker(ToolSwitcherWidget& widget) {
        widget.show_tool_picker();
    }

    static void hide_picker(ToolSwitcherWidget& widget) {
        widget.hide_mount_picker();
    }

    /// The picker's tool list, or null while the picker this tile raised is hidden.
    static lv_obj_t* picker_list(ToolSwitcherWidget& widget) {
        if (!widget.has_overlay_open() || !widget.parent_screen_) {
            return nullptr;
        }
        return lv_obj_find_by_name(widget.parent_screen_, "tool_list");
    }
};

} // namespace helix
