// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_fireworks.h"

void helix::ui::FireworksScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) {
    step_sim(dt_ms, dirty);
    if (sim_.level() != level()) {
        apply_level(sim_.level());
    }
}

#endif // HELIX_ENABLE_SCREENSAVER
