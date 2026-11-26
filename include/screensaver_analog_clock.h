// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "ui_widget_ref.h"

#include "screensaver.h"
#include "screensaver_analog_clock_sim.h"
#include "screensaver_clock_reading.h"
#include "screensaver_sim_saver.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <string>
#include <vector>

namespace helix::ui {

/// The Clock Face setting's Shuffle choice, after the faces: a different face each run.
inline constexpr int kAnalogFaceShuffle = static_cast<int>(kAnalogStyleCount);

/// The face a run shows for the Clock Face setting `setting` (a face, or Shuffle), given the
/// face the last run showed (`last`, or -1) and a random draw `draw`. Shuffle, and a face
/// picked for the Random screensaver (`random_saver`), never repeats the last one.
AnalogStyle analog_style_for_run(int setting, bool random_saver, int last, uint32_t draw);

/// The words `text` says at `reading`, in the user's language.
std::string analog_text_for(const AnalogText& text, const ClockReading& reading);

/// Writes `sim`'s texts at `reading` into the sim's dial, through `text_canvas` (a hidden canvas
/// that `buf` is pointed at the dial for), so the hands pass over the text. Every host of an
/// analog face calls this after the sim draws its dial again. With `drop_unfit`, a text even the
/// smallest font cannot fit in its box is left out rather than drawn past it: a small dial in a
/// home tile.
void analog_write_dial_texts(lv_obj_t* text_canvas, lv_draw_buf_t& buf, const AnalogClockSim& sim,
                             const ClockReading& reading, bool drop_unfit = false);

/**
 * @brief An analog clock with five faces that shows a running print its own way
 *
 * helix::ui::AnalogClockSim draws the faces; this class reads the time and the print, picks the
 * face from the Clock Face setting, and writes the dial's text in the user's language into the
 * sim's cached dial whenever the sim draws it again, so the hands pass over the text.
 */
class AnalogClockScreensaver : public SimSaver<AnalogClockSim> {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::ANALOG_CLOCK;
    }
    const ClockState& clock() const {
        return clock_;
    }
    AnalogStyle style() const {
        return style_;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return kLvDrawCanvasFormat;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) override;
    void on_stop() override;

  private:
    AnalogFace face() const {
        return AnalogFace{style_, clock_.reading()};
    }
    /// Writes the sim's texts into its dial.
    void write_texts();

    ClockState clock_;
    AnalogStyle style_ = AnalogStyle::CHRONOGRAPH;
    int last_style_ = -1;
    /// A hidden canvas over the sim's dial, so LVGL can draw text into it.
    WidgetRef dial_canvas_;
    lv_draw_buf_t dial_buf_{};
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
