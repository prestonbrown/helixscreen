// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tips_widget.h"

#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_modal.h"
#include "ui_timer_guard.h"

#include "display_settings_manager.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

// Tip rotation interval (60 seconds)
static constexpr uint32_t TIP_ROTATION_INTERVAL_MS = 60000;

// Fade animation duration
static constexpr uint32_t TIP_FADE_DURATION_MS = 300;

// Subject owned by TipsWidget module — created before XML bindings resolve
static lv_subject_t s_status_subject;
static char s_status_buffer[512];
static bool s_subjects_initialized = false;

static void tips_widget_init_subjects() {
    if (s_subjects_initialized) {
        return;
    }

    lv_subject_init_string(&s_status_subject, s_status_buffer, nullptr, sizeof(s_status_buffer),
                           "Welcome to HelixScreen");
    lv_xml_register_subject(nullptr, "status_text", &s_status_subject);
    SubjectDebugRegistry::instance().register_subject(&s_status_subject, "status_text",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

    s_subjects_initialized = true;

    // Self-register cleanup with StaticSubjectRegistry (co-located with init)
    StaticSubjectRegistry::instance().register_deinit("TipsWidgetSubjects", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            lv_subject_deinit(&s_status_subject);
            s_subjects_initialized = false;
            spdlog::trace("[TipsWidget] Subjects deinitialized");
        }
    });

    spdlog::debug("[TipsWidget] Subjects initialized (status_text)");
}

namespace helix {
void register_tips_widget() {
    register_widget_factory("tips",
                            [](const std::string&) { return std::make_unique<TipsWidget>(); });
    register_widget_subjects("tips", tips_widget_init_subjects);

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "tip_text_clicked_cb", TipsWidget::tip_text_clicked_cb);
}
} // namespace helix

using namespace helix;

TipsWidget::TipsWidget() = default;

TipsWidget::~TipsWidget() {
    detach();
}

void TipsWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Store this pointer for event callback recovery
    lv_obj_set_user_data(widget_obj_, this);

    // Set user_data on the tip_container child (where event_cb is registered in XML)
    // so the callback can recover this widget instance via lv_obj_get_user_data()
    auto* tip_container = lv_obj_find_by_name(widget_obj_, "tip_container");
    if (tip_container) {
        lv_obj_set_user_data(tip_container, this);
        // Pressed feedback: dim on touch
        lv_obj_set_style_opa(tip_container, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
    }

    // Cache tip label for fade animation
    tip_label_ = lv_obj_find_by_name(widget_obj_, "status_text_label");
    if (!tip_label_) {
        spdlog::warn("[TipsWidget] Could not find status_text_label for tip animation");
    }

    // Set initial tip of the day
    update_tip_of_day();

    spdlog::debug("[TipsWidget] Attached");
}

void TipsWidget::detach() {
    if (lv_is_initialized()) {
        // Cancel any in-flight tip fade animations (var=this, not an lv_obj_t*)
        if (tip_animating_) {
            tip_animating_ = false;
            lv_anim_delete(this, nullptr);
        }

        if (tip_rotation_timer_) {
            helix::ui::lv_timer_cancel_safe(tip_rotation_timer_);
            tip_rotation_timer_ = nullptr;
        }
    }

    tip_label_ = nullptr;

    if (widget_obj_) {
        auto* tip_container = lv_obj_find_by_name(widget_obj_, "tip_container");
        if (tip_container) {
            lv_obj_set_user_data(tip_container, nullptr);
        }
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;

    spdlog::debug("[TipsWidget] Detached");
}

void TipsWidget::on_activate() {
    // Reset label opacity in case deactivation interrupted a mid-fade animation
    if (tip_label_) {
        lv_obj_set_style_opa(tip_label_, LV_OPA_COVER, LV_PART_MAIN);
    }

    if (!tip_rotation_timer_) {
        tip_rotation_timer_ =
            lv_timer_create(tip_rotation_timer_cb, TIP_ROTATION_INTERVAL_MS, this);
        spdlog::debug("[TipsWidget] Started tip rotation timer ({}ms interval)",
                      TIP_ROTATION_INTERVAL_MS);
    }
}

void TipsWidget::on_deactivate() {
    // Cancel any in-flight tip fade animations before the timer
    if (tip_animating_) {
        tip_animating_ = false;
        lv_anim_delete(this, nullptr);
    }

    if (tip_rotation_timer_) {
        helix::ui::lv_timer_cancel_safe(tip_rotation_timer_);
        tip_rotation_timer_ = nullptr;
        spdlog::debug("[TipsWidget] Stopped tip rotation timer");
    }
}

void TipsWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                 int /*height_px*/) {
    if (!widget_obj_)
        return;

    // Below wide-tier width, use smaller text and icon
    bool compact = (width_px < widget_size::w_wide());
    const char* font_token = compact ? "font_body" : "font_heading";
    const lv_font_t* text_font = theme_manager_get_font(font_token);
    if (!text_font)
        return;

    const lv_font_t* icon_font = theme_manager_get_font("icon_font_md");
    if (!icon_font)
        icon_font = &mdi_icons_32;

    // Update text labels: "Tip:" prefix and bound tip text
    auto* tip_container = lv_obj_find_by_name(widget_obj_, "tip_container");
    if (!tip_container)
        return;

    // The tip text label (named)
    if (tip_label_)
        lv_obj_set_style_text_font(tip_label_, text_font, 0);

    // The "Tip:" prefix label is the first child of tip_container (unnamed text_heading)
    lv_obj_t* prefix = lv_obj_get_child(tip_container, 0);
    if (prefix)
        lv_obj_set_style_text_font(prefix, text_font, 0);

    // The help_circle icon is the last child (icon component = lv_label with MDI font)
    uint32_t count = lv_obj_get_child_count(tip_container);
    if (count > 0) {
        lv_obj_t* icon = lv_obj_get_child(tip_container, static_cast<int32_t>(count - 1));
        if (icon)
            lv_obj_set_style_text_font(icon, icon_font, 0);
    }
}

