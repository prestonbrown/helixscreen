// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_calibration_holds.h"

#include <memory>
#include <utility>

namespace helix::calibration {

void start_held_calibration_gcode(IMoonrakerAPI* api, MachineGcode cmd, bool needs_home,
                                  const char* caller_tag) {
    auto busy = std::make_shared<MachineBusyToken>();
    busy->acquire();
    cmd.on_finished = [busy, chained = std::move(cmd.on_finished)]() {
        busy->reset();
        if (chained) {
            chained();
        }
    };
    start_calibration_gcode(api, cmd, needs_home, caller_tag);
}

} // namespace helix::calibration
