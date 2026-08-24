// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_toast_manager.h"

#include "ui_breakpoint.h"
#include "ui_icon_codepoints.h"
#include "ui_notification_history.h"
#include "ui_notification_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "display_settings_manager.h"
#include "sound_manager.h"
#include "static_subject_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <string>

using namespace helix;

namespace helix {
namespace ui {

void show_feature_unavailable_toast() {
    // Single shared, translatable copy reused across every excluded-subsystem
    // affordance on the ESP32 v1 build. See ui_toast_manager.h.
    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Not yet available on this display"),
                                  3000);
}

} // namespace ui
} // namespace helix

// Animation durations — kept in sync with globals.xml anim_fast / anim_normal.
static constexpr int32_t TOAST_ENTRANCE_DURATION_MS = 200;
static constexpr int32_t TOAST_EXIT_DURATION_MS = 150;
static constexpr int32_t TOAST_ENTRANCE_OFFSET_Y = -30; // slide-down distance

ToastManager& ToastManager::instance() {
    static ToastManager instance;
    return instance;
}

ToastManager::~ToastManager() {
    if (!lv_is_initialized())
        return;
    for (auto& inst : active_) {
        if (inst.dismiss_timer)
            lv_timer_delete(inst.dismiss_timer);
    }
}

// ============================================================================
// Helpers
// ============================================================================

static const char* severity_to_string(ToastSeverity s) {
    switch (s) {
    case ToastSeverity::ERROR:
        return "error";
    case ToastSeverity::WARNING:
        return "warning";
    case ToastSeverity::SUCCESS:
        return "success";
    case ToastSeverity::INFO:
    default:
        return "info";
    }
}

static void severity_to_icon(ToastSeverity s, const char*& glyph, const char*& color_token) {
    switch (s) {
    case ToastSeverity::SUCCESS:
        glyph = ui_icon::lookup_codepoint("check");
        color_token = "#success";
        break;
    case ToastSeverity::WARNING:
        glyph = ui_icon::lookup_codepoint("triangle_exclamation");
        color_token = "#warning";
        break;
    case ToastSeverity::ERROR:
        glyph = ui_icon::lookup_codepoint("triangle_exclamation");
        color_token = "#danger";
        break;
    case ToastSeverity::INFO:
    default:
        glyph = ui_icon::lookup_codepoint("info_circle");
        color_token = "#info";
        break;
    }
    if (!glyph)
        glyph = ""; // paranoia
}

// Read an integer spacing constant from globals.xml (e.g. space_sm). Falls
// back to a sane default if the scope isn't registered yet.
static int32_t xml_int_const(const char* name, int32_t fallback) {
    const char* val = lv_xml_get_const_silent(nullptr, name);
    if (!val || !*val)
        return fallback;
    return static_cast<int32_t>(std::atoi(val));
}

// Toast stack width by breakpoint. Calibrated to hold ~50 chars of body text
// per line given the body font for each tier, so a typical message wraps to
// at most two lines. Smaller screens get near-full-width; larger screens get
// a fixed-ish card that floats in the top-right corner.
static int32_t toast_stack_width_for(UiBreakpoint bp) {
    switch (bp) {
    case UiBreakpoint::Micro:
    case UiBreakpoint::Tiny:
        return 400;
    case UiBreakpoint::Small:
        return 420;
    case UiBreakpoint::Medium:
        return 460;
    case UiBreakpoint::Large:
        return 500;
    case UiBreakpoint::XLarge:
        return 560;
    case UiBreakpoint::XXLarge:
    default:
        return 640;
    }
}

static int32_t compute_toast_stack_width() {
    UiBreakpoint bp = UiBreakpoint::Medium;
    if (auto* s = theme_manager_get_breakpoint_subject()) {
        bp = as_breakpoint(lv_subject_get_int(s));
    }
    return toast_stack_width_for(bp);
}

// ============================================================================
// Animations
// ============================================================================

