// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_test_utils.h"

#include "ui_modal.h"

#include "core/update_queue.h"
#include "env_knobs.h"
#include "lib/lvgl/src/misc/lv_timer_private.h"
#include "platform_info.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using namespace helix;
using namespace helix::ui;

// ============================================================================
// LVGL safe initialization (idempotent)
// ============================================================================

void lv_init_safe() {
    if (!lv_is_initialized()) {
        lv_init();
        lv_xml_init(); // Must be called after lv_init() — LVGL 9.5 removed XML from core
    }
    // Ensure UpdateQueue accepts callbacks. A prior test in this shard may
    // have called shutdown() (e.g. LVGLTestFixture dtor), which blocks
    // queue() until init() is called again. This is idempotent — if already
    // initialized, init() returns immediately.
    helix::ui::UpdateQueue::instance().init();
}

void ensure_displays_never_block_on_flush() {
    if (!lv_is_initialized()) {
        return;
    }
    for (lv_display_t* d = lv_display_get_next(nullptr); d != nullptr; d = lv_display_get_next(d)) {
        lv_display_set_flush_wait_cb(d, [](lv_display_t*) {});
    }
}

uint32_t lv_timer_handler_safe() {
    // Drain the UpdateQueue — executes pending callbacks which set subjects.
    // Subject observers fire synchronously during drain, propagating bindings.
    helix::ui::UpdateQueue::instance().drain();

    // Pause ALL timers to prevent infinite handler loops, then selectively
    // execute one-shot timers (lv_async_call, retry timers) manually.
    //
    // Background: LVGL's test fixture leaks display refresh timers with stale
    // last_run timestamps. When lv_timer_handler()'s do-while loop processes
    // them all simultaneously, any timer fire that creates/deletes a timer
    // restarts the loop from the head — infinite loop.
    lv_timer_t* t = lv_timer_get_next(nullptr);
    while (t) {
        lv_timer_pause(t);
        t = lv_timer_get_next(t);
    }

    // Step animations explicitly.
    //
    // LVGL drives them from a PERIODIC timer (repeat_count -1), which the sweep
    // above paused and the one-shot loop below skips, so before this call no
    // animation ever advanced in a test — not slowly, not at all. Anything whose
    // completion runs from an anim ready_cb simply never completed: a modal exit
    // with animations enabled left its dialog parented to the screen forever, and
    // the next lv_obj_find_by_name() walked into the outgoing subtree.
    //
    // lv_anim_refr_now() is LVGL's own entry point for this (it calls the same
    // anim_timer body) and it reads lv_tick_elaps(), which process_lvgl() is
    // already advancing. Ready callbacks may delete widgets and schedule
    // lv_async_call one-shots, so run it BEFORE the one-shot loop and those
    // deletions land in the same pump.
    lv_anim_refr_now();

    // Execute one-shot timers (repeat_count >= 1) that are ready.
    // These include lv_async_call (period=0, repeat=1) and scheduled
    // retry timers. Process in a loop since callbacks may create new ones.
    uint32_t now = lv_tick_get();
    for (int safety = 0; safety < 100; safety++) {
        bool found = false;
        t = lv_timer_get_next(nullptr);
        while (t) {
            lv_timer_t* next = lv_timer_get_next(t); // Save next before potential deletion
            if (t->repeat_count > 0 && (now - t->last_run >= t->period)) {
                if (t->timer_cb) {
                    t->last_run = now;
                    t->repeat_count--;
                    t->timer_cb(t);
                    found = true;
                    break; // Restart iteration since list may have changed
                }
            }
            t = next;
        }
        if (!found)
            break; // No more ready one-shot timers
    }

    // Reap every one-shot left at repeat_count == 0, matching lv_timer_exec()'s
    // own cleanup for an exhausted timer (lv_timer.c). The loop above's
    // readiness check requires repeat_count > 0, so once a timer reaches zero
    // it is invisible to that loop forever; without this pass it would sit in
    // LVGL's list holding its callback's user_data until the process exits.
    //
    // Read by CURRENT repeat_count on a fresh list walk, never by a pointer
    // saved before a callback ran. The common one-shot pattern (every
    // lv_async_call included) deletes itself from inside its own callback
    // (lib/lvgl/src/misc/lv_async.c#lv_async_timer_cb), so that node is
    // already gone by the time this runs; a timer freshly created in its
    // place never starts at repeat_count == 0 (lv_timer_create() defaults to
    // -1, and lv_async_call sets 1 synchronously before returning), so this
    // pass can only ever find a timer that is genuinely spent right now, and
    // deletes it in the same step it is found, with no gap where a nested
    // callback could invalidate the pointer first.
    t = lv_timer_get_next(nullptr);
    while (t) {
        lv_timer_t* next = lv_timer_get_next(t);
        if (t->repeat_count == 0) {
            if (t->auto_delete) {
                lv_timer_delete(t);
            } else {
                lv_timer_pause(t);
            }
        }
        t = next;
    }

    // Re-pause ALL timers before calling lv_timer_handler().
    //
    // One-shot callbacks above may have indirectly unpaused timers.
    // Example: lv_obj_delete_async fires → lv_obj_delete → lv_obj_destructor
    // → lv_anim_delete(obj, NULL) → anim_mark_list_change() →
    // lv_timer_resume(animation_timer).  If the animation timer enters
    // lv_timer_handler() unpaused with a stale last_run timestamp, its
    // do-while loop restarts on every timer create/delete and never terminates.
    t = lv_timer_get_next(nullptr);
    while (t) {
        lv_timer_pause(t);
        t = lv_timer_get_next(t);
    }

    // Call lv_timer_handler() with all timers paused (no-op, just updates state)
    uint32_t result = lv_timer_handler();

    // Resume all timers
    t = lv_timer_get_next(nullptr);
    while (t) {
        lv_timer_resume(t);
        t = lv_timer_get_next(t);
    }

    return result;
}