void TipsWidget::update_tip_of_day() {
    // Tip selection allocates on the heap; on memory-constrained ARM devices
    // (ad5x, k1) a std::bad_alloc here would otherwise unwind out of a
    // 60-second LVGL timer callback and terminate the process (#771).
    PrintingTip tip;
    try {
        tip = TipsManager::get_instance()->get_random_unique_tip();
    } catch (const std::exception& e) {
        spdlog::warn("[TipsWidget] get_random_unique_tip threw: {} — skipping rotation", e.what());
        return;
    }

    if (!tip.title.empty()) {
        // Use animated transition if label is available and not already animating
        if (tip_label_ && !tip_animating_) {
            start_tip_fade_transition(tip);
        } else {
            // Fallback: instant update (initial load or animation in progress)
            current_tip_ = tip;
            std::snprintf(s_status_buffer, sizeof(s_status_buffer), "%s", tip.title.c_str());
            lv_subject_copy_string(&s_status_subject, s_status_buffer);
            spdlog::trace("[TipsWidget] Updated tip (instant): {}", tip.title);
        }
    } else {
        spdlog::warn("[TipsWidget] Failed to get tip, keeping current");
    }
}

void TipsWidget::start_tip_fade_transition(const PrintingTip& new_tip) {
    if (!tip_label_ || tip_animating_) {
        return;
    }

    // Store the pending tip to apply after fade-out
    pending_tip_ = new_tip;
    tip_animating_ = true;

    spdlog::debug("[TipsWidget] Starting tip fade transition to: {}", new_tip.title);

    // Skip animation if disabled — apply text immediately
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        current_tip_ = pending_tip_;
        std::snprintf(s_status_buffer, sizeof(s_status_buffer), "%s", pending_tip_.title.c_str());
        lv_subject_copy_string(&s_status_subject, s_status_buffer);
        lv_obj_set_style_opa(tip_label_, LV_OPA_COVER, LV_PART_MAIN);
        tip_animating_ = false;
        spdlog::debug("[TipsWidget] Animations disabled - applied tip instantly");
        return;
    }

    // Fade out animation
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, this);
    lv_anim_set_values(&anim, 255, 0);
    lv_anim_set_duration(&anim, TIP_FADE_DURATION_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);

    // Execute callback: update opacity on each frame
    lv_anim_set_exec_cb(&anim, [](void* var, int32_t value) {
        auto* self = static_cast<TipsWidget*>(var);
        if (self->tip_label_) {
            lv_obj_set_style_opa(self->tip_label_, static_cast<lv_opa_t>(value), LV_PART_MAIN);
        }
    });

    // Completion callback: apply new text and start fade-in
    lv_anim_set_completed_cb(&anim, [](lv_anim_t* a) {
        auto* self = static_cast<TipsWidget*>(a->var);
        self->apply_pending_tip();
    });

    lv_anim_start(&anim);
}

void TipsWidget::apply_pending_tip() {
    // Apply the pending tip text
    current_tip_ = pending_tip_;
    std::snprintf(s_status_buffer, sizeof(s_status_buffer), "%s", pending_tip_.title.c_str());
    lv_subject_copy_string(&s_status_subject, s_status_buffer);

    spdlog::debug("[TipsWidget] Applied pending tip: {}", pending_tip_.title);

    // Skip animation if disabled — show at full opacity immediately
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        if (tip_label_) {
            lv_obj_set_style_opa(tip_label_, LV_OPA_COVER, LV_PART_MAIN);
        }
        tip_animating_ = false;
        return;
    }

    // Fade in animation
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, this);
    lv_anim_set_values(&anim, 0, 255);
    lv_anim_set_duration(&anim, TIP_FADE_DURATION_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);

    // Execute callback: update opacity on each frame
    lv_anim_set_exec_cb(&anim, [](void* var, int32_t value) {
        auto* self = static_cast<TipsWidget*>(var);
        if (self->tip_label_) {
            lv_obj_set_style_opa(self->tip_label_, static_cast<lv_opa_t>(value), LV_PART_MAIN);
        }
    });

    // Completion callback: mark animation as done
    lv_anim_set_completed_cb(&anim, [](lv_anim_t* a) {
        auto* self = static_cast<TipsWidget*>(a->var);
        self->tip_animating_ = false;
    });

    lv_anim_start(&anim);
}

void TipsWidget::handle_tip_text_clicked() {
    if (current_tip_.title.empty()) {
        spdlog::warn("[TipsWidget] No tip available to display");
        return;
    }

    spdlog::info("[TipsWidget] Tip text clicked - showing detail dialog");

    // Use alert helper which auto-handles OK button to close
    helix::ui::modal_alert(current_tip_.title.c_str(), current_tip_.content.c_str(),
                           ModalSeverity::Info);
}

void TipsWidget::handle_tip_rotation_timer() {
    update_tip_of_day();
}

void TipsWidget::tip_text_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TipsWidget] tip_text_clicked_cb");

    auto* widget = panel_widget_from_event<TipsWidget>(e);
    if (widget) {
        widget->handle_tip_text_clicked();
    } else {
        spdlog::warn("[TipsWidget] tip_text_clicked_cb: could not recover widget instance");
    }

    LVGL_SAFE_EVENT_CB_END();
}

void TipsWidget::tip_rotation_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<TipsWidget*>(lv_timer_get_user_data(timer));
    if (self) {
        self->handle_tip_rotation_timer();
    }
}
