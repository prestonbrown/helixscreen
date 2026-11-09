// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "tour_steps.h"

#include <functional>
#include <lvgl.h>
#include <vector>

namespace helix::tour {

/// Renders the coach-mark overlay: dim layer, target highlight, tooltip.
/// Non-singleton — created per tour run, destroyed on skip/finish.
class TourOverlay {
  public:
    using AdvanceCb = std::function<void()>;
    using SkipCb = std::function<void()>;

    TourOverlay(std::vector<TourStep> steps, AdvanceCb on_next, SkipCb on_skip);
    ~TourOverlay();

    TourOverlay(const TourOverlay&) = delete;
    TourOverlay& operator=(const TourOverlay&) = delete;

    /// Show step at `index`. 0-based. Triggers target resolution + tooltip placement.
    void show_step(size_t index);

    /// Hide the card and highlight while the screen beneath changes.
    void hide_card();

    lv_obj_t* root() const {
        return root_;
    }

  private:
    void build_tree();
    void resolve_target(const TourStep& step, lv_area_t& out_rect, bool& out_has_target);
    void place_highlight(const lv_area_t& target_rect);
    void place_tooltip(const lv_area_t& target_rect, bool has_target, TooltipAnchor hint);
    void update_tooltip_text(const TourStep& step, size_t index, size_t total);
    void update_counter(size_t index, size_t total);
    void update_preview(const TourStep& step);

    static void on_skip_cb(lv_event_t* e);
    static void on_next_cb(lv_event_t* e);

    std::vector<TourStep> steps_;
    AdvanceCb on_next_cb_;
    SkipCb on_skip_cb_;

    lv_obj_t* root_ = nullptr; // on lv_layer_top()
    lv_obj_t* dim_ = nullptr;
    lv_obj_t* highlight_ = nullptr;
    lv_obj_t* tooltip_ = nullptr; // instantiated from tour_overlay.xml
    int tooltip_width_ = 0;
};

} // namespace helix::tour
