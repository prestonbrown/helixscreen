// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_nav_backdrop.h"

#include "ui_effects.h"
#include "ui_utils.h"

#include "backdrop_blur.h"
#include "lvgl/src/core/lv_obj_private.h"       // styles[]: no public getter
#include "lvgl/src/core/lv_obj_style_private.h" // is_trans: no public getter
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace helix::ui {

namespace {

/// The colors the navbar is painted in right now.
std::string active_palette_key() {
    const helix::ThemeData& theme = theme_manager_get_active_theme();
    const helix::ModePalette& palette = theme_manager_is_dark_mode() ? theme.dark : theme.light;
    std::string key;
    for (size_t i = 0; i < helix::ModePalette::color_names().size(); i++)
        key += palette.at(i);
    return key;
}

/// A snapshot backdrop is an image; a dim layer is a translucent plain object.
bool is_snapshot_backdrop(lv_obj_t* backdrop) {
    return backdrop && lv_obj_check_type(backdrop, &lv_image_class);
}

/// Longest transition, delay included, among the shown widgets on @p screen that
/// are mid-transition right now; 0 when none is.
uint32_t longest_running_transition(lv_obj_t* screen) {
    uint32_t longest = 0;
    lv_obj_tree_walk(
        screen,
        [](lv_obj_t* obj, void* user) {
            if (lv_obj_is_hidden(obj))
                return LV_OBJ_TREE_WALK_SKIP_CHILDREN;
            for (uint32_t i = 0; i < obj->style_cnt; i++) {
                if (!obj->styles[i].is_trans)
                    continue;
                if (const lv_style_transition_dsc_t* tr =
                        lv_obj_get_style_transition(obj, LV_PART_MAIN)) {
                    auto* out = static_cast<uint32_t*>(user);
                    *out = std::max(*out, tr->time + tr->delay);
                }
                break;
            }
            return LV_OBJ_TREE_WALK_NEXT;
        },
        &longest);
    return longest;
}

} // namespace

void OverlayBackdrop::adopt(lv_obj_t* screen, lv_obj_t* arriving, lv_event_cb_t click_cb) {
    {
        ScopedHideChrome chrome_hidden;
        const bool arriving_shown = arriving && !lv_obj_has_flag(arriving, LV_OBJ_FLAG_HIDDEN);
        if (arriving_shown) {
            lv_obj_add_flag(arriving, LV_OBJ_FLAG_HIDDEN);
        }
        primary_ = create_darkened_backdrop(screen, 40);
        palette_key_ = active_palette_key();
        if (arriving_shown) {
            lv_obj_remove_flag(arriving, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!primary_)
        return;

    bring_to_front(primary_);
    // PRESSED latches keyboard visibility before LVGL's click-focus
    // hides it; CLICKED consumes the tap for the keyboard dismiss.
    // DECLARATIVE_OK: the snapshot backdrop is created in C++, it has no XML layer.
    lv_obj_add_event_cb(primary_, click_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(primary_, click_cb, LV_EVENT_CLICKED, nullptr);

    cancel_settle_retake();
    if (!retake_ || !is_snapshot_backdrop(primary_))
        return;
    const uint32_t settle_ms = longest_running_transition(screen);
    if (settle_ms == 0)
        return;
    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<OverlayBackdrop*>(lv_timer_get_user_data(t));
            self->settle_timer_.release(); // a one-shot timer deletes itself
            self->retake_();
        },
        settle_ms, this);
    lv_timer_set_repeat_count(timer, 1);
    settle_timer_.reset(timer);
}

void OverlayBackdrop::cancel_settle_retake() {
    settle_timer_.reset();
}

void OverlayBackdrop::refresh(lv_obj_t* app_layout, lv_obj_t* base_panel) {
    if (!primary_)
        return;
    // A dim layer is translucent over the live navbar, so it is never stale.
    if (!is_snapshot_backdrop(primary_))
        return;

    lv_obj_t* screen = lv_obj_get_screen(primary_);
    if (!screen || screen != lv_screen_active())
        return;

    // Everything the snapshot must not contain: the overlays it sits under, the
    // backdrop itself, the printer-switch menu, anything else parked on
    // the screen. Hide them all and restore the exact flags afterwards — the
    // snapshot has to reproduce what the screen looked like at push time, not
    // what it looks like now.
    std::vector<std::pair<lv_obj_t*, bool>> saved;
    uint32_t child_count = lv_obj_get_child_count(screen);
    saved.reserve(child_count);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(screen, static_cast<int32_t>(i));
        if (!child || child == app_layout)
            continue;
        saved.emplace_back(child, lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN));
        lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
    }

    // push_overlay(hide_previous) hid the base panel behind the backdrop. It has
    // to be visible again for the snapshot or a narrower overlay (#1178) would
    // expose dimmed emptiness where the panel used to show through.
    bool base_was_hidden = false;
    if (base_panel) {
        base_was_hidden = lv_obj_has_flag(base_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(base_panel, LV_OBJ_FLAG_HIDDEN);
    }

    // Into the buffer the backdrop already owns: a second full frame is 768KB
    // on an 800x480 RGB565 panel.
    const bool retaken = retake_darkened_backdrop(primary_, 40);

    if (base_panel && base_was_hidden)
        lv_obj_add_flag(base_panel, LV_OBJ_FLAG_HIDDEN);
    for (auto& [child, was_hidden] : saved) {
        if (was_hidden)
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(child, LV_OBJ_FLAG_HIDDEN);
    }

    if (!retaken) {
        spdlog::warn("[NavigationManager] Backdrop refresh failed — keeping stale snapshot");
        return;
    }
    palette_key_ = active_palette_key();

    spdlog::debug("[NavigationManager] Overlay backdrop re-snapshotted");
}

bool OverlayBackdrop::primary_is_dim_layer() const {
    return primary_ && !is_snapshot_backdrop(primary_);
}

bool OverlayBackdrop::palette_changed() const {
    return active_palette_key() != palette_key_;
}

void OverlayBackdrop::release_primary() {
    cancel_settle_retake();
    if (primary_) {
        safe_delete_deferred(primary_);
        primary_ = nullptr;
    }
}

void OverlayBackdrop::delete_primary_now() {
    cancel_settle_retake();
    if (primary_) {
        lv_obj_del(primary_);
        primary_ = nullptr;
    }
}

void OverlayBackdrop::retire(lv_obj_t* overlay) {
    auto it = nested_.find(overlay);
    if (it != nested_.end()) {
        safe_delete_deferred(it->second);
        nested_.erase(it);
    }
}

void OverlayBackdrop::retire_all() {
    for (auto& [_, backdrop] : nested_) {
        safe_delete_deferred(backdrop);
    }
    nested_.clear();
}

void OverlayBackdrop::scrub(lv_obj_t* overlay) {
    nested_.erase(overlay);
}

void OverlayBackdrop::rekey(lv_obj_t* old_overlay, lv_obj_t* new_overlay) {
    auto it = nested_.find(old_overlay);
    if (it != nested_.end()) {
        lv_obj_t* backdrop = it->second;
        nested_.erase(it);
        nested_[new_overlay] = backdrop;
    }
}

bool OverlayBackdrop::take_keyboard_dismiss() {
    if (!press_keyboard_visible_) {
        return false;
    }
    press_keyboard_visible_ = false;
    return true;
}

void OverlayBackdrop::reset() {
    nested_.clear();
    delete_primary_now();
}

} // namespace helix::ui
