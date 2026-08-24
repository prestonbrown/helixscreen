// SPDX-License-Identifier: GPL-3.0-or-later
#include "page_scroll_controller.h"

#include "display_settings_manager.h"
#include "page_scroll_math.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

PageScrollController::~PageScrollController() {
    // If the container is still alive we own the gutter + mutations; clean up.
    if (container_ != nullptr) {
        detach();
    }
}

bool PageScrollController::attach(lv_obj_t* container) {
    if (container == nullptr) {
        return false;
    }
    if (attached()) {
        // Double-attach without an intervening detach() would orphan the
        // previous gutter + its still-registered event callbacks. One-shot
        // usage is the intended contract — reject instead of silently
        // leaking.
        spdlog::error("[PageScroll] attach() called while already attached; call detach() first");
        return false;
    }
    container_ = container;
    saved_pad_right_ = lv_obj_get_style_pad_right(container_, LV_PART_MAIN);
    saved_scrollbar_mode_ = lv_obj_get_scrollbar_mode(container_);

    // Build the chevron column as a floating child of the container.
    gutter_ = static_cast<lv_obj_t*>(lv_xml_create(container_, "page_scroll_gutter", nullptr));
    if (gutter_ == nullptr) {
        spdlog::error("[PageScroll] failed to create page_scroll_gutter component");
        container_ = nullptr;
        return false;
    }
    up_btn_ = lv_obj_find_by_name(gutter_, "up");
    down_btn_ = lv_obj_find_by_name(gutter_, "down");
    if (up_btn_ == nullptr || down_btn_ == nullptr) {
        spdlog::error("[PageScroll] page_scroll_gutter is missing its up/down buttons");
        lv_obj_delete_async(gutter_);
        gutter_ = nullptr;
        up_btn_ = nullptr;
        down_btn_ = nullptr;
        container_ = nullptr;
        return false;
    }

    // Measure the gutter's natural width to size the reserved strip.
    lv_obj_update_layout(gutter_);
    gutter_w_ = lv_obj_get_width(gutter_);
    if (gutter_w_ <= 0) {
        gutter_w_ = lv_obj_get_height(gutter_); // square button fallback
    }

    // Suppress the native scrollbar while the gutter is active.
    lv_obj_set_scrollbar_mode(container_, LV_SCROLLBAR_MODE_OFF);

    // Wire per-instance handlers (runtime control — see header note).
    lv_obj_add_event_cb(up_btn_, up_clicked_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(down_btn_, down_clicked_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(container_, container_event_cb, LV_EVENT_SCROLL, this);
    lv_obj_add_event_cb(container_, container_event_cb, LV_EVENT_SIZE_CHANGED, this);
    lv_obj_add_event_cb(container_, container_event_cb, LV_EVENT_DELETE, this);
    // The gutter is a child of the container, so a repopulate that clears the
    // container's children (lv_obj_clean) while the container survives destroys the
    // gutter without firing the container's own LV_EVENT_DELETE. Watch the gutter's
    // deletion directly so gutter_ can never dangle into refresh_reach_state (#1123).
    lv_obj_add_event_cb(gutter_, gutter_event_cb, LV_EVENT_DELETE, this);

    refresh_reach_state();
    return true;
}

void PageScrollController::apply_reserved_padding() {
    if (pad_applied_ || container_ == nullptr) {
        return;
    }
    lv_obj_set_style_pad_right(container_, saved_pad_right_ + gutter_w_, LV_PART_MAIN);
    // Push the floating gutter into the freed strip (align RIGHT_MID anchors to
    // the padded content edge; translate back out over the reserved band).
    lv_obj_set_style_translate_x(gutter_, gutter_w_, LV_PART_MAIN);
    pad_applied_ = true;
}

void PageScrollController::remove_reserved_padding() {
    if (!pad_applied_ || container_ == nullptr) {
        return;
    }
    lv_obj_set_style_pad_right(container_, saved_pad_right_, LV_PART_MAIN);
    pad_applied_ = false;
}

void PageScrollController::refresh_reach_state() {
    if (container_ == nullptr || gutter_ == nullptr) {
        return;
    }
    PageScrollReach reach = page_scroll_compute_reach(lv_obj_get_scroll_y(container_),
                                                      lv_obj_get_scroll_bottom(container_));
    if (!reach.has_overflow) {
        lv_obj_add_flag(gutter_, LV_OBJ_FLAG_HIDDEN);
        remove_reserved_padding();
        return;
    }
    apply_reserved_padding();
    lv_obj_remove_flag(gutter_, LV_OBJ_FLAG_HIDDEN);

    auto set_disabled = [](lv_obj_t* btn, bool disabled) {
        if (disabled) {
            lv_obj_add_state(btn, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(btn, LV_STATE_DISABLED);
        }
    };
    set_disabled(up_btn_, !reach.can_up);
    set_disabled(down_btn_, !reach.can_down);
}

void PageScrollController::scroll_by_page(int32_t direction) {
    if (container_ == nullptr) {
        return;
    }
    int32_t step = page_scroll_step(lv_obj_get_content_height(container_));
    // Animate the page scroll when the user's Animations setting is on; otherwise
    // jump instantly. reach-state is recomputed from the SCROLL event as the
    // animation runs, so the dim/disable end-states stay correct either way.
    lv_anim_enable_t anim = helix::DisplaySettingsManager::instance().get_animations_enabled()
                                ? LV_ANIM_ON
                                : LV_ANIM_OFF;
    // Bounded, not lv_obj_scroll_by(): the last page has less than a full step of
    // room left, and an unbounded delta parks the view past the content end, leaving
    // a sliver of content up top and dead space filling the rest. The bounded variant
    // clamps against scroll_top + scroll_bottom, so a full step still moves a full
    // page and only the final one is shortened to land on the content edge. It also
    // makes a press at either end a no-op, which matters because LV_STATE_DISABLED
    // does not suppress LV_EVENT_CLICKED on the dimmed chevron.
    lv_obj_scroll_by_bounded(container_, 0, -direction * step, anim);
    refresh_reach_state();
}

void PageScrollController::page_up() {
    scroll_by_page(-1);
}
void PageScrollController::page_down() {
    scroll_by_page(+1);
}

void PageScrollController::detach() {
    if (container_ == nullptr) {
        return;
    }
    lv_obj_remove_event_cb_with_user_data(container_, container_event_cb, this);
    // Remove the button click cbs too (they carry `this`) before async-deleting the
    // gutter, so no orphaned callback can reference this controller — makes teardown
    // self-evidently safe rather than relying on async-delete timing.
    if (up_btn_ != nullptr) {
        lv_obj_remove_event_cb_with_user_data(up_btn_, up_clicked_cb, this);
    }
    if (down_btn_ != nullptr) {
        lv_obj_remove_event_cb_with_user_data(down_btn_, down_clicked_cb, this);
    }
    remove_reserved_padding();
    lv_obj_set_scrollbar_mode(container_, saved_scrollbar_mode_);
    if (gutter_ != nullptr) {
        // Drop the gutter-death watcher before async-deleting so the deferred
        // LV_EVENT_DELETE can't re-enter a controller that may already be gone.
        lv_obj_remove_event_cb_with_user_data(gutter_, gutter_event_cb, this);
        lv_obj_delete_async(gutter_); // sanctioned escape route (L059/L081)
        gutter_ = nullptr;
    }
    up_btn_ = down_btn_ = nullptr;
    container_ = nullptr;
    pad_applied_ = false;
}

void PageScrollController::on_container_deleted() {
    // Container (and its gutter child) are being destroyed by LVGL. Do NOT touch
    // LVGL objects — just null out and notify the owner to prune us.
    // All pointer-nulling MUST happen before on_deleted_() runs (see warning below).
    //
    // LVGL fires the parent's LV_EVENT_DELETE *before* deleting children
    // (obj_delete_core, lv_obj_tree.c), so the gutter is still alive here. Its
    // still-registered delete watcher carries `this`; drop it now so the child
    // deletion that follows can't re-enter a controller that on_deleted_() below
    // may have already destroyed.
    if (gutter_ != nullptr) {
        lv_obj_remove_event_cb_with_user_data(gutter_, gutter_event_cb, this);
    }
    gutter_ = up_btn_ = down_btn_ = nullptr;
    container_ = nullptr;
    pad_applied_ = false;
    if (on_deleted_) {
        // WARNING: on_deleted_() may synchronously destroy *this (owner prune).
        // Nothing may run after it.
        on_deleted_();
    }
}

void PageScrollController::on_gutter_deleted() {
    // The gutter was destroyed out from under us while the container survived — a
    // repopulate cleared the container's children (lv_obj_clean) without deleting the
    // container itself (#1123). A full container teardown never reaches here:
    // on_container_deleted() fires first (parent DELETE precedes child deletion) and
    // removes this watcher. So container_ is alive and safe to restore.
    gutter_ = up_btn_ = down_btn_ = nullptr; // children of the dying gutter
    if (container_ != nullptr) {
        // Unhook from the surviving container and undo what attach() mutated, so it
        // returns to its pre-attach state and can be cleanly re-injected later.
        lv_obj_remove_event_cb_with_user_data(container_, container_event_cb, this);
        remove_reserved_padding();
        lv_obj_set_scrollbar_mode(container_, saved_scrollbar_mode_);
        container_ = nullptr;
    }
    if (on_deleted_) {
        // WARNING: on_deleted_() may synchronously destroy *this (owner prune).
        // Nothing may run after it.
        on_deleted_();
    }
}

void PageScrollController::gutter_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    // self may be destroyed synchronously inside on_gutter_deleted() (owner-prune
    // callback) — do not touch self after this call.
    static_cast<PageScrollController*>(lv_event_get_user_data(e))->on_gutter_deleted();
}

void PageScrollController::container_event_cb(lv_event_t* e) {
    auto* self = static_cast<PageScrollController*>(lv_event_get_user_data(e));
    switch (lv_event_get_code(e)) {
    case LV_EVENT_SCROLL:
    case LV_EVENT_SIZE_CHANGED:
        self->refresh_reach_state();
        break;
    case LV_EVENT_DELETE:
        // self may be destroyed synchronously inside on_container_deleted()
        // (owner-prune callback) — do not touch self after this call.
        self->on_container_deleted();
        break;
    default:
        break;
    }
}

void PageScrollController::up_clicked_cb(lv_event_t* e) {
    static_cast<PageScrollController*>(lv_event_get_user_data(e))->page_up();
}
void PageScrollController::down_clicked_cb(lv_event_t* e) {
    static_cast<PageScrollController*>(lv_event_get_user_data(e))->page_down();
}

} // namespace helix::ui
