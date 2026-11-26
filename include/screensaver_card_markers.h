// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_canvas.h"
#include "screensaver_clock.h"
#include "screensaver_flip_clock_sim.h"
#include "screensaver_frame.h"
#include "screensaver_nixie_clock_sim.h"

#include <lvgl.h>
#include <string>
#include <utility>
#include <vector>

namespace helix::ui {

/**
 * @brief The AM/PM markers a card clock keeps over its cards
 *
 * The flip clock's cards and the nixie clock's tubes are written pixel by pixel; their AM/PM is
 * lv_draw_label text on the canvas: the main row's on the first card or tube, the small row's
 * after its time. The flip and nixie savers and the home tile's faces all draw them here.
 */
class CardClockMarkers {
  public:
    /**
     * @brief Draws the markers for `face` where they changed or a box in `under` crossed them
     *
     * `box` is where the main marker goes, in `color` aligned by `align`. When the main marker
     * changes over a first card the frame did not repaint, `redraw_first(frame, dirty)` paints
     * that card again so the old text goes. What is drawn is added to `dirty`.
     */
    template <typename RedrawFirst>
    void draw(SaverCanvas& canvas, const FlipLayout& layout, const FlipFace& face,
              const DirtyRect& box, Rgb color, lv_text_align_t align, RedrawFirst&& redraw_first,
              const std::vector<DirtyRect>& under, std::vector<DirtyRect>& dirty) {
        // The small row's marker changes only with a relayout, which clears the space beneath it.
        sub_.draw(canvas,
                  shown_text(clock_meridiem_text(layout.sub_meridiem), layout.sub_meridiem_box),
                  layout.sub_meridiem_box, color, LV_OPA_COVER, LV_TEXT_ALIGN_LEFT, under, dirty);

        const std::string text = shown_text(clock_meridiem_text(face.meridiem), box);
        if (!main_.needs_draw(text, box, under)) {
            return;
        }
        // The old marker goes first, unless the frame just repainted the card beneath it.
        const bool repainted = main_.needs_draw(main_.text(), main_.box(), under);
        if (!main_.text().empty() && !repainted) {
            FrameTarget frame = canvas.frame();
            std::forward<RedrawFirst>(redraw_first)(frame, dirty);
        }
        main_.draw(canvas, text, box, color, LV_OPA_COVER, align, under, dirty);
    }

    /// The flip clock's markers: dimmer than the digits, so the cards stay the subject; the main
    /// one in the first card's top-left corner.
    void draw_flip(SaverCanvas& canvas, const FlipClockSim& sim, const FlipFace& face,
                   const std::vector<DirtyRect>& under, std::vector<DirtyRect>& dirty) {
        draw(
            canvas, sim.layout(), face, sim.layout().meridiem, kFlipInk, LV_TEXT_ALIGN_LEFT,
            [&sim](FrameTarget& frame, std::vector<DirtyRect>& d) { sim.redraw_card(frame, 0, d); },
            under, dirty);
    }

    /// The nixie clock's markers: a pale engraving on the dark socket, dimmer than the glow; the
    /// main one centered on the first tube's socket.
    void draw_nixie(SaverCanvas& canvas, const NixieClockSim& sim, const FlipFace& face,
                    const std::vector<DirtyRect>& under, std::vector<DirtyRect>& dirty) {
        draw(
            canvas, sim.layout(), face, sim.meridiem_box(), kNixieInk, LV_TEXT_ALIGN_CENTER,
            [&sim](FrameTarget& frame, std::vector<DirtyRect>& d) { sim.redraw_tube(frame, 0, d); },
            under, dirty);
    }

    /// Leaves out a marker no font fits in its box, rather than drawing it past the box: cards
    /// in a home tile. The savers keep every marker.
    void set_drop_unfit(bool drop) {
        drop_unfit_ = drop;
    }

    /// Forgets what was drawn, so the next draw() draws both.
    void reset() {
        main_.reset();
        sub_.reset();
    }

  private:
    std::string shown_text(std::string text, const DirtyRect& box) const {
        if (!drop_unfit_ || text.empty() || box.empty()) {
            return text;
        }
        const int32_t w = box.x2 - box.x1 + 1;
        const int32_t h = box.y2 - box.y1 + 1;
        return clock_text_fits(clock_font_fitting(h, w, text.c_str()), text.c_str(), w, h)
                   ? text
                   : std::string{};
    }

    static constexpr Rgb kFlipInk{150, 150, 150};
    static constexpr Rgb kNixieInk{170, 150, 130};

    SaverLabel main_;
    SaverLabel sub_;
    bool drop_unfit_ = false;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