namespace UITest {

// Virtual input device for simulating touches/clicks
static lv_indev_t* virtual_indev = nullptr;
static lv_indev_data_t last_data;

// Input device read callback
static void virtual_indev_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;
    // Copy last simulated input state
    *data = last_data;
}

void init(lv_obj_t* screen) {
    (void)screen; // Screen parameter reserved for future use

    if (virtual_indev) {
        spdlog::warn("[UITest] Already initialized");
        return;
    }

    spdlog::info("[UITest] Initializing virtual input device");

    // Initialize last_data
    last_data.point.x = 0;
    last_data.point.y = 0;
    last_data.state = LV_INDEV_STATE_RELEASED;

    // Create virtual input device
    virtual_indev = lv_indev_create();
    lv_indev_set_type(virtual_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(virtual_indev, virtual_indev_read_cb);

    spdlog::info("[UITest] Virtual input device created");
}

void cleanup() {
    if (virtual_indev) {
        // Note: Don't call lv_indev_delete() - lv_deinit() handles cleanup
        // Just null our reference so we don't use a stale pointer
        virtual_indev = nullptr;
        spdlog::info("[UITest] Virtual input device reference cleared");
    }
}

bool click(lv_obj_t* widget) {
    if (!widget || !virtual_indev) {
        spdlog::error("[UITest] Invalid widget or input device not initialized");
        return false;
    }

    // Get widget center coordinates
    int32_t x = lv_obj_get_x(widget) + lv_obj_get_width(widget) / 2;
    int32_t y = lv_obj_get_y(widget) + lv_obj_get_height(widget) / 2;

    // Convert to absolute coordinates if widget has parent
    lv_obj_t* parent = lv_obj_get_parent(widget);
    while (parent) {
        x += lv_obj_get_x(parent);
        y += lv_obj_get_y(parent);
        parent = lv_obj_get_parent(parent);
    }

    return click_at(x, y);
}

bool press_at(int32_t x, int32_t y) {
    if (!virtual_indev) {
        spdlog::error("[UITest] Input device not initialized - call init() first");
        return false;
    }

    spdlog::debug("[UITest] Simulating press at ({}, {})", x, y);
    last_data.point.x = x;
    last_data.point.y = y;
    last_data.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(virtual_indev); // Directly read indev to process press
    advance_ms(50);               // Minimum press duration
    return true;
}

bool move_to(int32_t x, int32_t y) {
    if (!virtual_indev) {
        spdlog::error("[UITest] Input device not initialized - call init() first");
        return false;
    }

    spdlog::debug("[UITest] Simulating move to ({}, {})", x, y);
    last_data.point.x = x;
    last_data.point.y = y;
    lv_indev_read(virtual_indev);
    advance_ms(50);
    return true;
}

bool release() {
    if (!virtual_indev) {
        spdlog::error("[UITest] Input device not initialized - call init() first");
        return false;
    }

    spdlog::debug("[UITest] Simulating release");
    last_data.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(virtual_indev); // Directly read indev to process release
    advance_ms(50);               // Allow click handlers to execute
    return true;
}

bool click_at(int32_t x, int32_t y) {
    return press_at(x, y) && release();
}

bool type_text(const std::string& text) {
    // Get focused textarea
    lv_obj_t* focused = lv_group_get_focused(lv_group_get_default());
    if (!focused) {
        spdlog::error("[UITest] No focused textarea");
        return false;
    }

    // Check if it's a textarea
    if (!lv_obj_check_type(focused, &lv_textarea_class)) {
        spdlog::error("[UITest] Focused widget is not a textarea");
        return false;
    }

    spdlog::debug("[UITest] Typing text: {}", text);

    // Add text directly to textarea
    lv_textarea_add_text(focused, text.c_str());
    lv_timer_handler_safe();
    advance_ms(50); // Allow text processing

    return true;
}

bool type_text(lv_obj_t* textarea, const std::string& text) {
    if (!textarea) {
        spdlog::error("[UITest] Invalid textarea");
        return false;
    }

    // Check if it's a textarea
    if (!lv_obj_check_type(textarea, &lv_textarea_class)) {
        spdlog::error("[UITest] Widget is not a textarea");
        return false;
    }

    spdlog::debug("[UITest] Typing text into textarea: {}", text);

    // Add text directly to textarea
    lv_textarea_add_text(textarea, text.c_str());
    lv_timer_handler_safe();
    advance_ms(50); // Allow text processing

    return true;
}

bool send_key(uint32_t key) {
    // Get focused textarea
    lv_obj_t* focused = lv_group_get_focused(lv_group_get_default());
    if (!focused) {
        spdlog::error("[UITest] No focused widget");
        return false;
    }

    spdlog::debug("[UITest] Sending key: {}", key);

    // For special keys (Enter, Backspace, etc.), use appropriate LVGL functions
    if (lv_obj_check_type(focused, &lv_textarea_class)) {
        if (key == LV_KEY_BACKSPACE) {
            lv_textarea_delete_char(focused);
        } else if (key == LV_KEY_ENTER) {
            // Trigger READY event on textarea
            lv_obj_send_event(focused, LV_EVENT_READY, nullptr);
        }
        lv_timer_handler_safe();
        advance_ms(50);
        return true;
    }

    spdlog::warn("[UITest] send_key() only supports textarea widgets");
    return false;
}

// The test binary creates its display with a bare lv_display_create() — no
// driver, so nothing ever calls lv_tick_set_cb() and lv_tick_get() returns only
// what lv_tick_inc() has been fed. A wait that sleeps on the real clock without
// advancing the tick leaves LVGL frozen: lv_timer_handler_safe() compares
// `now - last_run >= period`, so zero-period one-shots (lv_async_call) still
// fire, but any timer with a real period never comes due, however long you wait.
// wait_ms() and wait_until() below therefore advance the virtual clock in step
// with the sleep; advance_ms() advances it without sleeping at all.
static constexpr uint32_t kWaitPollMs = 5;

bool advance_until(const std::function<bool()>& condition, uint32_t budget_ms, uint32_t step_ms) {
    if (step_ms == 0) {
        step_ms = 1;
    }
    for (uint32_t elapsed = 0;;) {
        lv_tick_inc(step_ms);
        elapsed += step_ms;
        // Drains the UpdateQueue, normalizes timer timestamps, and pauses the
        // queue timer during lv_timer_handler() to prevent infinite loops.
        lv_timer_handler_safe();
        if (condition()) {
            return true;
        }
        if (elapsed >= budget_ms) {
            return false;
        }
    }
}

void advance_ms(uint32_t ms) {
    if (ms > 0) {
        advance_until([] { return false; }, ms, kWaitPollMs);
    }
}

void wait_ms(uint32_t ms) {
    auto start = std::chrono::steady_clock::now();
    auto end = start + std::chrono::milliseconds(ms);

    while (std::chrono::steady_clock::now() < end) {
        lv_tick_inc(kWaitPollMs);
        lv_timer_handler_safe(); // Process LVGL tasks
        std::this_thread::sleep_for(std::chrono::milliseconds(kWaitPollMs));
    }
}

bool wait_until(std::function<bool()> condition, uint32_t timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    auto end = start + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < end) {
        lv_tick_inc(kWaitPollMs);
        lv_timer_handler_safe(); // Process LVGL tasks

        if (condition()) {
            return true; // Condition met
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kWaitPollMs));
    }

    spdlog::warn("[UITest] wait_until() timed out after {}ms", timeout_ms);
    return false; // Timeout
}

