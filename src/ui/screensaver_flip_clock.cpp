// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_flip_clock.h"

#include "lvgl.h"
#include "screensaver_pixel_writer.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace helix::ui {

namespace {

/// Caption and marker text: dimmer than the digits, so the cards stay the subject.
constexpr Rgb TEXT_COLOR{150, 150, 150};

} // namespace

std::string FlipClockScreensaver::caption_text(const FlipFace& face) {
    switch (face.caption) {
    case FlipCaption::DONE_AT: {
        return fmt::format(fmt::runtime(lv_tr("done at {} · {}%")),
                           clock_minute_text(face.eta_minute_of_day), face.progress_pct);
    }
    case FlipCaption::PAUSED:
    case FlipCaption::DONE:
        // The print line's first half: PAUSED or DONE.
        return clock_lines_now().first;
    case FlipCaption::NONE:
        break;
    }
    return {};
}

void FlipClockScreensaver::reset_clock_text() {
    caption_.reset();
    meridiem_.reset();
}

void FlipClockScreensaver::draw_clock_text(const std::vector<DirtyRect>& under,
                                           std::vector<DirtyRect>& dirty) {
    const FlipFace face = clock_.face();
    const FlipLayout& layout = sim_.layout();
    draw_text(caption_, caption_text(face), layout.caption, under, false, dirty);
    draw_text(meridiem_, clock_meridiem_text(face.meridiem), layout.meridiem, under, true, dirty);
}

void FlipClockScreensaver::draw_text(SaverLabel& label, const std::string& text,
                                     const DirtyRect& box, const std::vector<DirtyRect>& under,
                                     bool on_card, std::vector<DirtyRect>& dirty) {
    if (!label.needs_draw(text, box, under)) {
        return;
    }
    // Old text goes first, unless the frame just repainted beneath it: the caption sits on
    // the black background, the marker on the first card, which the sim draws again.
    const bool repainted = label.needs_draw(label.text(), label.box(), under);
    FrameTarget frame = canvas().frame();
    if (!label.text().empty() && !repainted) {
        if (on_card) {
            sim_.redraw_card(frame, 0, dirty);
        } else {
            PixelWriter(frame).fill_rect(label.box(), FlipClockSim::kBackground);
            dirty.push_back(label.box());
        }
    }
    label.draw(canvas(), text, box, TEXT_COLOR, LV_OPA_COVER,
               on_card ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, under, dirty);
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
