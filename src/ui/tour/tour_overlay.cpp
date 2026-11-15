// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tour_overlay.h"

#include "printer_image_manager.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace helix::tour {

namespace {
constexpr int kHighlightOutlineWidth = 3;
constexpr int kTooltipMaxWidth = 480;

// Per-screen dim fill with ~55% opacity.
constexpr uint8_t kDimOpa = 140;

// A preview would push the card off a screen shorter than this.
constexpr int kMinScreenHeightForPreview = 400;

constexpr int kTooltipMinWidth = 220;

// Spacing tokens resolved at call time from the theme (responsive per breakpoint).
inline int highlight_outline_pad() {
    return theme_manager_get_spacing("space_xs");
}
inline int tooltip_margin() {
    return theme_manager_get_spacing("space_sm");
}

/// The overlay a card button belongs to: its root sits on lv_layer_top() and
/// carries the overlay as user data.
TourOverlay* overlay_of(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    while (obj && lv_obj_get_parent(obj) != lv_layer_top()) {
        obj = lv_obj_get_parent(obj);
    }
    return obj ? static_cast<TourOverlay*>(lv_obj_get_user_data(obj)) : nullptr;
}
} // namespace

TourOverlay::TourOverlay(std::vector<TourStep> steps, AdvanceCb on_next, SkipCb on_skip)
    : steps_(std::move(steps)), on_next_cb_(std::move(on_next)), on_skip_cb_(std::move(on_skip)) {
    lv_subject_init_string(&title_subject_, title_buf_, nullptr, sizeof(title_buf_), "");
    lv_subject_init_string(&body_subject_, body_buf_, nullptr, sizeof(body_buf_), "");
    lv_subject_init_string(&counter_subject_, counter_buf_, nullptr, sizeof(counter_buf_), "");
    lv_subject_init_string(&next_text_subject_, next_text_buf_, nullptr, sizeof(next_text_buf_),
                           "");
    subjects_.publish("tour_title", &title_subject_);
    subjects_.publish("tour_body", &body_subject_);
    subjects_.publish("tour_counter", &counter_subject_);
    subjects_.publish("tour_next_text", &next_text_subject_);
    lv_xml_register_event_cb(nullptr, "on_tour_skip", on_skip_cb);
    lv_xml_register_event_cb(nullptr, "on_tour_next", on_next_cb);
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
    lv_obj_set_user_data(root_, this); // the card buttons find their overlay here
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_size(root_, screen_w, screen_h);
    lv_obj_set_style_bg_opa(root_, 0, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_scrollable(root_, false);
    lv_obj_set_click_focusable(root_, false);

    // Dim layer — click-blocking, absorbs all touches that miss the tooltip.
    dim_ = lv_obj_create(root_);
    lv_obj_set_pos(dim_, 0, 0);
    lv_obj_set_size(dim_, screen_w, screen_h);
    lv_obj_set_style_bg_color(dim_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim_, kDimOpa, 0);
    lv_obj_set_style_border_width(dim_, 0, 0);
    lv_obj_set_style_pad_all(dim_, 0, 0);
    lv_obj_set_style_radius(dim_, 0, 0);
    lv_obj_set_clickable(dim_, true); // absorb clicks
    lv_obj_set_scrollable(dim_, false);

    // Highlight rect — transparent fill, outline + shadow. Hidden until step has target.
    highlight_ = lv_obj_create(root_);
    lv_obj_set_style_bg_opa(highlight_, 0, 0);
    lv_obj_set_style_border_width(highlight_, 0, 0);
    lv_obj_set_style_outline_width(highlight_, kHighlightOutlineWidth, 0);
    lv_obj_set_style_outline_color(highlight_, theme_manager_get_color("primary"), 0);
    lv_obj_set_style_outline_pad(highlight_, highlight_outline_pad(), 0);
    lv_obj_set_style_shadow_width(highlight_, 24, 0);
    lv_obj_set_style_shadow_opa(highlight_, 180, 0);
    lv_obj_set_style_shadow_color(highlight_, theme_manager_get_color("primary"), 0);
    lv_obj_set_clickable(highlight_, false);
    lv_obj_set_hidden(highlight_, true);

    // Tooltip — instantiated from XML component.
    tooltip_ = static_cast<lv_obj_t*>(lv_xml_create(root_, "tour_tooltip_card", nullptr));
    if (!tooltip_) {
        spdlog::error("[TourOverlay] Failed to instantiate tour_tooltip_card");
        return;
    }
    // Responsive tooltip width: ~55% of screen, clamped to [220, 480].
    // Covers AD5M (480 -> 264px) through Sonic Pad (1024 -> 480px).
    tooltip_width_ = std::clamp(screen_w * 55 / 100, kTooltipMinWidth, kTooltipMaxWidth);
    lv_obj_set_width(tooltip_, tooltip_width_);
    lv_obj_update_layout(tooltip_);
}

void TourOverlay::on_skip_cb(lv_event_t* e) {
    TourOverlay* self = overlay_of(e);
    if (!self || !self->on_skip_cb_)
        return;
    self->on_skip_cb_();
}

void TourOverlay::on_next_cb(lv_event_t* e) {
    TourOverlay* self = overlay_of(e);
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
    if (!target && !step.fallback_target_name.empty()) {
        target = lv_obj_find_by_name(scr, step.fallback_target_name.c_str());
    }
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
    lv_obj_set_hidden(highlight_, false);
}

void TourOverlay::place_tooltip(const lv_area_t& target_rect, bool has_target, TooltipAnchor hint) {
    lv_obj_update_layout(tooltip_);
    const int screen_w = lv_display_get_horizontal_resolution(nullptr);
    const int screen_h = lv_display_get_vertical_resolution(nullptr);

    int width = tooltip_width_;
    if (has_target && hint == TooltipAnchor::LeftNarrowed) {
        const int room = target_rect.x1 - tooltip_margin() - 8;
        if (room >= kTooltipMinWidth) {
            width = std::min(room, tooltip_width_);
        } else {
            hint = TooltipAnchor::PreferAbove; // a portrait layout leaves nothing to the left
        }
    }
    lv_obj_set_width(tooltip_, width);

    // Cap tooltip width responsively for small screens.
    const int max_w = std::min(kTooltipMaxWidth, screen_w - 32);
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
    case TooltipAnchor::LeftNarrowed:
        set_left();
        break;
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
    lv_subject_set_string(&title_subject_, lv_tr(step.title_key.c_str()));
    lv_subject_set_string(&body_subject_, lv_tr(step.body_key.c_str()));
    // "Next" reads "Done" on the last step.
    lv_subject_set_string(&next_text_subject_, lv_tr((index + 1 == total) ? "Done" : "Next"));
    update_counter(index, total);
}

void TourOverlay::update_counter(size_t index, size_t total) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%zu / %zu", index + 1, total);
    lv_subject_set_string(&counter_subject_, buf);
}