bool wait_for_visible(lv_obj_t* widget, uint32_t timeout_ms) {
    if (!widget) {
        spdlog::error("[UITest] Invalid widget");
        return false;
    }

    return wait_until([widget]() { return !lv_obj_has_flag(widget, LV_OBJ_FLAG_HIDDEN); },
                      timeout_ms);
}

bool wait_for_hidden(lv_obj_t* widget, uint32_t timeout_ms) {
    if (!widget) {
        spdlog::error("[UITest] Invalid widget");
        return false;
    }

    return wait_until([widget]() { return lv_obj_has_flag(widget, LV_OBJ_FLAG_HIDDEN); },
                      timeout_ms);
}

bool wait_for_timers(uint32_t timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    auto end = start + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < end) {
        uint32_t next_timer = lv_timer_handler_safe();

        // If next timer is in the far future (> 1 second), no active timers
        if (next_timer > 1000) {
            spdlog::debug("[UITest] All timers completed");
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    spdlog::warn("[UITest] wait_for_timers() timed out after {}ms", timeout_ms);
    return false;
}

bool is_visible(lv_obj_t* widget) {
    if (!widget) {
        return false;
    }
    return !lv_obj_has_flag(widget, LV_OBJ_FLAG_HIDDEN);
}

std::string get_text(lv_obj_t* widget) {
    if (!widget) {
        return "";
    }

    // Try label first
    if (lv_obj_check_type(widget, &lv_label_class)) {
        const char* text = lv_label_get_text(widget);
        return text ? std::string(text) : "";
    }

    // Try textarea
    if (lv_obj_check_type(widget, &lv_textarea_class)) {
        const char* text = lv_textarea_get_text(widget);
        return text ? std::string(text) : "";
    }

    spdlog::warn("[UITest] get_text() called on non-text widget");
    return "";
}

bool is_checked(lv_obj_t* widget) {
    if (!widget) {
        return false;
    }
    return lv_obj_has_state(widget, LV_STATE_CHECKED);
}

lv_obj_t* find_by_name(lv_obj_t* parent, const std::string& name) {
    if (!parent) {
        return nullptr;
    }
    return lv_obj_find_by_name(parent, name.c_str());
}

int count_children_with_marker(lv_obj_t* parent, const char* marker) {
    if (!parent || !marker) {
        return 0;
    }

    int count = 0;
    uint32_t child_count = lv_obj_get_child_count(parent);

    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(parent, i);
        if (!child)
            continue;

        const void* user_data = lv_obj_get_user_data(child);
        if (user_data && strcmp((const char*)user_data, marker) == 0) {
            count++;
        }
    }

    return count;
}

} // namespace UITest

