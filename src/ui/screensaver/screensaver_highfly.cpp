// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_highfly.h"

namespace helix::ui {

namespace {

/// The date block and AM/PM: the numerals' pale blue-white, a little more see-through.
constexpr Rgb TEXT_COLOR{226, 238, 250};
/// Their soft shadow, the numerals' deep blue.
constexpr Rgb kTextHalo{10, 50, 92};
constexpr lv_opa_t kTextHaloOpa = LV_OPA_60;

} // namespace

void HighFlyScreensaver::draw_clock_text(const std::vector<DirtyRect>& under,
                                         std::vector<DirtyRect>& dirty) {
    if (under.empty() && !date_line_.box().empty()) {
        // Nothing beneath the text was repainted, so a changed line waits for the next frame
        // that repaints the clock rather than drawing over the old one.
        return;
    }
    const ClockLines& lines = clock_lines_now();
    const HighFlyClockLayout& layout = sim_.clock_layout();
    // Every box sits inside the clock bounds, which the sim repaints before the text changes.
    date_line_.draw(canvas(), lines.first, layout.date_line, TEXT_COLOR, LV_OPA_80,
                    LV_TEXT_ALIGN_LEFT, under, dirty);
    day_line_.draw(canvas(), lines.second, layout.day_line, TEXT_COLOR, LV_OPA_80,
                   LV_TEXT_ALIGN_LEFT, under, dirty);
    meridiem_.draw(canvas(), lines.meridiem, layout.meridiem, TEXT_COLOR, LV_OPA_80,
                   LV_TEXT_ALIGN_LEFT, under, dirty);
}

void HighFlyScreensaver::reset_clock_text() {
    date_line_.reset();
    day_line_.reset();
    meridiem_.reset();
    const SaverLabelHalo halo{kTextHalo, kTextHaloOpa, sim_.clock_layout().text_halo};
    date_line_.set_halo(halo);
    day_line_.set_halo(halo);
    meridiem_.set_halo(halo);
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
