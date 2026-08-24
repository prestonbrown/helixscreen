// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tour_overlay.h"

#include "ui_button.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

namespace helix::tour {

namespace {
constexpr int HIGHLIGHT_OUTLINE_WIDTH = 3;
constexpr int TOOLTIP_MAX_WIDTH = 480;

// Per-screen dim fill with ~55% opacity.
constexpr uint8_t DIM_OPA = 140;

// Spacing tokens resolved at call time from the theme (responsive per breakpoint).
inline int highlight_outline_pad() {
    return theme_manager_get_spacing("space_xs");
}
inline int tooltip_margin() {
    return theme_manager_get_spacing("space_sm");
}
} // namespace

TourOverlay::TourOverlay(std::vector<TourStep> steps, AdvanceCb on_next, SkipCb on_skip)
    : steps_(std::move(steps)), on_next_cb_(std::move(on_next)), on_skip_cb_(std::move(on_skip)) {
    build_tree();
}

TourOverlay::~TourOverlay() {
    if (root_) {
        lv_obj_delete(root_); // full-screen overlay, not owned by any panel
        root_ = nullptr;
    }
}

void TourOverlay::build_tree() {
    lv_obj_t* top = lv_layer_top();

    // Absolute screen dimensions — avoids LV_PCT() sizing quirks when parent
    // is lv_layer_top() and layout hasn't resolved yet. See #<tour-overlay>.
    const int screen_w = lv_display_get_horizontal_resolution(nullptr);
    const int screen_h = lv_display_get_vertical_resolution(nullptr);

    root_ = lv_obj_create(top);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_size(root_, screen_w, screen_h);
    lv_obj_set_style_bg_opa(root_, 0, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    // Dim layer — click-blocking, absorbs all touches that miss the tooltip.
    dim_ = lv_obj_create(root_);
    lv_obj_set_pos(dim_, 0, 0);
    lv_obj_set_size(dim_, screen_w, screen_h);
    lv_obj_set_style_bg_color(dim_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim_, DIM_OPA, 0);
    lv_obj_set_style_border_width(dim_, 0, 0);
    lv_obj_set_style_pad_all(dim_, 0, 0);
    lv_obj_set_style_radius(dim_, 0, 0);
    lv_obj_add_flag(dim_, LV_OBJ_FLAG_CLICKABLE); // absorb clicks
    lv_obj_clear_flag(dim_, LV_OBJ_FLAG_SCROLLABLE);

    // Highlight rect — transparent fill, outline + shadow. Hidden until step has target.
    highlight_ = lv_obj_create(root_);
    lv_obj_set_style_bg_opa(highlight_, 0, 0);
    lv_obj_set_style_border_width(highlight_, 0, 0);
    lv_obj_set_style_outline_width(highlight_, HIGHLIGHT_OUTLINE_WIDTH, 0);
    lv_obj_set_style_outline_color(highlight_, theme_manager_get_color("primary"), 0);
    lv_obj_set_style_outline_pad(highlight_, highlight_outline_pad(), 0);
    lv_obj_set_style_shadow_width(highlight_, 24, 0);
    lv_obj_set_style_shadow_opa(highlight_, 180, 0);
    lv_obj_set_style_shadow_color(highlight_, theme_manager_get_color("primary"), 0);
    lv_obj_clear_flag(highlight_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(highlight_, LV_OBJ_FLAG_HIDDEN);

    // Tooltip — instantiated from XML component.
    tooltip_ = static_cast<lv_obj_t*>(lv_xml_create(root_, "tour_tooltip_card", nullptr));
    if (!tooltip_) {
        spdlog::error("[TourOverlay] Failed to instantiate tour_tooltip_card");
        return;
    }
    // Responsive tooltip width: ~55% of screen, clamped to [220, 480].
    // Covers AD5M (480 -> 264px) through Sonic Pad (1024 -> 480px).
    const int tooltip_w = std::clamp(screen_w * 55 / 100, 220, 480);
    lv_obj_set_width(tooltip_, tooltip_w);
    lv_obj_update_layout(tooltip_);

    // Find the skip/next buttons and attach static callbacks.
    // Exception: per-instance closure — XML event_cb can't pass `this` captures.
    // These buttons live and die with the overlay, so no static pointer hazards.
    lv_obj_t* skip_btn = lv_obj_find_by_name(tooltip_, "tour_skip_btn");
    lv_obj_t* next_btn = lv_obj_find_by_name(tooltip_, "tour_next_btn");
    if (skip_btn) {
        lv_obj_add_event_cb(skip_btn, on_skip_cb, LV_EVENT_CLICKED, this);
    }
    if (next_btn) {
        lv_obj_add_event_cb(next_btn, on_next_cb, LV_EVENT_CLICKED, this);
    }
}

void TourOverlay::on_skip_cb(lv_event_t* e) {
    auto* self = static_cast<TourOverlay*>(lv_event_get_user_data(e));
    if (!self || !self->on_skip_cb_)
        return;
    self->on_skip_cb_();
}

void TourOverlay::on_next_cb(lv_event_t* e) {
    auto* self = static_cast<TourOverlay*>(lv_event_get_user_data(e));
    if (!self || !self->on_next_cb_)
        return;
    self->on_next_cb_();
}

void TourOverlay::resolve_target(const TourStep& step, lv_area_t& out_rect, bool& out_has_target) {
    out_has_target = false;
    if (step.target_name.empty())
        return;

    // Search the entire screen tree (home panel root + navbar + anything on layer_top below us).
    lv_obj_t* scr = lv_screen_active();
    lv_obj_t* target = lv_obj_find_by_name(scr, step.target_name.c_str());
    if (!target) {
        spdlog::warn("[TourOverlay] Target '{}' not found — skipping highlight", step.target_name);
        return;
    }
    lv_obj_update_layout(target);
    lv_obj_get_coords(target, &out_rect);
    out_has_target = true;
}

void TourOverlay::place_highlight(const lv_area_t& target_rect) {
    const int pad = highlight_outline_pad();
    lv_obj_set_pos(highlight_, target_rect.x1 - pad, target_rect.y1 - pad);
    lv_obj_set_size(highlight_, (target_rect.x2 - target_rect.x1) + 2 * pad,
                    (target_rect.y2 - target_rect.y1) + 2 * pad);
    lv_obj_clear_flag(highlight_, LV_OBJ_FLAG_HIDDEN);
}

void TourOverlay::place_tooltip(const lv_area_t& target_rect, bool has_target, TooltipAnchor hint) {
    lv_obj_update_layout(tooltip_);
    const int screen_w = lv_display_get_horizontal_resolution(nullptr);
    const int screen_h = lv_display_get_vertical_resolution(nullptr);

    // Cap tooltip width responsively for small screens.
    const int max_w = std::min(TOOLTIP_MAX_WIDTH, screen_w - 32);
    lv_obj_set_style_max_width(tooltip_, max_w, 0);
    lv_obj_update_layout(tooltip_);
    const int tw = lv_obj_get_width(tooltip_);
    const int th = lv_obj_get_height(tooltip_);

    if (!has_target) {
        lv_obj_set_pos(tooltip_, (screen_w - tw) / 2, (screen_h - th) / 2);
        return;
    }

    const int tx1 = target_rect.x1;
    const int tx2 = target_rect.x2;
    const int ty1 = target_rect.y1;
    const int ty2 = target_rect.y2;

    auto clamp = [&](int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); };

    int x = 0, y = 0;
    auto set_below = [&] {
        x = clamp(tx1, 8, screen_w - tw - 8);
        y = ty2 + tooltip_margin();
    };
    auto set_above = [&] {
        x = clamp(tx1, 8, screen_w - tw - 8);
        y = ty1 - tooltip_margin() - th;
    };
    auto set_right = [&] {
        x = tx2 + tooltip_margin();
        y = clamp(ty1, 8, screen_h - th - 8);
    };
    auto set_left = [&] {
        x = tx1 - tooltip_margin() - tw;
        y = clamp(ty1, 8, screen_h - th - 8);
    };

    auto fits = [&](int xx, int yy) {
        return xx >= 0 && yy >= 0 && xx + tw <= screen_w && yy + th <= screen_h;
    };

    switch (hint) {
    case TooltipAnchor::PreferBelow:
        set_below();
        if (fits(x, y))
            break;
        [[fallthrough]];
    case TooltipAnchor::PreferAbove:
        set_above();
        if (fits(x, y))
            break;
        [[fallthrough]];
    case TooltipAnchor::PreferRight:
        set_right();
        if (fits(x, y))
            break;
        [[fallthrough]];
    case TooltipAnchor::PreferLeft:
        set_left();
        if (fits(x, y))
            break;
        [[fallthrough]];
    default:
        set_below();
    }

    // Final clamp for edge cases.
    x = clamp(x, 8, screen_w - tw - 8);
    y = clamp(y, 8, screen_h - th - 8);
    lv_obj_set_pos(tooltip_, x, y);
}

void TourOverlay::update_tooltip_text(const TourStep& step, size_t index, size_t total) {
    if (!tooltip_)
        return;
    lv_obj_t* title = lv_obj_find_by_name(tooltip_, "tour_title");
    lv_obj_t* body = lv_obj_find_by_name(tooltip_, "tour_body");
    lv_obj_t* next_btn = lv_obj_find_by_name(tooltip_, "tour_next_btn");

    if (title)
        lv_label_set_text(title, lv_tr(step.title_key.c_str()));
    if (body)
        lv_label_set_text(body, lv_tr(step.body_key.c_str()));

    // Change "Next" to "Done" on last step.
    if (next_btn) {
        const char* key = (index + 1 == total) ? "Done" : "Next";
        ui_button_set_text(next_btn, lv_tr(key));
    }

    update_counter(index, total);
}

void TourOverlay::update_counter(size_t index, size_t total) {
    lv_obj_t* counter = lv_obj_find_by_name(tooltip_, "tour_counter");
    if (!counter)
        return;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%zu / %zu", index + 1, total);
    lv_label_set_text(counter, buf);
}

void TourOverlay::show_step(size_t index) {
    if (index >= steps_.size())
        return;
    // Defensive: if build_tree() failed to instantiate the tooltip component
    // (e.g. XML not registered), every placement/update path below dereferences
    // tooltip_. Skip rather than crash.
    if (!tooltip_ || !highlight_)
        return;
    const TourStep& step = steps_[index];

    lv_area_t target_rect{};
    bool has_target = false;
    resolve_target(step, target_rect, has_target);

    if (has_target) {
        place_highlight(target_rect);
    } else {
        lv_obj_add_flag(highlight_, LV_OBJ_FLAG_HIDDEN);
    }

    update_tooltip_text(step, index, steps_.size());
    place_tooltip(target_rect, has_target, step.anchor_hint);
}

} // namespace helix::tour
