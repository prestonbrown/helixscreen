// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Puts one printer picture's callouts on screen: measures each chip, asks
// compute_callout_layout() where everything goes, then positions the image,
// chips, leader lines and glows. The home printer-image widget and the print
// status printer tile each own one, so both draw from the same engine.

#include "callout_layout.h"
#include "printer_image_regions.h"

#include <array>
#include <lvgl.h>
#include <optional>
#include <vector>

namespace helix::ui {

class CalloutPresenter {
  public:
    struct Chip {
        CalloutKind kind = CalloutKind::Nozzle;
        lv_obj_t* chip = nullptr; ///< an activity_chip; its width is measured here
        lv_obj_t* line = nullptr; ///< a leader_line, or nullptr for a chip that never has one
        lv_obj_t* glow = nullptr; ///< its region glow, or nullptr
        bool in_budget = false;   ///< this printer can ever show it: decides the mode
        bool active = false;      ///< showing now
        bool expendable = false; ///< ChipContent::expendable: the live reading is an expected value
        /// May the layout shed this chip's room in this phase. Decides the MODE, so it
        /// must not follow live state; the tile sets it from phase_budget_expendable().
        /// A surface whose budget is static anyway (home) leaves both flags false.
        bool budget_expendable = false;
        bool glowing = false; ///< ChipContent::glow
        /// Widths the caller measured in the fonts the chip renders, including padding and
        /// comfort margin: the worst case it can show (decides the mode), what it shows now,
        /// and its compact form for pinned mode.
        int w_budget = 0;
        int w_full = 0;
        int w_compact = 0;
        /// w_budget and w_full with the chip's label left off; 0 for a chip without one.
        int w_budget_unlabeled = 0;
        int w_full_unlabeled = 0;
        int icons = 0; ///< pixels its icons take in the chip row; bounds the label
        /// Heights of a chip taller than one text line, full-size and compact,
        /// measured like the widths; 0 is Config's chip_h / compact_chip_h.
        int h = 0;
        int h_compact = 0;
    };

    struct Parts {
        lv_obj_t* area = nullptr;  ///< the container whose content box the layout fills
        lv_obj_t* image = nullptr; ///< the printer picture
        std::vector<Chip> chips;
        std::optional<Chip>
            toolhead; ///< the merged nozzle + part-fan chip, when the screen has one
    };

    struct Config {
        bool single_cell = false;
        int chip_h = 0;              ///< a full-size chip's height
        int compact_chip_h = 0;      ///< a pinned (compact) chip's height
        int gap = 0;                 ///< CalloutLayoutInput::gap
        int min_line = 0;            ///< CalloutLayoutInput::min_line
        int leader_min_distance = 0; ///< CalloutLayoutInput::leader_min_distance
    };

    /// Decide, then position. `regions` is nullptr for an untagged image. Returns the
    /// layout so the owner can publish the mode subject. Chip texts are the caller's
    /// to publish, so the widths in `parts` are for the text as it stands.
    CalloutLayout apply(const Parts& parts, const ImageRegions* regions, int natural_w,
                        int natural_h, const Config& config);

    /// Moves and sizes `image` to `moved`, or back to filling its container when null.
    /// True when the rect changed, so the owner can refresh the image for its new size.
    bool place_image(lv_obj_t* image, const CalloutRect* moved);

    /// Whether the last apply() moved the image.
    [[nodiscard]] bool image_changed() const {
        return image_changed_;
    }

  private:
    void place_chip(const CalloutChipOut& c, const Chip& part, bool compact);
    void place_line(const CalloutChipOut& c, const Chip& part);
    void place_glow(const Chip& part, const ImageRegions& r, const CalloutRect& image);

    /// lv_line keeps the pointer it is given, so each kind's points live here.
    std::array<std::array<lv_point_precise_t, 3>, kCalloutKindCount> line_pts_{};
    bool image_changed_ = false;
    /// Each glow's last geometry, so a relayout that leaves it in place does
    /// not re-render its mask.
    std::array<uint32_t, kCalloutKindCount> glow_key_{};
};

} // namespace helix::ui
