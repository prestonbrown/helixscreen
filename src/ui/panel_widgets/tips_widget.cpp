// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tips_widget.h"

#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_modal.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "core/subject.h"
#include "core/timer_guard.h"
#include "display_settings_manager.h"
#include "exception_policy.h"
#include "featured_tip.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix_psram_attr.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "subject_debug_registry.h"
#include "subject_managed_panel.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "hv/json.hpp"

// Tip rotation interval (60 seconds)
static constexpr uint32_t kTipRotationIntervalMs = 60000;

// Fade animation duration
static constexpr uint32_t kTipFadeDurationMs = 300;

// How often an active widget checks Moonraker for a featured line
static constexpr uint32_t kFeaturedPollIntervalMs = 15000;

// The featured line last shown or passed over, shared by every instance so a
// recycled widget does not show it again.
static std::string s_last_featured_id;

// The featured line on screen, when it was posted and when it went up. Shared
// so it holds the tile across a grid rebuild that recycles the widget.
static helix::PrintingTip s_featured_tip;
static double s_featured_posted_at = 0.0;
static uint32_t s_featured_shown_at = 0;

static double unix_now_s() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Whether the featured line keeps the tile from the tips: while it is current
/// when it replaces them, else for one rotation interval.
static bool featured_tip_holds_tile(bool featured_only) {
    if (s_featured_tip.title.empty()) {
        return false;
    }
    if (featured_only) {
        helix::featured_tip::FeaturedTip age_only;
        age_only.posted_at = s_featured_posted_at;
        return helix::featured_tip::is_fresh(age_only, unix_now_s(),
                                             helix::featured_tip::kReplaceMaxAgeSeconds);
    }
    return lv_tick_elaps(s_featured_shown_at) < kTipRotationIntervalMs;
}

// Subject owned by TipsWidget module — created before XML bindings resolve
static helix::Subject s_status_subject;
static HELIX_PSRAM_BSS char s_status_buffer[512];
// 1 while the label shows a featured line, which hides the "Tip:" label
static helix::Subject s_featured_subject;
static helix::WidgetSubjects s_subjects{"TipsWidgetSubjects"};

/// Widest unbreakable run of any tip title in `font`. LVGL wraps after any of
/// LV_TXT_BREAK_CHARS (a space is dropped at the line end, the others stay on
/// it) and otherwise breaks inside a run wider than its line.
static int widest_title_run_px(const lv_font_t* font) {
    int widest = 0;
    for (const auto& title : helix::TipsManager::get_instance()->get_all_titles()) {
        std::string run;
        for (size_t i = 0; i <= title.size(); ++i) {
            const char c = i < title.size() ? title[i] : ' ';
            if (std::strchr(LV_TXT_BREAK_CHARS, c) == nullptr) {
                run += c;
                continue;
            }
            if (c != ' ')
                run += c;
            widest = std::max(widest, helix::ui::measure_text_px(run.c_str(), font));
            run.clear();
        }
    }
    return widest;
}

/// A px const declared in the tips component's <consts>.
static int tips_component_px(const char* name) {
    const char* value = lv_xml_get_const(lv_xml_component_get_scope("panel_widget_tips"), name);
    return value ? lv_xml_atoi(value) : 0;
}

