// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_flip_clock.h"

#include "lvgl.h"
#include "screensaver_pixel_writer.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

bool FlipClockScreensaver::on_start() {
    if (!ClockSaver::on_start()) {
        return false;
    }
    FrameTarget frame = canvas().frame();
    std::vector<DirtyRect> dirty;
    sim_.spin_up(frame, dirty);
    // The marker goes back over the cards spun back beneath it.
    const std::vector<DirtyRect> spun = dirty;
    draw_clock_text(spun, dirty);
    return true;
}

void FlipClockScreensaver::reset_clock_text() {
    markers_.reset();
}

void FlipClockScreensaver::draw_clock_text(const std::vector<DirtyRect>& under,
                                           std::vector<DirtyRect>& dirty) {
    markers_.draw_flip(canvas(), sim_, clock_.face(), under, dirty);
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
