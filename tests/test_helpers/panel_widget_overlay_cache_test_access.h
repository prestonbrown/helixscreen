// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "src/ui/panel_widgets/gcode_console_widget.h"
#include "src/ui/panel_widgets/macros_widget.h"
#include "src/ui/panel_widgets/motion_widget.h"

namespace helix {

// Friend access to the three PanelWidget overlay caches that are STATIC and so
// survive the printer switch which destroys their panels' overlay widgets.
// Tests open the overlays through lazy_create_and_push_overlay with the real
// cache storage (what MotionWidget::handle_click and siblings pass), then
// assert the switch sequence nulls them. Follows the tests/test_helpers/
// TestAccess pattern ([L088]).
//
// In ONE place: two test translation units each defining their own version of
// these classes would be an ODR violation.
class MotionWidgetTestAccess {
  public:
    static lv_obj_t*& motion_panel() {
        return MotionWidget::motion_panel_;
    }
};

class GCodeConsoleWidgetTestAccess {
  public:
    static lv_obj_t*& console_panel() {
        return GCodeConsoleWidget::console_panel_;
    }
};

class MacrosWidgetTestAccess {
  public:
    static lv_obj_t*& macros_panel() {
        return MacrosWidget::macros_panel_;
    }
};

} // namespace helix
