// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_canvas.h"
#include "screensaver_fireflies_sim.h"
#include "screensaver_sim_saver.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <vector>

namespace helix::ui {

/**
 * @brief Coloured swarms of fireflies chasing invisible baits, after Fireflies
 *
 * helix::ui::FirefliesSim draws into the canvas at the display's own depth, RGB565 or
 * XRGB8888, and reports the box it cleared and drew. A level request applies at once. A
 * finished print spells "DONE" where the print reading spells states and holds a check mark
 * otherwise.
 */
class FirefliesScreensaver : public SimSaver<FirefliesSim> {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::FIREFLIES;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return kSaverBuildCanvasFormat;
    }
    /// Steps the sim and logs each mode the swarm begins.
    void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) override;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
