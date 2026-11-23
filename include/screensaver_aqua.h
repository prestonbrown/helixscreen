// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"
#include "screensaver_aqua_sim.h"
#include "screensaver_clock.h"
#include "screensaver_clock_saver.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief Aqua Surface: glassy water with a ring clock in it
 *
 * helix::ui::AquaSim draws the water, the rings and their digits; this class draws the date or
 * print line with lv_draw_label, so the canvas takes kLvDrawCanvasFormat.
 */
class AquaScreensaver : public ClockSaver<AquaSim> {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::AQUA;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return kLvDrawCanvasFormat;
    }
    void draw_clock_text(const std::vector<DirtyRect>& under,
                         std::vector<DirtyRect>& dirty) override;
    void reset_clock_text() override;

  private:
    friend class AquaScreensaverTestAccess;

    SaverLabel line_;
    SaverLabel meridiem_; ///< inside the top ring, in 12-hour mode
    /// The date or print line, joined from the clock's two lines.
    std::string text_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