static void tips_widget_init_subjects() {
    s_subjects.ensure_init([] {
        s_status_subject.init_string(s_status_buffer, nullptr, sizeof(s_status_buffer),
                                     "Welcome to HelixScreen");
        s_subjects.publish("status_text", &s_status_subject);
        HELIX_SUBJECT_DEBUG_REGISTER(&s_status_subject, "status_text", LV_SUBJECT_TYPE_STRING);
        s_featured_subject.init_int(0);
        s_subjects.publish("tip_is_featured", &s_featured_subject);
        HELIX_SUBJECT_DEBUG_REGISTER(&s_featured_subject, "tip_is_featured", LV_SUBJECT_TYPE_INT);

        spdlog::debug("[TipsWidget] Subjects initialized (status_text, tip_is_featured)");
    });
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

    // Set user_data on the tip_container child (where event_cb is registered in XML)
    // so the callback can recover this widget instance via lv_obj_get_user_data()
    auto* tip_container = lv_obj_find_by_name(widget_obj_, "tip_container");
    if (tip_container) {
        lv_obj_set_user_data(tip_container, this);
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
    }
    stop_timers();
    lifetime_.invalidate();

    tip_label_ = nullptr;

    if (widget_obj_) {
        auto* tip_container = lv_obj_find_by_name(widget_obj_, "tip_container");
        if (tip_container) {
            lv_obj_set_user_data(tip_container, nullptr);
        }
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;

    spdlog::debug("[TipsWidget] Detached");
}

void TipsWidget::on_activate() {
    // Reset label opacity in case deactivation interrupted a mid-fade animation
    if (tip_label_) {
        // DECLARATIVE_OK: the tip fade animates this opacity frame by frame.
        lv_obj_set_style_opa(tip_label_, LV_OPA_COVER, LV_PART_MAIN);
    }

    if (!tip_rotation_timer_) {
        tip_rotation_timer_ = lv_timer_create(tip_rotation_timer_cb, kTipRotationIntervalMs, this);
        spdlog::debug("[TipsWidget] Started tip rotation timer ({}ms interval)",
                      kTipRotationIntervalMs);
    }
    if (!featured_poll_timer_) {
        featured_poll_timer_ =
            lv_timer_create(featured_poll_timer_cb, kFeaturedPollIntervalMs, this);
    }
    poll_featured_tip();
}

void TipsWidget::on_deactivate() {
    // Cancel any in-flight tip fade animations before the timer
    if (tip_animating_) {
        tip_animating_ = false;
        lv_anim_delete(this, nullptr);
    }

    stop_timers();
    // A featured-line reply that lands after this is dropped.
    lifetime_.invalidate();
    spdlog::debug("[TipsWidget] Stopped tip timers");
}

void TipsWidget::stop_timers() {
    if (tip_rotation_timer_) {
        helix::ui::lv_timer_cancel_safe(tip_rotation_timer_);
        tip_rotation_timer_ = nullptr;
    }
    if (featured_poll_timer_) {
        helix::ui::lv_timer_cancel_safe(featured_poll_timer_);
        featured_poll_timer_ = nullptr;
    }
}

void TipsWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px, int height_px) {
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

    tile_w_ = width_px;
    tile_h_ = height_px;
    fit_title_font();

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

bool TipsWidget::fits_at(int width_px, int /*height_px*/) const {
    // The title column is what the row leaves beside the accent bar, "Tip:" and
    // the help icon. Measured in font_heading, the larger of the two faces the
    // tile draws in, so the answer cannot turn from yes back to no where wider
    // tiles switch to it.
    const lv_font_t* text_font = theme_manager_get_font("font_heading");
    const lv_font_t* icon_font = theme_manager_get_font("icon_font_md");
    if (!text_font || !icon_font)
        return true;
    const int chrome =
        tips_component_px("accent_bar_width") + tips_component_px("accent_bar_gap") +
        helix::ui::measure_text_px(lv_tr("Tip:"), text_font) +
        helix::ui::measure_text_px(helix::ui::icon::lookup_codepoint("help_circle"), icon_font) +
        2 * theme_manager_get_spacing("space_sm");
    return width_px - chrome >= widest_title_run_px(text_font);
}

void TipsWidget::fit_title_font() {
    if (!tip_label_ || tile_w_ <= 0)
        return;
    // The title's height is capped at what the tile leaves inside the row's
    // padding, which a featured line drops: the XML max height resolves against
    // a content-height row, so without the cap a long title stops at one line
    // instead of wrapping into a tall tile and ending in its ellipsis there.
    auto* tip_container = lv_obj_get_parent(tip_label_);
    const int32_t pad_v = lv_obj_get_style_pad_top(tip_container, LV_PART_MAIN) +
                          lv_obj_get_style_pad_bottom(tip_container, LV_PART_MAIN);
    const int32_t max_h = std::max<int32_t>(0, tile_h_ - pad_v);
    lv_obj_set_style_max_height(tip_label_, max_h, 0);
    // Below wide-tier width the tile reads in the body face (on_size_changed).
    const bool compact = tile_w_ < widget_size::w_wide();
    const lv_font_t* font = theme_manager_get_font(compact ? "font_body" : "font_heading");
    if (!font)
        return;
    if (featured_tip::is_featured(current_tip_)) {
        // Only the smallest face may end in the ellipsis. Measured on the
        // laid-out label, which the hidden "Tip:" has just widened: the row
        // decides its width to the pixel, and a pixel short wraps the last word
        // into the ellipsis.
        lv_obj_update_layout(widget_obj_);
        std::vector<const lv_font_t*> faces;
        for (const char* face : {"font_heading", "font_body", "font_small"}) {
            if (compact && std::strcmp(face, "font_heading") == 0)
                continue;
            if (const lv_font_t* candidate = theme_manager_get_font(face))
                faces.push_back(candidate);
        }
        if (const lv_font_t* fitted = featured_tip::fit_font(
                current_tip_.title, faces, lv_obj_get_content_width(tip_label_), max_h,
                lv_obj_get_style_text_letter_space(tip_label_, LV_PART_MAIN),
                lv_obj_get_style_text_line_space(tip_label_, LV_PART_MAIN))) {
            font = fitted;
        }
    }
    lv_obj_set_style_text_font(tip_label_, font, 0);
}

void TipsWidget::set_displayed_tip(const PrintingTip& tip) {
    current_tip_ = tip;
    std::snprintf(s_status_buffer, sizeof(s_status_buffer), "%s", tip.title.c_str());
    lv_subject_set_string(&s_status_subject, s_status_buffer);
    lv_subject_set_int(&s_featured_subject, featured_tip::is_featured(tip) ? 1 : 0);
    fit_title_font();
}

void TipsWidget::set_config(const nlohmann::json& config) {
    featured_only_ = featured_tip::featured_only(config);
}

bool TipsWidget::on_edit_configure() {
    featured_only_ = !featured_only_;
    nlohmann::json config = nlohmann::json::object();
    config[featured_tip::kConfigFeaturedOnly] = featured_only_;
    save_widget_config(config);
    spdlog::info("[TipsWidget] Featured lines {} tips", featured_only_ ? "replace" : "mix with");
    ToastManager::instance().show(ToastSeverity::INFO,
                                  featured_only_ ? lv_tr("Lines from other apps replace tips")
                                                 : lv_tr("Lines from other apps mix with tips"));
    return true;
}

void TipsWidget::update_tip_of_day() {
    if (featured_tip_holds_tile(featured_only_)) {
        // Already up: a rotation does not fade the same line out and back in.
        if (current_tip_.id != s_featured_tip.id || tip_animating_) {
            show_tip(s_featured_tip);
        }
        return;
    }
    // Tip selection allocates on the heap; on memory-constrained ARM devices
    // (ad5x, k1) a std::bad_alloc here would otherwise unwind out of a
    // 60-second LVGL timer callback and terminate the process (#771).
    PrintingTip tip;
    if (!helix::contain_exceptions("[TipsWidget] get_random_unique_tip, skipping rotation", [&] {
            tip = TipsManager::get_instance()->get_random_unique_tip();
        })) {
        return;
    }

    if (!tip.title.empty()) {
        show_tip(tip);
    } else {
        spdlog::warn("[TipsWidget] Failed to get tip, keeping current");
    }
}

void TipsWidget::show_tip(const PrintingTip& tip) {
    if (tip_animating_) {
        // The fade in flight applies pending_tip_ when it reaches zero.
        pending_tip_ = tip;
        return;
    }
    if (tip_label_) {
        start_tip_fade_transition(tip);
        return;
    }
    // No label to animate (initial load): instant update
    set_displayed_tip(tip);
    spdlog::trace("[TipsWidget] Updated tip (instant): {}", tip.title);
}

void TipsWidget::poll_featured_tip() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        return;
    }
    api->database_get_item(
        featured_tip::kNamespace, featured_tip::kKey,
        lifetime_.bg_cb("TipsWidget::featured_tip",
                        [this](const nlohmann::json& value) { apply_featured_tip(value); }),
        // No featured line: Moonraker answers a missing key with an
        // error, which is the normal state without a writer, and
        // after a switch to a printer that has none.
        lifetime_.bg_cb("TipsWidget::featured_tip_missing", [this](const MoonrakerError& err) {
            spdlog::trace("[TipsWidget] No featured tip: {}", err.message);
            drop_featured_tip();
        }));
}

