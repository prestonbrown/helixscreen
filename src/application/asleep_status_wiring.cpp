// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "asleep_status_wiring.h"

#include "core/update_queue.h"
#include "display_manager.h"
#include "lvgl.h"
#include "moonraker_manager.h"

#include <utility>

namespace helix {

void wire_asleep_status(DisplayManager& display, std::function<MoonrakerManager*()> moonraker) {
    // Waking ends the status hold in the settle below, never here: a wake inside an
    // UpdateQueue batch notifies at once, and status is never applied inside a batch.
    display.register_sleep_callback([moonraker](bool sleeping) {
        if (!sleeping) {
            // The settle has already released the widgets on every wake that drew a
            // frame; this covers a wake that ended the sleep without one.
            lv_subject_hold_obj_observers(false);
            return;
        }
        lv_subject_hold_obj_observers(true);
        if (MoonrakerManager* manager = moonraker()) {
            manager->set_status_hold(true);
        }
    });
    // Status first, so the widgets are released onto the values it brings.
    display.set_wake_frame_settle([moonraker = std::move(moonraker)] {
        if (MoonrakerManager* manager = moonraker()) {
            manager->set_status_hold(false);
        }
        lv_subject_hold_obj_observers(false);
        ui::UpdateQueue::instance().drain_until_empty();
    });
}

} // namespace helix