void ToastManager::animate_entrance(lv_obj_t* toast) {
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_obj_set_style_translate_y(toast, 0, LV_PART_MAIN);
        lv_obj_set_style_opa(toast, LV_OPA_COVER, LV_PART_MAIN);
        return;
    }

    lv_obj_set_style_translate_y(toast, TOAST_ENTRANCE_OFFSET_Y, LV_PART_MAIN);
    lv_obj_set_style_opa(toast, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_anim_t slide_anim;
    lv_anim_init(&slide_anim);
    lv_anim_set_var(&slide_anim, toast);
    lv_anim_set_values(&slide_anim, TOAST_ENTRANCE_OFFSET_Y, 0);
    lv_anim_set_duration(&slide_anim, TOAST_ENTRANCE_DURATION_MS);
    lv_anim_set_path_cb(&slide_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&slide_anim, [](void* obj, int32_t value) {
        auto* o = static_cast<lv_obj_t*>(obj);
        if (!lv_obj_is_valid(o))
            return;
        lv_obj_set_style_translate_y(o, value, LV_PART_MAIN);
    });
    lv_anim_start(&slide_anim);

    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, toast);
    lv_anim_set_values(&fade_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade_anim, TOAST_ENTRANCE_DURATION_MS);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        auto* o = static_cast<lv_obj_t*>(obj);
        if (!lv_obj_is_valid(o))
            return;
        lv_obj_set_style_opa(o, static_cast<lv_opa_t>(value), LV_PART_MAIN);
    });
    lv_anim_start(&fade_anim);
}

void ToastManager::animate_exit(lv_obj_t* toast) {
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        finalize_remove(toast);
        return;
    }

    lv_anim_t fade_anim;
    lv_anim_init(&fade_anim);
    lv_anim_set_var(&fade_anim, toast);
    lv_anim_set_values(&fade_anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade_anim, TOAST_EXIT_DURATION_MS);
    lv_anim_set_path_cb(&fade_anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&fade_anim, [](void* obj, int32_t value) {
        auto* o = static_cast<lv_obj_t*>(obj);
        if (!lv_obj_is_valid(o))
            return;
        lv_obj_set_style_opa(o, static_cast<lv_opa_t>(value), LV_PART_MAIN);
    });
    lv_anim_set_completed_cb(&fade_anim, exit_animation_complete_cb);
    lv_anim_start(&fade_anim);
}

void ToastManager::exit_animation_complete_cb(lv_anim_t* anim) {
    auto* toast = static_cast<lv_obj_t*>(anim->var);
    ToastManager::instance().finalize_remove(toast);
}

// ============================================================================
// Init / deinit
// ============================================================================

void ToastManager::init() {
    if (initialized_.load(std::memory_order_acquire)) {
        spdlog::warn("[ToastManager] Already initialized - skipping");
        return;
    }

    lv_xml_register_event_cb(nullptr, "toast_close_btn_clicked", close_btn_clicked);

    StaticSubjectRegistry::instance().register_deinit(
        "ToastManager", []() { ToastManager::instance().deinit_subjects(); });

    initialized_.store(true, std::memory_order_release);
    spdlog::debug("[ToastManager] Toast notification system initialized");
}

void ToastManager::deinit_subjects() {
    if (!initialized_.load(std::memory_order_acquire))
        return;

    if (!lv_is_initialized()) {
        initialized_.store(false, std::memory_order_release);
        return;
    }

    // Cancel timers first so dismiss_timer_cb can't fire mid-teardown.
    for (auto& inst : active_) {
        if (inst.dismiss_timer) {
            lv_timer_delete(inst.dismiss_timer);
            inst.dismiss_timer = nullptr;
        }
    }
    // Stack container is parented to lv_layer_top — that gets cleaned by
    // lv_deinit. Just drop the pointer.
    active_.clear();
    toast_stack_ = nullptr;

    initialized_.store(false, std::memory_order_release);
    spdlog::debug("[ToastManager] Deinitialized");
}

// ============================================================================
// Stack container (lazy)
// ============================================================================