#include "ui_notification.h"
#include "ui_toast_manager.h"

#include "app_globals.h"

namespace {

using MessageHook = std::function<void(const std::string&)>;

MessageHook g_test_warning_hook;
MessageHook g_test_sticky_warning_hook;
MessageHook g_test_error_hook;
MessageHook g_test_success_hook;
MessageHook g_test_info_hook;
std::function<void(ToastSeverity, const std::string&, const std::string&)> g_test_action_hook;
std::function<void(const helix::ui::NotificationEvent&)> g_test_notification_event_hook;

std::function<void(ToastSeverity, const std::string&, uint32_t)> g_test_toast_hook;
// The last action toast shown, which a fire consumes.
toast_action_callback_t g_last_toast_action = nullptr;
void* g_last_toast_action_data = nullptr;
// The last action toast shown and the last pair hide(action) was given, for
// last_action_toast_dismissed(). A fire leaves these alone.
toast_action_callback_t g_shown_toast_action = nullptr;
void* g_shown_toast_action_data = nullptr;
toast_action_callback_t g_shown_toast_closed_cb = nullptr;
toast_action_callback_t g_dismissed_toast_action = nullptr;
void* g_dismissed_toast_action_data = nullptr;

void call(const MessageHook& hook, const std::string& message) {
    if (hook) {
        hook(message);
    }
}

// Hands each event the real ui_notification.o reports to the hook a test
// installed for its kind.
void route_notification(const helix::ui::NotificationEvent& e) {
    if (g_test_notification_event_hook) {
        g_test_notification_event_hook(e);
    }
    if (!e.action.empty()) {
        if (g_test_action_hook) {
            g_test_action_hook(e.severity, e.message, e.action);
        }
        return;
    }
    switch (e.severity) {
    case ToastSeverity::INFO:
        call(g_test_info_hook, e.message);
        break;
    case ToastSeverity::SUCCESS:
        call(g_test_success_hook, e.message);
        break;
    case ToastSeverity::WARNING:
        call(e.sticky ? g_test_sticky_warning_hook : g_test_warning_hook, e.message);
        break;
    case ToastSeverity::ERROR:
        call(g_test_error_hook, e.message);
        break;
    }
}

// Records what the real ToastManager was asked to show or dismiss.
void route_toast(const ToastManager::Event& e) {
    if (e.kind == ToastManager::Event::Kind::HideAction) {
        g_dismissed_toast_action = e.action_cb;
        g_dismissed_toast_action_data = e.action_user_data;
        return;
    }
    if (e.action_cb) {
        g_last_toast_action = e.action_cb;
        g_last_toast_action_data = e.action_user_data;
        g_shown_toast_action = e.action_cb;
        g_shown_toast_action_data = e.action_user_data;
    }
    g_shown_toast_closed_cb = e.user_closed_cb;
    if (g_test_toast_hook) {
        // One string per toast: the detail line follows the message.
        g_test_toast_hook(e.severity, e.detail.empty() ? e.message : e.message + " - " + e.detail,
                          e.duration_ms);
    }
}

// Both observers stay installed for the life of the binary; each hook is
// empty until a test sets it.
__attribute__((constructor)) void install_test_observers() {
    helix::ui::set_notification_observer(route_notification);
    ToastManager::instance().set_observer(route_toast);
}

} // namespace