void TourOverlay::update_preview(const TourStep& step) {
    lv_obj_t* preview = helix::ui::find_required(tooltip_, "tour_preview", "TourOverlay");
    if (!preview)
        return;
    const int screen_h = lv_display_get_vertical_resolution(nullptr);
    if (!step.shows_print_status_preview || screen_h < kMinScreenHeightForPreview) {
        lv_obj_set_hidden(preview, true); // DECLARATIVE_OK: per-step, C++-built card
        return;
    }
    const std::string image = helix::PrinterImageManager::instance().get_displayed_image_path(
        lv_display_get_horizontal_resolution(nullptr));
    lv_obj_t* printer = helix::ui::find_required(tooltip_, "tour_preview_printer", "TourOverlay");
    if (printer && !image.empty()) {
        lv_image_set_src(printer, image.c_str());
    }
    lv_obj_update_layout(tooltip_);
    // The real screen's 5:3 shape, at whatever width the card has on this display.
    lv_obj_set_height(preview, lv_obj_get_content_width(tooltip_) * 3 / 5);
    lv_obj_set_hidden(preview, false); // DECLARATIVE_OK: per-step, C++-built card
}

void TourOverlay::hide_card() {
    if (!tooltip_ || !highlight_)
        return;
    lv_obj_set_hidden(tooltip_, true);   // DECLARATIVE_OK: C++-built overlay
    lv_obj_set_hidden(highlight_, true); // DECLARATIVE_OK: C++-built overlay
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
        lv_obj_set_hidden(highlight_, true);
    }

    lv_obj_set_hidden(tooltip_, false); // DECLARATIVE_OK: C++-built overlay
    update_tooltip_text(step, index, steps_.size());
    update_preview(step);
    place_tooltip(target_rect, has_target, step.anchor_hint);
}

} // namespace helix::tour
