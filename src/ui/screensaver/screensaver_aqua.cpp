// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_aqua.h"

#include <string>

namespace helix::ui {

namespace {

/// The date or print line: a little dimmer than the digits in the rings.
constexpr Rgb LINE_COLOR{200, 206, 212};

} // namespace

void AquaScreensaver::draw_clock_text(const std::vector<DirtyRect>& under,
                                      std::vector<DirtyRect>& dirty) {
    // "ETA 9:41 PM · 1h 12m left" while a print runs, else "October 21, 2019 · Monday".
    const ClockLines& lines = clock_lines_now();
    if (lines.first.empty() || lines.second.empty()) {
        text_.assign(lines.first.empty() ? lines.second : lines.first);
    } else {
        text_.assign(lines.first).append(" · ").append(lines.second);
    }
    const DirtyRect& box = sim_.clock_layout().text;
    if (text_ != line_.text() || !(box == line_.box())) {
        // The old line comes off by putting back the water saved under it.
        FrameTarget frame = canvas().frame();
        sim_.restore(frame, line_.box(), dirty);
    }
    line_.draw(canvas(), text_, box, LINE_COLOR, LV_OPA_90, LV_TEXT_ALIGN_CENTER, under, dirty);
}

void AquaScreensaver::reset_clock_text() {
    line_.reset();
    std::string().swap(text_);
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
