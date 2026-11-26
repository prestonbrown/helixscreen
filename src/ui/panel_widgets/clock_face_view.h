// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "ui_widget_ref.h"

#include "core/timer_guard.h"
#include "screensaver_analog_clock_sim.h"
#include "screensaver_canvas.h"
#include "screensaver_card_markers.h"
#include "screensaver_clock_reading.h"
#include "screensaver_flip_clock_sim.h"
#include "screensaver_nixie_clock_sim.h"
#include "src/ui/panel_widgets/clock_face.h"

#include <cstddef>
#include <cstdint>
#include <lvgl.h>
#include <memory>
#include <vector>

namespace helix {

/**
 * @brief One clock face painted on a canvas inside a home tile
 *
 * The clock savers' own painters (AnalogClockSim, FlipClockSim, NixieClockSim) on a canvas the
 * size of the tile, held still (no drift), with no print on it. The canvas, the sim's buffers
 * and the timer exist only between show() and hide(); a hidden view holds nothing.
 *
 * The timer repaints at the tempo's period while something moves (a seconds hand, a flip, a
 * fade) and otherwise sleeps to the next whole minute. Main thread only.
 */
class ClockFaceView {
  public:
    ClockFaceView() = default;
    ~ClockFaceView();
    ClockFaceView(const ClockFaceView&) = delete;
    ClockFaceView& operator=(const ClockFaceView&) = delete;

    /**
     * @brief Shows `face` on a `w` x `h` canvas created in `parent`, painted for `in`
     *
     * Replaces whatever showed. `backdrop` is the color the face sits on (the card behind
     * it), painted round the dial or cards so no square shows. With `run_timer` false the face
     * repaints only through step() or resume(): a still preview, or a tile not on screen.
     * False, showing nothing, for Text or when the canvas cannot be allocated.
     */
    bool show(lv_obj_t* parent, ClockFace face, int32_t w, int32_t h, const ClockFaceTempo& tempo,
              const ui::ClockInput& in, ui::Rgb backdrop, bool run_timer = true);

    /// Moves the face on by `dt_ms` to `in` and repaints what changed. True while something
    /// still moves and wants the next frame at the tempo's period.
    bool step(uint32_t dt_ms, const ui::ClockInput& in);

    /// Stops the timer and frees the canvas, its objects and the sim's buffers.
    void hide();

    /// hide() for a view whose canvas is being deleted with its parent right now (inside the
    /// parent's LV_EVENT_DELETE): frees the buffers and the sim and leaves the objects to the
    /// deletion already under way.
    void release_dying();

    /// Stops the timer and keeps the canvas and the sim, so resume() needs no rebuild. Nothing
    /// is painted while paused.
    void pause();

    /// Brings a paused face up to `in`, repaints what changed and starts the timer again.
    void resume(const ui::ClockInput& in);

    /// Showing with no timer running: paused, or a still preview.
    bool paused() const {
        return showing() && !timer_;
    }

    bool showing() const {
        return face_ != ClockFace::TEXT;
    }
    ClockFace face() const {
        return face_;
    }
    const ClockFaceTempo& tempo() const {
        return tempo_;
    }
    /// The color show() painted round the face.
    ui::Rgb backdrop() const {
        return backdrop_;
    }
    int32_t width() const {
        return canvas_.width();
    }
    int32_t height() const {
        return canvas_.height();
    }
    lv_obj_t* canvas_obj() const {
        return canvas_ref_.get();
    }
    lv_timer_t* timer() const {
        return timer_.get();
    }
    /// Bytes held: the canvas buffer and the sim's caches.
    size_t heap_bytes() const;

  private:
    static void timer_cb(lv_timer_t* timer);
    /// Arms the timer for the next frame: the tempo's period while `moving`, else the next
    /// whole minute.
    void schedule(bool moving);
    void write_analog_texts();
    /// Makes the surround inside `areas` transparent on a canvas with alpha (keyed_).
    void clear_surround(const std::vector<ui::DirtyRect>& areas);
    ui::AnalogFace analog_face() const;

    ClockFace face_ = ClockFace::TEXT;
    ClockFaceTempo tempo_;
    ui::Rgb backdrop_{};
    /// The surround's key on a canvas with alpha; false on RGB565.
    bool keyed_ = false;
    ui::Rgb key_{};
    ui::SaverCanvas canvas_;
    helix::ui::WidgetRef canvas_ref_;
    /// A hidden canvas over the analog dial, so LVGL can write the dial's text into it.
    helix::ui::WidgetRef text_canvas_;
    lv_draw_buf_t text_buf_{};

    ui::ClockState clock_;
    ui::FlipClockState cards_;
    std::unique_ptr<ui::AnalogClockSim> analog_;
    std::unique_ptr<ui::FlipClockSim> flip_;
    std::unique_ptr<ui::NixieClockSim> nixie_;
    ui::CardClockMarkers markers_;
    std::vector<ui::DirtyRect> dirty_;
    std::vector<ui::DirtyRect> under_;

    ui::LvglTimerGuard timer_;
    uint32_t last_tick_ = 0;
};

} // namespace helix

#endif // HELIX_ENABLE_SCREENSAVER
