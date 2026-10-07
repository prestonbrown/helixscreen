// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "power_device_widget.h"

namespace helix {

// Test-only access to PowerDeviceWidget's device picker.
struct PowerDeviceWidgetTestAccess {
    /// The two pointers show_device_picker() needs: the screen the backdrop goes
    /// on and the tile the card hangs off. attach() would also build sensor
    /// observers and a carousel, none of which the picker depends on.
    static void set_screen(PowerDeviceWidget& w, lv_obj_t* screen, lv_obj_t* tile) {
        w.parent_screen_ = screen;
        w.widget_obj_ = tile;
    }
    static void show_picker(PowerDeviceWidget& w) {
        w.show_device_picker();
    }
    static void hide_picker(PowerDeviceWidget& w) {
        w.picker_.hide();
    }
    static bool picker_visible(const PowerDeviceWidget& w) {
        return w.picker_.is_visible();
    }
    static const helix::ui::ContextMenu* picker(const PowerDeviceWidget& w) {
        return &w.picker_;
    }
    /// The open picker's backdrop on @p screen, found through the card the
    /// shared context_menu_card component names, or nullptr.
    static lv_obj_t* backdrop(lv_obj_t* screen) {
        lv_obj_t* card = lv_obj_find_by_name(screen, "context_menu");
        return card ? lv_obj_get_parent(card) : nullptr;
    }
    static const std::string& device_name(const PowerDeviceWidget& w) {
        return w.device_name_;
    }
};

} // namespace helix
