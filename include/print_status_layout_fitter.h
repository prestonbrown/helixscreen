// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file print_status_layout_fitter.h
 * @brief Measured arrangement of the print-status panel's three regions.
 *
 * The job tile, the printer tile and the strip share `overlay_content`, and how
 * they share it depends on the pixels that box measures, which no style
 * attribute can express. This measures the box, asks
 * decide_print_status_tile_layout(), or decide_print_status_ultrawide_layout()
 * while `ui_is_ultrawide` is 1, with the `has_filament_system_or_multi_tool`
 * gate, and publishes the answer as subjects the XML binds to.
 *
 * @threading Main thread only.
 */

#pragma once

#include "lvgl.h"
#include "print_status_tile_layout.h"

#include <functional>
#include <string>

namespace helix::ui {

class PrintStatusLayoutFitter {
  public:
    struct Subjects {
        lv_subject_t* arrangement;       ///< PrintStatusArrangement int
        lv_subject_t* strip_stacked;     ///< 1 = progress above the buttons
        lv_subject_t* checklist_compact; ///< 1 = current step + dots
        // Ultrawide column widths in px (PrintStatusColumns), bound as widths.
        lv_subject_t* preview_w = nullptr;
        lv_subject_t* printer_w = nullptr;
        lv_subject_t* readouts_w = nullptr;
        lv_subject_t* actions_w = nullptr;
        // Where the lane tray shows (PrintStatusLaneTray int) and the stacked
        // strip's height in px.
        lv_subject_t* lane_tray = nullptr;
        lv_subject_t* lane_tray_h = nullptr;
        /// The stacked printer tile's height beside a lane tray strip, in px;
        /// lv_pct(100) otherwise, where its flex share sets it.
        lv_subject_t* printer_tile_h = nullptr;
    };

    struct Host {
        std::function<lv_obj_t*()> root;
        std::function<bool()> subjects_ready;
        /// PrintStatusTiles::full_callouts_min_tile_h(); optional, 0 when unknown.
        std::function<int32_t()> printer_floor_h;
    };

    PrintStatusLayoutFitter(std::string log_tag, Subjects subjects, Host host);

    PrintStatusLayoutFitter(const PrintStatusLayoutFitter&) = delete;
    PrintStatusLayoutFitter& operator=(const PrintStatusLayoutFitter&) = delete;

    /// Measure `overlay_content`, publish the arrangement and return it. A box
    /// not laid out yet (a hidden panel measures 0) publishes nothing and
    /// returns the last decision.
    const PrintStatusTileLayout& recompute();

    [[nodiscard]] const PrintStatusTileLayout& last() const {
        return last_;
    }

    /// Strip heights: progress text beside the buttons, and progress text
    /// above a row of buttons. `progress` is the strip's laid-out progress
    /// block, whose rows are measured; without one the rows are estimated
    /// from the current tier's fonts.
    [[nodiscard]] static int32_t strip_row_height(lv_obj_t* progress = nullptr);
    [[nodiscard]] static int32_t strip_stacked_height(lv_obj_t* progress = nullptr);

  private:
    std::string log_tag_;
    Subjects subjects_;
    Host host_;
    PrintStatusTileLayout last_;
};

} // namespace helix::ui