void ToastManager::ensure_stack_container() {
    const int32_t margin = xml_int_const("space_2xl", 24);
    int32_t stack_width = compute_toast_stack_width();

    // The breakpoint width table is calibrated for landscape; in portrait the
    // cramped axis is the width, so clamp to the screen or the stack overflows
    // the left edge (it is anchored TOP_RIGHT with a margin on each side).
    if (auto* disp = lv_display_get_default()) {
        const int32_t max_width = lv_display_get_horizontal_resolution(disp) - 2 * margin;
        if (max_width > 0 && stack_width > max_width) {
            stack_width = max_width;
        }
    }

    if (!toast_stack_ || !lv_obj_is_valid(toast_stack_)) {
        lv_obj_t* layer = lv_layer_top();
        toast_stack_ = lv_obj_create(layer);

        lv_obj_set_flex_flow(toast_stack_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(toast_stack_, xml_int_const("space_sm", 8), LV_PART_MAIN);
        lv_obj_set_style_pad_all(toast_stack_, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(toast_stack_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(toast_stack_, 0, LV_PART_MAIN);
        // Container itself should never intercept clicks — only the toasts inside do.
        lv_obj_clear_flag(toast_stack_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(toast_stack_, LV_OBJ_FLAG_CLICKABLE);
    }

    // (Re)apply size + anchor every time — picks up breakpoint changes between
    // toasts. Toasts are width:100% children, so they resize with us.
    lv_obj_set_size(toast_stack_, stack_width, LV_SIZE_CONTENT);
    lv_obj_align(toast_stack_, LV_ALIGN_TOP_RIGHT, -margin, margin);
}

// ============================================================================
// List lookup
// ============================================================================

ToastManager::ToastList::iterator ToastManager::find_by_widget(lv_obj_t* widget) {
    for (auto it = active_.begin(); it != active_.end(); ++it) {
        if (it->widget == widget)
            return it;
    }
    return active_.end();
}

size_t ToastManager::visible_count() const {
    size_t n = 0;
    for (const auto& i : active_) {
        if (!i.is_exiting)
            ++n;
    }
    return n;
}

// ============================================================================
// Public API
// ============================================================================

void ToastManager::show(ToastSeverity severity, const char* message, uint32_t duration_ms) {
    struct ToastParams {
        ToastSeverity severity;
        std::string message;
        uint32_t duration_ms;
    };

    auto params =
        std::make_unique<ToastParams>(ToastParams{severity, message ? message : "", duration_ms});

    helix::ui::queue_update<ToastParams>(std::move(params), [](ToastParams* p) {
        ToastManager::instance().create_toast_internal(p->severity, p->message.c_str(), nullptr,
                                                       p->duration_ms, false, nullptr, nullptr,
                                                       nullptr);
    });
}

void ToastManager::show_with_detail(ToastSeverity severity, const char* message, const char* detail,
                                    uint32_t duration_ms) {
    struct ToastDetailParams {
        ToastSeverity severity;
        std::string message;
        std::string detail;
        uint32_t duration_ms;
    };

    auto params = std::make_unique<ToastDetailParams>(
        ToastDetailParams{severity, message ? message : "", detail ? detail : "", duration_ms});

    helix::ui::queue_update<ToastDetailParams>(std::move(params), [](ToastDetailParams* p) {
        ToastManager::instance().create_toast_internal(p->severity, p->message.c_str(),
                                                       p->detail.c_str(), p->duration_ms, false,
                                                       nullptr, nullptr, nullptr);
    });
}

void ToastManager::show_with_action(ToastSeverity severity, const char* message,
                                    const char* action_text, toast_action_callback_t callback,
                                    void* user_data, uint32_t duration_ms) {
    if (!action_text || !callback) {
        spdlog::warn("[ToastManager] Toast action requires action_text and callback");
        show(severity, message, duration_ms);
        return;
    }

    struct ToastActionParams {
        ToastSeverity severity;
        std::string message;
        std::string action_text;
        toast_action_callback_t action_cb;
        void* user_data;
        uint32_t duration_ms;
    };

    auto params = std::make_unique<ToastActionParams>(ToastActionParams{
        severity, message ? message : "", action_text, callback, user_data, duration_ms});

    helix::ui::queue_update<ToastActionParams>(std::move(params), [](ToastActionParams* p) {
        ToastManager::instance().create_toast_internal(p->severity, p->message.c_str(), nullptr,
                                                       p->duration_ms, true, p->action_cb,
                                                       p->user_data, p->action_text.c_str());
    });
}

void ToastManager::hide() {
    // Dismiss all visible toasts.
    for (auto it = active_.begin(); it != active_.end(); ++it) {
        if (!it->is_exiting)
            begin_exit(it);
    }
}

bool ToastManager::is_visible() const {
    return visible_count() > 0;
}

// ============================================================================
// Create
// ============================================================================

void ToastManager::create_toast_internal(ToastSeverity severity, const char* message,
                                         const char* detail, uint32_t duration_ms, bool with_action,
                                         toast_action_callback_t action_cb, void* action_user_data,
                                         const char* action_text) {
    if (!message) {
        spdlog::warn("[ToastManager] Attempted to show toast with null message");
        return;
    }

    // Dedupe: rapid identical toasts refresh the existing one instead of
    // stacking. Action toasts are excluded (callback/user_data may differ).
    if (!with_action && refresh_duplicate(severity, message)) {
        return;
    }

    ensure_stack_container();
    if (!toast_stack_) {
        spdlog::error("[ToastManager] Failed to create stack container");
        return;
    }

    // Enforce visible cap — force-remove oldest if at capacity. Only drops
    // not-already-exiting toasts (exiting ones will be cleared by their anim).
    while (visible_count() >= max_visible_) {
        auto it = active_.begin();
        while (it != active_.end() && it->is_exiting)
            ++it;
        if (it == active_.end())
            break;
        force_remove(it);
    }

    const char* icon_glyph = nullptr;
    const char* icon_color = nullptr;
    severity_to_icon(severity, icon_glyph, icon_color);

    // Build attrs. Keep the buffers alive for the lv_xml_create call.
    const char* hide_action_str = with_action ? "false" : "true";
    const char* action_text_safe = (with_action && action_text) ? action_text : "";
    const bool has_detail = detail && *detail;
    const char* detail_safe = has_detail ? detail : "";
    const char* hide_detail_str = has_detail ? "false" : "true";
    // lv_xml_create's attrs are key/value pairs terminated by a null entry.
    const char* attrs[] = {"message",     message,         "icon_glyph",  icon_glyph,
                           "icon_color",  icon_color,      "action_text", action_text_safe,
                           "hide_action", hide_action_str, "detail",      detail_safe,
                           "hide_detail", hide_detail_str, nullptr};

    lv_obj_t* widget =
        static_cast<lv_obj_t*>(lv_xml_create(toast_stack_, "toast_notification", attrs));
    if (!widget) {
        spdlog::error("[ToastManager] Failed to create toast notification widget");
        return;
    }

    // Allocate ToastInstance in the list (stable pointer) and wire it up.
    active_.push_back(ToastInstance{});
    auto it = std::prev(active_.end());
    it->widget = widget;
    it->action_cb = with_action ? action_cb : nullptr;
    it->action_user_data = with_action ? action_user_data : nullptr;
    it->severity = severity;
    it->message = message;

    if (with_action) {
        lv_obj_t* action_btn = lv_obj_find_by_name(widget, "toast_action_btn");
        if (action_btn) {
            // Pointer to ToastInstance is stable because active_ is std::list.
            lv_obj_add_event_cb(action_btn, action_btn_clicked, LV_EVENT_CLICKED, &*it);
        } else {
            spdlog::warn("[ToastManager] action toast missing toast_action_btn widget");
        }
    }

    animate_entrance(widget);

    it->dismiss_timer = lv_timer_create(dismiss_timer_cb, duration_ms, widget);
    if (it->dismiss_timer)
        lv_timer_set_repeat_count(it->dismiss_timer, 1);

    update_notification_bell();

    if (severity == ToastSeverity::ERROR) {
        SoundManager::instance().play("error_tone", SoundPriority::EVENT);
    }

    spdlog::debug("[ToastManager] Toast shown: [{}] {}{}{} ({}ms, action={}, stack={})",
                  severity_to_string(severity), message, has_detail ? " | " : "", detail_safe,
                  duration_ms, with_action, active_.size());
}

// ============================================================================
// Remove / dismiss
// ============================================================================

void ToastManager::detach_from_input(lv_obj_t* widget) {
    if (!widget)
        return;

    // A toast being torn down may be an input device's cached press target.
    // force_remove()/finalize_remove() hand the widget to
    // safe_delete_deferred_raw(), which reparents it to lv_layer_top() and
    // async-deletes it; in the window before that delete runs, indev's act_obj
    // still points at the (soon-to-be-freed) action button. A release/click
    // dispatched in that window dereferences freed widget memory in
    // event_send_core (target->spec_attr->event_list) — SIGBUS (#850 pattern;
    // crash bundle A5V73UV4). lv_indev_reset(NULL, ...) clears the cached
    // press/scroll target on every pointer device; it is a no-op unless a press
    // is actually in flight, so an idle auto-dismiss costs nothing.
    lv_indev_reset(nullptr, widget);

    // Belt-and-suspenders: a dying toast's action button must not latch a fresh
    // press once teardown has begun (CLICKABLE gates indev hit-testing).
    if (lv_obj_t* action_btn = lv_obj_find_by_name(widget, "toast_action_btn")) {
        lv_obj_remove_flag(action_btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

void ToastManager::begin_exit(ToastList::iterator it) {
    if (it == active_.end() || it->is_exiting)
        return;

    it->is_exiting = true;

    // Detach from input before any (possibly synchronous) deletion path.
    detach_from_input(it->widget);
    if (it->dismiss_timer) {
        lv_timer_delete(it->dismiss_timer);
        it->dismiss_timer = nullptr;
    }

    // Detach from focus group so LVGL can't auto-focus a dying toast.
    if (lv_group_get_default()) {
        lv_group_remove_obj(it->widget);
    }
    animate_exit(it->widget);
}

void ToastManager::force_remove(ToastList::iterator it) {
    if (it == active_.end())
        return;
    detach_from_input(it->widget);
    if (it->dismiss_timer) {
        lv_timer_delete(it->dismiss_timer);
        it->dismiss_timer = nullptr;
    }
    if (it->widget) {
        lv_anim_delete(it->widget, nullptr);
        uint32_t child_count = lv_obj_get_child_count(it->widget);
        for (uint32_t i = 0; i < child_count; i++) {
            lv_obj_t* child = lv_obj_get_child(it->widget, static_cast<int32_t>(i));
            if (child)
                lv_anim_delete(child, nullptr);
        }
        // safe_delete_deferred_raw reparents to lv_layer_top before async
        // delete, so the toast survives if toast_stack_ is torn down first.
        helix::ui::safe_delete_deferred_raw(it->widget);
    }
    active_.erase(it);
    update_notification_bell();
}

void ToastManager::finalize_remove(lv_obj_t* widget) {
    auto it = find_by_widget(widget);
    if (it == active_.end())
        return;

    // Defense in depth: the dismiss timer should already be cancelled by
    // begin_exit before we get here, but null any leftover so a mis-routed
    // caller can't fire dismiss_timer_cb with a freed widget.
    if (it->dismiss_timer) {
        lv_timer_delete(it->dismiss_timer);
        it->dismiss_timer = nullptr;
    }
    if (it->widget) {
        // Reparenting + async delete — see L081 and CLAUDE.md § "No sync
        // widget deletion in queued callbacks" (#776/#190/#80).
        helix::ui::safe_delete_deferred_raw(it->widget);
    }
    active_.erase(it);
    update_notification_bell();
}

void ToastManager::update_notification_bell() {
    // Recompute count + severity color together from the current history.
    // Single source of truth shared with the notification-create paths.
    helix::ui::notification_refresh_from_history();
}

// ============================================================================
// Callbacks
// ============================================================================

void ToastManager::dismiss_timer_cb(lv_timer_t* timer) {
    auto* widget = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
    if (!widget)
        return;
    auto& mgr = ToastManager::instance();
    mgr.begin_exit(mgr.find_by_widget(widget));
}

void ToastManager::close_btn_clicked(lv_event_t* e) {
    // event_cb is registered via XML; target is the close button. Walk up to
    // toast_root (the lv_xml_create return) and look up by widget pointer.
    lv_obj_t* node = lv_event_get_target_obj(e);
    auto& mgr = ToastManager::instance();
    while (node) {
        auto it = mgr.find_by_widget(node);
        if (it != mgr.active_.end()) {
            mgr.begin_exit(it);
            return;
        }
        node = lv_obj_get_parent(node);
    }
}

void ToastManager::action_btn_clicked(lv_event_t* e) {
    auto* inst = static_cast<ToastInstance*>(lv_event_get_user_data(e));
    if (!inst)
        return;

    auto& mgr = ToastManager::instance();
    // Snapshot callback before dismissing (find_by_widget may re-enter).
    toast_action_callback_t cb = inst->action_cb;
    void* data = inst->action_user_data;
    inst->action_cb = nullptr;
    inst->action_user_data = nullptr;

    mgr.begin_exit(mgr.find_by_widget(inst->widget));

    // Run the user action OUTSIDE this input-dispatch frame. Actions commonly
    // navigate or tear down panels; doing that synchronously while LVGL is
    // still iterating this button's event list invites re-entrant deletion of
    // the widget mid-dispatch. Defer to the next UpdateQueue tick — cb/data are
    // POD snapshots, so this stays valid even after the toast is gone.
    if (cb) {
        helix::ui::queue_update("ToastManager::action_cb", [cb, data]() { cb(data); });
    }
}
