// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "asleep_status_wiring.h"

#include "core/update_queue.h"
#include "display_manager.h"
#include "moonraker_manager.h"

#include <utility>

namespace helix {

void wire_asleep_status(DisplayManager& display, std::function<MoonrakerManager*()> moonraker) {
    // Waking ends the hold in the settle below, never here: a wake inside an UpdateQueue
    // batch notifies at once, and status is never applied inside a batch.
    display.register_sleep_callback([moonraker](bool sleeping) {
        if (!sleeping) {
            return;
        }
        if (MoonrakerManager* manager = moonraker()) {
            manager->set_status_hold(true);
        }
    });
    display.set_wake_frame_settle([moonraker = std::move(moonraker)] {
        if (MoonrakerManager* manager = moonraker()) {
            manager->set_status_hold(false);
        }
        ui::UpdateQueue::instance().drain_until_empty();
    });
}

} // namespace helix
