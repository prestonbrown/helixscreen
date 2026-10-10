// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_clog_meter.h"

#include "ui_progress_arc.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "clog_meter_geometry.h"
#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// Arc size = percentage of card width. The stroke comes from the shared
// helix_progress_arc helper's diameter tier, not from here.
constexpr int32_t ARC_WIDTH_PCT = 18; // arc is 18% of card width
constexpr int32_t MIN_ARC_SIZE = 24;

UiClogMeter::UiClogMeter(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[ClogMeter] NULL parent");
        return;
    }

    root_ = lv_obj_find_by_name(parent, "clog_meter");
    if (!root_) {
        spdlog::warn("[ClogMeter] clog_meter not found in parent");
        return;
    }

    arc_container_ = lv_obj_find_by_name(root_, "clog_arc_container");
    arc_ = lv_obj_find_by_name(root_, "clog_arc");
    if (!arc_container_ || !arc_) {
        spdlog::warn("[ClogMeter] clog_arc_container or clog_arc not found");
        return;
    }

    // Hook the shared helix_progress_arc helper. We still own arc_container_
    // sizing — the helper sizes the arc *inside* the
    // container, computes the diameter tier, and the helix_progress_arc.xml
    // bind_styles apply the matching stroke. Use the *_owned variant so the
    // helper allocates + manages the tier subject's lifetime, tied to the
    // arc's deletion — UiClogMeter is destroyed before its widgets (the arc
    // is owned by the LVGL tree under the card), so a member-owned subject
    // would leave dangling bind_style observers on the still-live arc.
    helix::ui::attach_progress_arc_owned(arc_, arc_container_);

    // Attach SIZE_CHANGED callback on the loaded card (root_'s parent)
    // to dynamically size the arc container to fill available height
    lv_obj_t* card = lv_obj_get_parent(root_);
    if (card) {
        // SIZE_CHANGED is a layout event — cannot be registered via XML <event_cb>
        lv_obj_add_event_cb(card, on_card_size_changed, LV_EVENT_SIZE_CHANGED, this);
        resize_arc();
    }

    // Cached so the safe state can swap the reading for the check icon
    value_text_ = lv_obj_find_by_name(root_, "clog_value_text");
    safe_icon_ = lv_obj_find_by_name(root_, "clog_safe_icon");

    model_.emplace([this](const ClogMeterSample& s) { apply(s); });
    apply(model_->sample());
    spdlog::debug("[ClogMeter] Initialized");
}

UiClogMeter::~UiClogMeter() {
    // Freeze update queue around observer teardown to prevent race with
    // WebSocket thread enqueueing deferred callbacks between drain and destroy
    auto freeze = UpdateQueue::instance().scoped_freeze();
    UpdateQueue::instance().drain();

    // Remove SIZE_CHANGED callback to prevent dangling this pointer
    // The card is reached through root_, so root_ must still be alive to ask.
    if (root_ && lv_is_initialized() && lv_obj_is_valid(root_)) {
        helix::ui::remove_event_cb_if_alive(lv_obj_get_parent(root_), on_card_size_changed, this);
    }

    // Before the widget pointers below are cleared: its callback calls apply(),
    // which reads them.
    model_.reset();

    root_ = nullptr;
    arc_ = nullptr;
    safe_icon_ = nullptr;
    value_text_ = nullptr;
    // The progress arc's tier subject (allocated via attach_progress_arc_owned)
    // is freed automatically when the arc is deleted by LVGL — no cleanup
    // needed here.
    spdlog::debug("[ClogMeter] Destroyed");
}

void UiClogMeter::apply(const ClogMeterSample& s) {
    if (!arc_)
        return;

    if (s.is_symmetrical()) {
        // Flowguard: -100..+100 mapped onto LVGL's 0..200 symmetrical range,
        // which draws out from the middle. The bar spells the same decision as
        // centre-out geometry; ClogMeterSample is where the two agree.
        lv_arc_set_range(arc_, 0, 200);
        lv_arc_set_mode(arc_, LV_ARC_MODE_SYMMETRICAL);
        lv_arc_set_value(arc_, s.value + 100);
    } else {
        lv_arc_set_range(arc_, 0, 100);
        lv_arc_set_mode(arc_, LV_ARC_MODE_NORMAL);
        lv_arc_set_value(arc_, s.value);
    }

    update_arc_color(s);
    update_safe_state(s);
}

void UiClogMeter::update_arc_color(const ClogMeterSample& s) {
    if (!arc_)
        return;

    // Dynamic indicator colour is an intentional exception to the "no C++
    // styling" rule. The rule itself lives in clog_meter_geometry.h so the
    // horizontal bar (#1017) draws the same ramp from the same tokens.
    lv_obj_set_style_arc_color(arc_, resolve_clog_tint(s.mode, s.value, s.warning),
                               LV_PART_INDICATOR);
}

void UiClogMeter::update_safe_state(const ClogMeterSample& s) {
    // "Nothing to report" rather than "zero danger": the arc goes away
    // entirely and the check icon stands in for it. The bar reads the same
    // predicate so the two presentations cannot drift.
    const bool safe = s.is_safe();

    if (arc_) {
        if (safe) {
            lv_obj_add_flag(arc_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(arc_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (value_text_) {
        if (safe) {
            lv_obj_add_flag(value_text_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(value_text_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (safe_icon_) {
        if (safe) {
            lv_obj_remove_flag(safe_icon_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(safe_icon_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void UiClogMeter::on_card_size_changed(lv_event_t* e) {
    auto* self = static_cast<UiClogMeter*>(lv_event_get_user_data(e));
    if (self)
        self->resize_arc();
}

void UiClogMeter::resize_arc() {
    if (!arc_ || !arc_container_ || !root_)
        return;

    // Re-entrancy guard: lv_obj_update_layout() can fire SIZE_CHANGED
    if (in_resize_)
        return;
    in_resize_ = true;

    lv_obj_t* card = lv_obj_get_parent(root_);
    if (!card) {
        in_resize_ = false;
        return;
    }

    lv_obj_update_layout(card);

    // Arc size = percentage of card width (responsive to breakpoint)
    const int32_t card_w = lv_obj_get_content_width(card);
    const int32_t arc_size = LV_MAX(card_w * ARC_WIDTH_PCT / 100, MIN_ARC_SIZE);

    // Skip if already at target size
    if (lv_obj_get_width(arc_) == arc_size && lv_obj_get_height(arc_) == arc_size) {
        in_resize_ = false;
        return;
    }

    // Size the container — the shared helper resizes the arc inside it
    // and applies stroke via bind_style.
    lv_obj_set_size(arc_container_, arc_size, arc_size);
    helix::ui::refresh_progress_arc(arc_);

    spdlog::debug("[ClogMeter] arc={}x{}", arc_size, arc_size);
    in_resize_ = false;
}

} // namespace helix::ui
