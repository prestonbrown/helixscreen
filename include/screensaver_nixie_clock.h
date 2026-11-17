// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"
#include "screensaver_clock.h"
#include "screensaver_clock_saver.h"
#include "screensaver_nixie_clock_sim.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief Nixie tube clock that counts a running print down
 *
 * helix::ui::NixieClockState decides what to show and helix::ui::NixieClockSim draws the tubes;
 * this class draws the AM/PM marker in the user's language on the first tube's socket. The tubes
 * are written pixel by pixel, but the marker is drawn with lv_draw_label, so the canvas takes
 * kLvDrawCanvasFormat for the reason it gives.
 */
class NixieClockScreensaver : public ClockSaver<NixieClockSim, NixieClockState> {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::NIXIE_CLOCK;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return kLvDrawCanvasFormat;
    }
    void draw_clock_text(const std::vector<DirtyRect>& under,
                         std::vector<DirtyRect>& dirty) override;
    void reset_clock_text() override;

  private:
    SaverLabel meridiem_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
