// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_flip_clock.h"

#include "lvgl.h"
#include "screensaver_pixel_writer.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

namespace {

/// The marker's text: dimmer than the digits, so the cards stay the subject.
constexpr Rgb kTextColor{150, 150, 150};

} // namespace

void FlipClockScreensaver::reset_clock_text() {
    meridiem_.reset();
}

void FlipClockScreensaver::draw_clock_text(const std::vector<DirtyRect>& under,
                                           std::vector<DirtyRect>& dirty) {
    const FlipFace face = clock_.face();
    const std::string text = clock_meridiem_text(face.meridiem);
    const DirtyRect& box = sim_.layout().meridiem;
    if (!meridiem_.needs_draw(text, box, under)) {
        return;
    }
    // The old marker goes first, unless the frame just repainted the card beneath it.
    const bool repainted = meridiem_.needs_draw(meridiem_.text(), meridiem_.box(), under);
    FrameTarget frame = canvas().frame();
    if (!meridiem_.text().empty() && !repainted) {
        sim_.redraw_card(frame, 0, dirty);
    }
    meridiem_.draw(canvas(), text, box, kTextColor, LV_OPA_COVER, LV_TEXT_ALIGN_LEFT, under, dirty);
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
