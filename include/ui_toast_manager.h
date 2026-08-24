// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"

#include <atomic>
#include <cstdint>
#include <list>
#include <string>

/**
 * @brief Toast notification severity levels
 */
enum class ToastSeverity {
    INFO,    ///< Informational message (blue)
    SUCCESS, ///< Success message (green)
    WARNING, ///< Warning message (orange)
    ERROR    ///< Error message (red)
};

/**
 * @brief Callback type for toast action button
 */
typedef void (*toast_action_callback_t)(void* user_data);

/**
 * @brief Singleton manager for toast notifications
 *
 * Stacks multiple simultaneous toasts in the top-right of the screen. Each
 * toast owns its own dismiss timer and action callback so the stack can
 * dismiss in any order. The stack height is capped at MAX_VISIBLE to protect
 * small screens — overflow silently drops the oldest toast (no queueing).
 */
class ToastManager {
  public:
    static ToastManager& instance();

    ToastManager(const ToastManager&) = delete;
    ToastManager& operator=(const ToastManager&) = delete;
    ToastManager(ToastManager&&) = delete;
    ToastManager& operator=(ToastManager&&) = delete;

    /** Initialize the toast system. Call once at app startup. */
    void init();

    /** Show a toast; does not replace existing toasts (stacks instead). */
    void show(ToastSeverity severity, const char* message, uint32_t duration_ms = 4000);

    /**
     * Show a toast carrying a second, smaller line under the message: what the
     * user should DO about it.
     *
     * Separate entry point rather than a defaulted parameter on show(), so no
     * existing caller changes shape and a literal 0 duration can never become
     * ambiguous with a null detail. An empty or null @p detail renders exactly
     * as show() would.
     *
     * Longer default duration than show(): a two-part message is roughly twice
     * the reading time, and the whole point of the detail line is that the user
     * acts on it rather than glancing past it.
     */
    void show_with_detail(ToastSeverity severity, const char* message, const char* detail,
                          uint32_t duration_ms = 8000);

    /**
     * Show a toast with an action button. Each toast has its own callback, so
     * multiple action toasts can coexist in the stack.
     */
    void show_with_action(ToastSeverity severity, const char* message, const char* action_text,
                          toast_action_callback_t action_callback, void* user_data,
                          uint32_t duration_ms = 5000);

    /** Dismiss all currently visible toasts (animated). */
    void hide();

    /** Deinit LVGL resources; safe to call before lv_deinit. */
    void deinit_subjects();

    /** True if at least one toast is visible. */
    bool is_visible() const;

    /** Whether init() has completed. Callers before phase 9d should route
     *  through PendingStartupWarnings — see ui_notification.cpp. Atomic
     *  because ui_notification may read this from background threads. */
    bool is_initialized() const {
        return initialized_.load(std::memory_order_acquire);
    }

  private:
    ToastManager() = default;
    ~ToastManager();

    struct ToastInstance {
        lv_obj_t* widget = nullptr;
        lv_timer_t* dismiss_timer = nullptr;
        toast_action_callback_t action_cb = nullptr;
        void* action_user_data = nullptr;
        bool is_exiting = false;
        ToastSeverity severity = ToastSeverity::INFO;
        std::string message; // for dedupe of rapid identical toasts
    };
    using ToastList = std::list<ToastInstance>; // std::list → stable pointers

    void create_toast_internal(ToastSeverity severity, const char* message, const char* detail,
                               uint32_t duration_ms, bool with_action,
                               toast_action_callback_t action_cb, void* action_user_data,
                               const char* action_text);
    void ensure_stack_container();
    ToastList::iterator find_by_widget(lv_obj_t* widget);
    /** If a non-exiting, non-action toast with identical severity+message is
     *  visible, reset its dismiss timer and return true (caller skips
     *  creating a duplicate). Rapid-fire error paths (jog spam, reconnect
     *  storms) otherwise stack N identical widgets in one queue drain.
     *
     *  Defined inline (header-only): mk/tests.mk excludes
     *  ui_toast_manager.o from the test build and tests/ui_test_utils.cpp
     *  supplies a stub ToastManager for the rest of the class, so an
     *  out-of-line definition here would need a second copy in the stub —
     *  and the unit test would end up exercising that copy, not this one.
     *  Keeping the one real implementation in the header means both the
     *  app binary and the test binary link the same code; only LVGL's
     *  lv_timer_reset() is needed, which is available in both. Do not
     *  move this back to ui_toast_manager.cpp or add a stub override. */
    bool refresh_duplicate(ToastSeverity severity, const char* message) {
        if (!message) {
            return false;
        }
        for (auto& t : active_) {
            if (!t.is_exiting && t.action_cb == nullptr && t.severity == severity &&
                t.message == message) {
                if (t.dismiss_timer) {
                    lv_timer_reset(t.dismiss_timer);
                }
                return true;
            }
        }
        return false;
    }
    void begin_exit(ToastList::iterator it);
    void force_remove(ToastList::iterator it); // no animation
    void finalize_remove(lv_obj_t* widget);    // called from exit-anim completion

    // Detach a toast that is being torn down from any input device that has it
    // (or a child) as a cached press/scroll target, and make its action button
    // unclickable. Prevents a stale release/click from dispatching to the
    // soon-to-be-freed widget — SIGBUS in event_send_core (#850; bundle
    // A5V73UV4).
    static void detach_from_input(lv_obj_t* widget);
    void update_notification_bell();
    size_t visible_count() const; // active_ minus those already exiting

    void animate_entrance(lv_obj_t* toast);
    void animate_exit(lv_obj_t* toast);
    static void exit_animation_complete_cb(lv_anim_t* anim);

    static void dismiss_timer_cb(lv_timer_t* timer);
    static void close_btn_clicked(lv_event_t* e);
    static void action_btn_clicked(lv_event_t* e);

    // Hard cap — further bounded at first show_by screen height.
    static constexpr size_t MAX_VISIBLE = 5;

    lv_obj_t* toast_stack_ = nullptr;
    ToastList active_;
    size_t max_visible_ = MAX_VISIBLE;
    std::atomic<bool> initialized_{false};

    friend class ToastManagerTestAccess;
};

namespace helix {
namespace ui {

/**
 * @brief Show the single shared "feature unavailable on this display" toast.
 *
 * One reusable call site for every excluded-subsystem affordance on the ESP32
 * v1 (K-Touch) build. The message text is the single translatable string
 * `lv_tr("Not yet available on this display")` so translation extraction picks
 * it up exactly once. Safe to call on any build; on desktop it is simply never
 * reached because its callers are gated behind `#if defined(HELIX_PLATFORM_ESP32)`.
 */
void show_feature_unavailable_toast();

} // namespace ui
} // namespace helix
