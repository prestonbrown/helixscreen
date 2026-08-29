// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "power_device_widget.h"

namespace helix {

// Test-only access to PowerDeviceWidget's device-picker state.
//
// The picker backdrop carries an LV_EVENT_DELETE hook holding the widget, and
// dismiss_device_picker() hands the backdrop to safe_delete_deferred(), so the
// backdrop outlives the widget by at least one lv_timer_handler tick. A test
// needs the raw pointer and the two picker entry points to stand inside that
// window.
struct PowerDeviceWidgetTestAccess {
    /// parent_screen_ is the only thing show_device_picker() requires.
    /// attach() would also build sensor observers and a carousel, none of which
    /// the picker's lifetime depends on.
    static void set_parent_screen(PowerDeviceWidget& w, lv_obj_t* screen) {
        w.parent_screen_ = screen;
    }
    static void show_picker(PowerDeviceWidget& w) {
        w.show_device_picker();
    }
    static void dismiss_picker(PowerDeviceWidget& w) {
        w.dismiss_device_picker();
    }
    static lv_obj_t* picker_backdrop(PowerDeviceWidget& w) {
        return w.picker_backdrop_;
    }
    static PowerDeviceWidget* active_picker() {
        return PowerDeviceWidget::s_active_picker_;
    }
};

} // namespace helix
