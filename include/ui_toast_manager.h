// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_notification_threshold.h"
#include "ui_widget_ref.h"

#include "lvgl.h"

#include <atomic>
#include <cstdint>
#include <functional>
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
 * dismiss in any order. The stack height is capped at kMaxVisible to protect
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
     * existing caller changes shape. An empty or null @p detail renders exactly
     * as show() would.
     *
     * Longer default duration than show(): a two-part message is roughly twice
     * the reading time, and the whole point of the detail line is that the user
     * acts on it rather than glancing past it.
     *
     * @p duration_ms of 0 means the toast never auto-dismisses: it stays until
     * its close button is tapped. For a warning the user must act on (a refused
     * helixscreen.env means every setting in it is being ignored).
     */
    void show_with_detail(ToastSeverity severity, const char* message, const char* detail,
                          uint32_t duration_ms = 8000);

    /**
     * Show a toast with an action button. Each toast has its own callback, so
     * multiple action toasts can coexist in the stack.
     *
     * @p duration_ms of 0 means the toast never auto-dismisses, as for
     * show_with_detail(): it stays until its button or close button is tapped.
     *
     * @p user_closed_callback runs when the user closes this toast with its X
     * button, with @p user_data — a decision to decline the action, unlike
     * hide(), the dismiss timer or stack eviction, which say nothing. Runs on
     * the main thread once the widget is removed.
     */
    void show_with_action(ToastSeverity severity, const char* message, const char* action_text,
                          toast_action_callback_t action_callback, void* user_data,
                          uint32_t duration_ms = 5000,
                          toast_action_callback_t user_closed_callback = nullptr);

    /** Dismiss all currently visible toasts (animated). */
    void hide();

    /** Dismiss only the action toasts whose button runs @p action_callback on
     *  @p user_data (animated): the pair show_with_action() was given. For a
     *  sticky toast whose question was answered somewhere else. */
    void hide(toast_action_callback_t action_callback, void* user_data);

    /** Deinit LVGL resources; safe to call before lv_deinit. */
    void deinit_subjects();

    /** True if at least one toast is visible. */
    bool is_visible() const;

    /**
     * What one show*() or hide(action) call asked for, reported on the calling
     * thread before the toast is queued or looked up.
     */
    struct Event {
        enum class Kind {
            Show,      ///< show(), show_with_detail() or show_with_action()
            HideAction ///< hide(action_callback, user_data)
        };
        Kind kind = Kind::Show;
        ToastSeverity severity = ToastSeverity::INFO;
        std::string message;
        std::string detail;
        uint32_t duration_ms = 0;
        toast_action_callback_t action_cb = nullptr;
        void* action_user_data = nullptr;
        toast_action_callback_t user_closed_cb = nullptr;
    };

    /** Install the one observer every Event goes to; an empty function removes
     *  it. The test binary reads what code under test asked to show through it.
     *  Not synchronized: install it before the calls it should see. */
    void set_observer(std::function<void(const Event&)> observer) {
        observer_ = std::move(observer);
    }

    /** Before init(), queue @p text for the drain that follows init() and
     *  return true; after it, return false and queue nothing. Thread-safe. */
    bool defer_until_initialized(ToastSeverity severity, const char* text, uint32_t duration_ms);

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
        helix::ui::WeakWidget widget;
        lv_timer_t* dismiss_timer = nullptr;
        toast_action_callback_t action_cb = nullptr;
        void* action_user_data = nullptr;
        toast_action_callback_t user_closed_cb = nullptr;
        bool closed_by_user = false;
        bool is_exiting = false;
        ToastSeverity severity = ToastSeverity::INFO;
        std::string message; // for dedupe of rapid identical toasts
    };
    using ToastList = std::list<ToastInstance>; // std::list → stable pointers

    void create_toast_internal(ToastSeverity severity, const char* message, const char* detail,
                               uint32_t duration_ms, bool with_action,
                               toast_action_callback_t action_cb, void* action_user_data,
                               const char* action_text, toast_action_callback_t user_closed_cb);
    /** The X button's removal: a user decision, unlike hide() or a timer. */
    void user_close_toast(ToastList::iterator it);
    void ensure_stack_container();
    ToastList::iterator find_by_widget(lv_obj_t* widget);
    /** If a non-exiting toast with identical severity+message is visible,
     *  reset its dismiss timer and return true (caller skips creating a
     *  duplicate). Rapid-fire error paths (jog spam, reconnect storms, a
     *  flapping sensor re-raising one notice) otherwise stack N identical
     *  widgets. An action toast only folds into one whose button runs the
     *  same callback on the same user_data: two toasts with one message but
     *  different targets are two questions, and merging them would drop one. */
    bool refresh_duplicate(ToastSeverity severity, const char* message,
                           toast_action_callback_t action_cb = nullptr,
                           void* action_user_data = nullptr) {
        if (!message) {
            return false;
        }
        for (auto& t : active_) {
            if (!t.is_exiting && t.action_cb == action_cb &&
                t.action_user_data == action_user_data && t.severity == severity &&
                t.message == message) {
                if (t.dismiss_timer) {
                    lv_timer_reset(t.dismiss_timer);
                }
                return true;
            }
        }
        return false;
    }
    /** Resolve the toast that owns @p node: @p node itself or any descendant
     *  of a widget in active_. Returns active_.end() when the node lives
     *  outside every registered toast — the safe no-op answer for event
     *  callbacks that can fire after their toast's list entry is gone
     *  (deinit_subjects runs on printer switches, not only before lv_deinit). */
    ToastList::iterator find_owning_toast(lv_obj_t* node) {
        while (node) {
            for (auto it = active_.begin(); it != active_.end(); ++it) {
                if (it->widget == node)
                    return it;
            }
            node = lv_obj_get_parent(node);
        }
        return active_.end();
    }
    void begin_exit(ToastList::iterator it);
    template <typename Pred> void hide_where(Pred pred);
    void force_remove(ToastList::iterator it); // no animation
    void finalize_remove(lv_obj_t* widget);    // called from exit-anim completion

    // Take a toast that is being torn down away from any pointer pressing or
    // scrolling inside it, and make its action button unclickable, so no press
    // on the button completes into a dismissed toast (#850; bundle A5V73UV4).
    // A press anywhere else keeps its gesture.
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
    static constexpr size_t kMaxVisible = 5;

    helix::ui::WidgetRef toast_stack_;
    ToastList active_;
    size_t max_visible_ = kMaxVisible;
    std::atomic<bool> initialized_{false};
    std::function<void(const Event&)> observer_;

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

namespace notifications {

/**
 * @brief Success toast for good news that is not history: no history row, no
 *        bell count.
 *
 * Honours the user's minimum toast severity, and drops the toast before
 * ToastManager::init() since there is no history row to fall back on.
 * Main thread only.
 */
inline void show_transient_success(const char* text, uint32_t duration_ms) {
    auto& toasts = ToastManager::instance();
    if (!toasts.is_initialized() ||
        !severity_meets_threshold(static_cast<int>(ToastSeverity::SUCCESS),
                                  get_min_toast_severity_cache())) {
        return;
    }
    toasts.show(ToastSeverity::SUCCESS, text, duration_ms);
}

} // namespace notifications
} // namespace ui
} // namespace helix
