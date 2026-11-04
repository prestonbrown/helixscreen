// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_fireflies.h"

#include <spdlog/spdlog.h>

using helix::ui::DirtyRect;

namespace {

const char* mode_name(helix::ui::FirefliesMode mode) {
    using helix::ui::FirefliesMode;
    switch (mode) {
    case FirefliesMode::STOP:
        return "stop";
    case FirefliesMode::LOOP:
        return "loop";
    case FirefliesMode::GLOW:
        return "glow";
    case FirefliesMode::HYPERSPEED:
        return "hyperspeed";
    case FirefliesMode::SPLIT:
        return "split";
    case FirefliesMode::MERGE:
        return "merge";
    case FirefliesMode::WIND_GUST:
        return "wind gust";
    default:
        return "normal";
    }
}

} // namespace

void helix::ui::FirefliesScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) {
    uint32_t begun[kFirefliesModeCount];
    for (size_t m = 0; m < kFirefliesModeCount; m++) {
        begun[m] = sim_.modes_begun(static_cast<FirefliesMode>(m));
    }
    step_sim(dt_ms, dirty);
    for (size_t m = 0; m < kFirefliesModeCount; m++) {
        if (sim_.modes_begun(static_cast<FirefliesMode>(m)) != begun[m]) {
            spdlog::debug("[Screensaver] Fireflies mode: {}",
                          mode_name(static_cast<FirefliesMode>(m)));
        }
    }
}

#endif // HELIX_ENABLE_SCREENSAVER