void TipsWidget::apply_featured_tip(const nlohmann::json& value) {
    const auto featured = featured_tip::parse_record(value);
    if (!featured || featured->id == s_last_featured_id) {
        return;
    }
    s_last_featured_id = featured->id;
    if (!featured_tip::is_fresh(*featured, unix_now_s(),
                                featured_tip::max_age_seconds(featured_only_))) {
        spdlog::debug("[TipsWidget] Skipping stale featured tip {}", featured->id);
        return;
    }
    spdlog::info("[TipsWidget] Showing featured tip from {}", featured->source);
    s_featured_tip = featured_tip::to_printing_tip(*featured);
    s_featured_posted_at = featured->posted_at;
    s_featured_shown_at = lv_tick_get();
    show_tip(s_featured_tip);
    // The featured line gets a full rotation interval before the next tip.
    if (tip_rotation_timer_) {
        lv_timer_reset(tip_rotation_timer_);
    }
}

void TipsWidget::drop_featured_tip() {
    // Mixed in, a line leaves on its own after one rotation. Replacing the
    // tips, it would stay up with no record behind it, so the tips come back
    // at the next rotation, and a record that reappears shows again.
    if (!featured_only_ || s_featured_tip.title.empty()) {
        return;
    }
    spdlog::debug("[TipsWidget] Featured tip gone, back to tips");
    s_featured_tip = PrintingTip{};
    s_featured_posted_at = 0.0;
    s_last_featured_id.clear();
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
        set_displayed_tip(pending_tip_);
        // DECLARATIVE_OK: the tip fade animates this opacity frame by frame.
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
    lv_anim_set_duration(&anim, kTipFadeDurationMs);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);

    // Execute callback: update opacity on each frame
    lv_anim_set_exec_cb(&anim, [](void* var, int32_t value) {
        auto* self = static_cast<TipsWidget*>(var);
        if (self->tip_label_) {
            // DECLARATIVE_OK: the tip fade animates this opacity frame by frame.
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
    set_displayed_tip(pending_tip_);

    spdlog::debug("[TipsWidget] Applied pending tip: {}", pending_tip_.title);

    // Skip animation if disabled — show at full opacity immediately
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        if (tip_label_) {
            // DECLARATIVE_OK: the tip fade animates this opacity frame by frame.
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
    lv_anim_set_duration(&anim, kTipFadeDurationMs);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);

    // Execute callback: update opacity on each frame
    lv_anim_set_exec_cb(&anim, [](void* var, int32_t value) {
        auto* self = static_cast<TipsWidget*>(var);
        if (self->tip_label_) {
            // DECLARATIVE_OK: the tip fade animates this opacity frame by frame.
            lv_obj_set_style_opa(self->tip_label_, static_cast<lv_opa_t>(value), LV_PART_MAIN);
        }
    });

    // Completion callback: mark animation as done. A tip that arrived during
    // the fade-in is still waiting in pending_tip_, so it gets its own fade.
    lv_anim_set_completed_cb(&anim, [](lv_anim_t* a) {
        auto* self = static_cast<TipsWidget*>(a->var);
        self->tip_animating_ = false;
        if (self->pending_tip_.id != self->current_tip_.id) {
            self->start_tip_fade_transition(self->pending_tip_);
        }
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
    const std::string& heading =
        current_tip_.heading.empty() ? current_tip_.title : current_tip_.heading;
    helix::ui::modal_alert(heading.c_str(), current_tip_.content.c_str(), ModalSeverity::Info);
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

void TipsWidget::featured_poll_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<TipsWidget*>(lv_timer_get_user_data(timer));
    if (self) {
        self->poll_featured_tip();
    }
}
