// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_nixie_clock.h"

#include "lvgl.h"
#include "screensaver_pixel_writer.h"

namespace helix::ui {

namespace {

/// The marker's text: a pale engraving on the dark socket, dimmer than the glow.
constexpr Rgb kTextColor{170, 150, 130};

} // namespace

void NixieClockScreensaver::reset_clock_text() {
    meridiem_.reset();
    sub_meridiem_.reset();
}

void NixieClockScreensaver::draw_clock_text(const std::vector<DirtyRect>& under,
                                            std::vector<DirtyRect>& dirty) {
    // The small row's marker changes only with a relayout, which clears the space beneath it.
    const FlipLayout& layout = sim_.layout();
    sub_meridiem_.draw(canvas(), clock_meridiem_text(layout.sub_meridiem), layout.sub_meridiem_box,
                       kTextColor, LV_OPA_COVER, LV_TEXT_ALIGN_LEFT, under, dirty);

    const FlipFace face = clock_.face();
    const std::string text = clock_meridiem_text(face.meridiem);
    const DirtyRect box = sim_.meridiem_box();
    if (!meridiem_.needs_draw(text, box, under)) {
        return;
    }
    // The old marker goes first, unless the frame just repainted the tube beneath it.
    const bool repainted = meridiem_.needs_draw(meridiem_.text(), meridiem_.box(), under);
    FrameTarget frame = canvas().frame();
    if (!meridiem_.text().empty() && !repainted) {
        sim_.redraw_tube(frame, 0, dirty);
    }
    meridiem_.draw(canvas(), text, box, kTextColor, LV_OPA_COVER, LV_TEXT_ALIGN_CENTER, under,
                   dirty);
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
