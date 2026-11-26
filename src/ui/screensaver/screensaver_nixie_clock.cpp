// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_nixie_clock.h"

#include "lvgl.h"
#include "screensaver_pixel_writer.h"

namespace helix::ui {

void NixieClockScreensaver::reset_clock_text() {
    markers_.reset();
}

void NixieClockScreensaver::draw_clock_text(const std::vector<DirtyRect>& under,
                                            std::vector<DirtyRect>& dirty) {
    markers_.draw_nixie(canvas(), sim_, clock_.face(), under, dirty);
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