namespace helix {
namespace ui {

void set_test_notification_warning_hook(MessageHook hook) {
    g_test_warning_hook = std::move(hook);
}
void set_test_notification_sticky_warning_hook(MessageHook hook) {
    g_test_sticky_warning_hook = std::move(hook);
}
void set_test_notification_error_hook(MessageHook hook) {
    g_test_error_hook = std::move(hook);
}
void set_test_notification_info_hook(MessageHook hook) {
    g_test_info_hook = std::move(hook);
}
void set_test_notification_success_hook(MessageHook hook) {
    g_test_success_hook = std::move(hook);
}
void set_test_notification_action_hook(
    std::function<void(ToastSeverity, const std::string&, const std::string&)> hook) {
    g_test_action_hook = std::move(hook);
}

bool last_shown_toast_has_closed_cb() {
    return g_shown_toast_closed_cb != nullptr;
}

void set_test_notification_event_hook(std::function<void(const NotificationEvent&)> hook) {
    g_test_notification_event_hook = std::move(hook);
}

void clear_test_hooks() {
    g_test_warning_hook = nullptr;
    g_test_sticky_warning_hook = nullptr;
    g_test_error_hook = nullptr;
    g_test_info_hook = nullptr;
    g_test_success_hook = nullptr;
    g_test_action_hook = nullptr;
    g_test_notification_event_hook = nullptr;
    set_test_toast_hook(nullptr);
}

void set_test_toast_hook(std::function<void(ToastSeverity, const std::string&, uint32_t)> hook) {
    g_test_toast_hook = std::move(hook);
    g_last_toast_action = nullptr;
    g_shown_toast_action = nullptr;
    g_dismissed_toast_action = nullptr;
}

bool last_action_toast_dismissed() {
    return g_shown_toast_action && g_dismissed_toast_action == g_shown_toast_action &&
           g_dismissed_toast_action_data == g_shown_toast_action_data;
}

bool fire_last_toast_action() {
    toast_action_callback_t action = g_last_toast_action;
    g_last_toast_action = nullptr;
    if (!action) {
        return false;
    }
    action(g_last_toast_action_data);
    return true;
}

} // namespace ui
} // namespace helix

// Answers root_escalation_available()'s sudo probe "no" for the whole binary, so
// no test forks `sudo -n true` on the developer's machine. A run as root still
// answers yes through the euid check that precedes the probe.
__attribute__((constructor)) static void helix_disable_test_sudo_probe() {
    set_root_escalation_probe(+[]() { return false; });
}

class TemperatureHistoryManager;
void set_test_temperature_history_manager(TemperatureHistoryManager* mgr) {
    set_temperature_history_manager(mgr);
}
