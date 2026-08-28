// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_filament_mapping_modal.h"

/**
 * @brief Friend access to FilamentMappingModal's toggle and cancel paths.
 *
 * Both are reachable in production only from an LVGL callback — the toggle from
 * the switch's VALUE_CHANGED, cancel from the secondary button — and neither
 * touches a widget on the way in. Driving them directly is the point: the unit
 * under test is what the pair does to the PERSISTED auto-color preference, and
 * clicking a switch through the widget tree would test LVGL's event routing
 * instead of the revert.
 */
class FilamentMappingModalTestAccess {
  public:
    static void toggle(helix::ui::FilamentMappingModal& modal, bool auto_color) {
        modal.on_toggle_changed(auto_color);
    }

    static void cancel(helix::ui::FilamentMappingModal& modal) {
        modal.on_cancel();
    }

    static void ok(helix::ui::FilamentMappingModal& modal) {
        modal.on_ok();
    }
};
