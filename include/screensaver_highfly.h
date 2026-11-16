// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"
#include "screensaver_clock.h"
#include "screensaver_clock_saver.h"
#include "screensaver_highfly_sim.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <vector>

namespace helix::ui {

/// High Fly's text block and AM/PM: while the numerals count down (highfly_counts_down()), the
/// print's first line and the time of day, with no AM/PM since the numerals are not a time;
/// clock_lines() otherwise.
ClockLines highfly_clock_lines(const ClockReading& reading);

/**
 * @brief High Fly: over a sunlit cloud deck with a thin clock at the cloud line
 *
 * helix::ui::HighFlySim draws the sky, clouds and numerals; this class draws the date block
 * and AM/PM with lv_draw_label, so the canvas takes kLvDrawCanvasFormat.
 */
class HighFlyScreensaver : public ClockSaver<HighFlySim> {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::HIGHFLY;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return kLvDrawCanvasFormat;
    }
    /// The date block and AM/PM, put back wherever `under` repainted beneath them.
    void draw_clock_text(const std::vector<DirtyRect>& under,
                         std::vector<DirtyRect>& dirty) override;
    void reset_clock_text() override;

  private:
    friend class HighFlyScreensaverTestAccess;

    SaverLabel date_line_;
    SaverLabel day_line_;
    SaverLabel meridiem_;
    ClockTextCache lines_{highfly_clock_lines, true};
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
