// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_test_utils.h"

#include "ui_modal.h"
#include "ui_update_queue.h"

#include "lib/lvgl/src/misc/lv_timer_private.h"
#include "platform_info.h"
#include "spdlog/spdlog.h"
#include "test_helpers/update_queue_test_access.h"

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

void ensure_headless_display() {
    static bool created = false;
    if (created) {
        return;
    }
    auto* disp = lv_display_create(480, 320);
    alignas(64) static lv_color_t buf[480 * 10];
    lv_display_set_buffers(disp, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(
        disp, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    created = true;
}

uint32_t lv_timer_handler_safe() {
    // Drain the UpdateQueue — executes pending callbacks which set subjects.
    // Subject observers fire synchronously during drain, propagating bindings.
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

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
                    if (t->repeat_count > 0) {
                        t->repeat_count--;
                    }
                    const bool exhausted = t->repeat_count == 0;
                    t->timer_cb(t);
                    // Match lv_timer_handler(): it deletes a timer whose repeat
                    // count reached 0 (lv_timer.c:369). Leaving it behind parks
                    // a spent lv_timer_t in LVGL's list holding the callback's
                    // user_data, so an owner destroyed later cannot free what it
                    // no longer has a handle to. Re-find it rather than reusing
                    // `t`: the callback may already have deleted it (the common
                    // one-shot pattern nulls its own handle and returns).
                    if (exhausted) {
                        for (lv_timer_t* s = lv_timer_get_next(nullptr); s != nullptr;
                             s = lv_timer_get_next(s)) {
                            if (s == t) {
                                lv_timer_delete(t);
                                break;
                            }
                        }
                    }
                    found = true;
                    break; // Restart iteration since list may have changed
                }
            }
            t = next;
        }
        if (!found)
            break; // No more ready one-shot timers
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
    wait_ms(50);                  // Minimum press duration
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
    wait_ms(50);                  // Allow click handlers to execute
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
    wait_ms(50); // Allow text processing

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
    wait_ms(50); // Allow text processing

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
        wait_ms(50);
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
// Both waits below therefore advance the virtual clock in step with the sleep.
static constexpr uint32_t WAIT_POLL_MS = 5;

void wait_ms(uint32_t ms) {
    auto start = std::chrono::steady_clock::now();
    auto end = start + std::chrono::milliseconds(ms);

    while (std::chrono::steady_clock::now() < end) {
        lv_tick_inc(WAIT_POLL_MS);
        lv_timer_handler_safe(); // Process LVGL tasks
        std::this_thread::sleep_for(std::chrono::milliseconds(WAIT_POLL_MS));
    }
}

bool wait_until(std::function<bool()> condition, uint32_t timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    auto end = start + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < end) {
        lv_tick_inc(WAIT_POLL_MS);
        lv_timer_handler_safe(); // Process LVGL tasks

        if (condition()) {
            return true; // Condition met
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(WAIT_POLL_MS));
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

// Stub implementations for main.cpp functions needed by wizard tests
// These return nullptr since wizard tests don't actually use Moonraker
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#ifdef HELIX_ENABLE_MOCKS
#include "moonraker_client_mock.h"
#endif
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "temperature_controller.h"

// Settable global client pointer — mirrors the real app_globals.cpp semantics.
// Defaults to nullptr (most UI tests don't touch Moonraker); tests that exercise
// real callback routing can swap in a MoonrakerClientMock via
// set_moonraker_client() and restore it in their dtor.
static helix::IMoonrakerClient* g_test_moonraker_client = nullptr;

#ifdef HELIX_ENABLE_MOCKS
// Mirrors app_globals.cpp: the same pointer under its concrete mock type, so
// consumers reaching for the mock-only API don't have to downcast the
// interface. A test that hands a MoonrakerClientMock to code which subscribes
// to simulator notifications must publish it here too.
static MoonrakerClientMock* g_test_moonraker_client_mock = nullptr;
#endif

IMoonrakerClient* get_moonraker_client() {
    return g_test_moonraker_client;
}

void set_moonraker_client(IMoonrakerClient* client) {
    g_test_moonraker_client = client;
#ifdef HELIX_ENABLE_MOCKS
    if (static_cast<IMoonrakerClient*>(g_test_moonraker_client_mock) != client) {
        g_test_moonraker_client_mock = nullptr;
    }
#endif
}

#ifdef HELIX_ENABLE_MOCKS
MoonrakerClientMock* get_moonraker_client_mock() {
    return g_test_moonraker_client_mock;
}

void set_moonraker_client_mock(MoonrakerClientMock* client) {
    g_test_moonraker_client_mock = client;
}
#endif

// Settable for the same reason as the client above: code under test reaches the
// API through this global, so a test that needs it to answer installs its own and
// restores the previous value in a dtor. Defaults to nullptr, which is what every
// test that does not care has always seen.
static IMoonrakerAPI* g_test_moonraker_api = nullptr;

IMoonrakerAPI* get_moonraker_api() {
    return g_test_moonraker_api;
}

void set_moonraker_api(IMoonrakerAPI* api) {
    g_test_moonraker_api = api;
}

PrinterState& get_printer_state() {
    static PrinterState instance;
    return instance;
}

helix::TemperatureController* get_temperature_controller() {
    return helix::PanelWidgetManager::instance().shared_resource<helix::TemperatureController>();
}

// Optional hook installed by tests to observe warning toasts (the UI is stubbed
// out in the test build, so warnings would otherwise be invisible).
namespace {
std::function<void(const std::string&)> g_test_warning_hook;
std::function<void(const std::string&)> g_test_error_hook;
std::function<void(const std::string&)> g_test_info_hook;
} // namespace

namespace helix {
namespace ui {
void set_test_notification_warning_hook(std::function<void(const std::string&)> hook) {
    g_test_warning_hook = std::move(hook);
}
void set_test_notification_error_hook(std::function<void(const std::string&)> hook) {
    g_test_error_hook = std::move(hook);
}

void set_test_notification_info_hook(std::function<void(const std::string&)> hook) {
    g_test_info_hook = std::move(hook);
}
} // namespace ui
} // namespace helix

// Stub implementations for notification functions (tests don't display UI)
void ui_notification_init() {
    // No-op in tests
}

void ui_notification_info(const char* message) {
    spdlog::debug("[Test Stub] ui_notification_info: {}", message ? message : "(null)");
    if (g_test_info_hook) {
        g_test_info_hook(message ? message : "");
    }
}

void ui_notification_info(const char* title, const char* message) {
    spdlog::debug("[Test Stub] ui_notification_info: {} - {}", title ? title : "(null)",
                  message ? message : "(null)");
    if (g_test_info_hook) {
        g_test_info_hook(message ? message : "");
    }
}

void ui_notification_info_with_action(const char* title, const char* message, const char* action) {
    spdlog::debug("[Test Stub] ui_notification_info_with_action: {} - {} (action: {})",
                  title ? title : "(null)", message ? message : "(null)",
                  action ? action : "(null)");
}

void ui_notification_success(const char* message) {
    spdlog::debug("[Test Stub] ui_notification_success: {}", message ? message : "(null)");
}

void ui_notification_success(const char* title, const char* message) {
    spdlog::debug("[Test Stub] ui_notification_success: {} - {}", title ? title : "(null)",
                  message ? message : "(null)");
}

void ui_notification_warning(const char* message) {
    spdlog::debug("[Test Stub] ui_notification_warning: {}", message ? message : "(null)");
    if (g_test_warning_hook) {
        g_test_warning_hook(message ? message : "");
    }
}

void ui_notification_warning(const char* title, const char* message) {
    spdlog::debug("[Test Stub] ui_notification_warning: {} - {}", title ? title : "(null)",
                  message ? message : "(null)");
    if (g_test_warning_hook) {
        g_test_warning_hook(message ? message : "");
    }
}

void ui_notification_error(const char* title, const char* message, bool modal) {
    spdlog::debug("[Test Stub] ui_notification_error: {} - {} (modal={})", title ? title : "(null)",
                  message ? message : "(null)", modal);
    if (g_test_error_hook) {
        g_test_error_hook(message ? message : "");
    }
}

// Feeds the same hook as ui_notification_error: to a test asserting WHAT was
// surfaced, a printer fault is an error notification. What it cannot reproduce
// is the real function's call to helix::ui::track_fault_modal() — no modal is
// built here at all. Coverage of the sweep therefore drives the registry
// directly (test_fault_modal_dismiss.cpp), which is the linked, real code.
void ui_notification_printer_fault(const char* title, const char* message) {
    spdlog::debug("[Test Stub] ui_notification_printer_fault: {} - {}", title ? title : "(null)",
                  message ? message : "(null)");
    if (g_test_error_hook) {
        g_test_error_hook(message ? message : "");
    }
}

// The two-line variants hand the hook the SAME text the toast renders, joined,
// so a test can assert that the suggestion actually reached the user instead of
// only that the message did. Matches ui_notification.cpp's history-row join.
static std::string join_detail(const char* message, const char* detail) {
    std::string joined = message ? message : "";
    if (detail && *detail) {
        joined += " - ";
        joined += detail;
    }
    return joined;
}

void ui_notification_error_with_detail(const char* message, const char* detail) {
    const std::string joined = join_detail(message, detail);
    spdlog::debug("[Test Stub] ui_notification_error_with_detail: {}", joined);
    if (g_test_error_hook) {
        g_test_error_hook(joined);
    }
}

void ui_notification_warning_with_detail(const char* message, const char* detail) {
    const std::string joined = join_detail(message, detail);
    spdlog::debug("[Test Stub] ui_notification_warning_with_detail: {}", joined);
    if (g_test_warning_hook) {
        g_test_warning_hook(joined);
    }
}

// Stub ToastManager class for tests
#include "ui_toast_manager.h"

// Fed by the ToastManager stub below, for tests asserting on direct
// ToastManager::show() calls. See set_test_toast_hook in the header.
static std::function<void(ToastSeverity, const std::string&)> g_test_toast_hook;

namespace helix {
namespace ui {

void set_test_toast_hook(std::function<void(ToastSeverity, const std::string&)> hook) {
    g_test_toast_hook = std::move(hook);
}

} // namespace ui
} // namespace helix

// Stub ToastManager class for tests
// The real ToastManager is excluded from test build, so we need a stub singleton
static ToastManager* s_test_toast_manager_instance = nullptr;

ToastManager& ToastManager::instance() {
    if (!s_test_toast_manager_instance) {
        s_test_toast_manager_instance = new ToastManager();
    }
    return *s_test_toast_manager_instance;
}

ToastManager::~ToastManager() {
    // Stub destructor
}

void ToastManager::init() {
    spdlog::debug("[Test Stub] ToastManager::init()");
}

void ToastManager::show(ToastSeverity severity, const char* message, uint32_t duration_ms) {
    (void)duration_ms;
    spdlog::debug("[Test Stub] ToastManager::show: {}", message ? message : "(null)");
    if (g_test_toast_hook) {
        g_test_toast_hook(severity, message ? message : "");
    }
}

void ToastManager::show_with_detail(ToastSeverity severity, const char* message, const char* detail,
                                    uint32_t duration_ms) {
    (void)duration_ms;
    const std::string joined = join_detail(message, detail);
    spdlog::debug("[Test Stub] ToastManager::show_with_detail: {}", joined);
    if (g_test_toast_hook) {
        g_test_toast_hook(severity, joined);
    }
}

void ToastManager::show_with_action(ToastSeverity severity, const char* message,
                                    const char* action_text,
                                    toast_action_callback_t action_callback, void* user_data,
                                    uint32_t duration_ms) {
    (void)action_text;
    (void)action_callback;
    (void)user_data;
    (void)duration_ms;
    spdlog::debug("[Test Stub] ToastManager::show_with_action: {}", message ? message : "(null)");
    if (g_test_toast_hook) {
        g_test_toast_hook(severity, message ? message : "");
    }
}

void ToastManager::hide() {
    spdlog::debug("[Test Stub] ToastManager::hide()");
}

bool ToastManager::is_visible() const {
    return false;
}

// refresh_duplicate() is NOT stubbed here — it's defined inline in
// include/ui_toast_manager.h so the test binary links the same
// implementation the real app uses. See the comment on that declaration.

// Text input widget implementation for tests
// This is a full implementation, not a stub, because tests need to actually
// test the text_input widget's placeholder and max_length attributes.
#include "ui_text_input.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_textarea_parser.h"

// Stub for notification manager functions (tests don't have notification UI)
#include "ui_notification.h"
#include "ui_notification_manager.h"
#include "ui_printer_status_icon.h"

void helix::ui::notification_update(NotificationStatus /* status */) {
    // No-op in tests
}

void helix::ui::notification_update_count(size_t /* count */) {
    // No-op in tests
}

// Application::tear_down_printer_state() calls both of these; ui_notification.o and
// ui_notification_manager.o stay out of the test link (see mk/tests.mk Group 2).
void ui_notification_deinit() {
    spdlog::debug("[Test Stub] ui_notification_deinit: no-op in tests");
}

void helix::ui::notification_refresh_from_history() {
    // No-op in tests
}

// Stub for app_request_restart (tests don't restart)
void app_request_restart() {
    spdlog::debug("[Test Stub] app_request_restart called - no-op in tests");
}

// Stub for app_request_restart_service (tests don't restart)
void app_request_restart_service() {
    spdlog::debug("[Test Stub] app_request_restart_service called - no-op in tests");
}

// Stub for app_get_stored_argv (tests don't have real argv)
char** app_get_stored_argv() {
    return nullptr;
}

// get_helix_cache_dir() is NOT stubbed any more.
//
// It used to be a hand-written two-rung reimplementation here, which meant
// tests/unit/test_cache_dir.cpp asserted against this copy rather than the
// shipped cascade — rungs 2-7 were never exercised, and the "/tmp/helix_test_"
// fallback it returned is a shape production never produces. The real resolver
// now lives in src/system/helix_cache_dir.cpp, outside the app_globals.o that
// mk/tests.mk excludes, so the test binary links it directly.
//
// Isolation comes from HELIX_CACHE_DIR instead — rung 1 of the real cascade,
// pinned to a per-run temp dir by helix_test_cache_sandbox() below. Faking the
// function was never what kept tests off the developer's ~/.cache/helix; the
// override is, and production already provides it.
#include "app_globals.h"

// Stub for app_get_install_root (tests don't have a resolvable install layout)
// Returns empty string — matches the production fallback when the exe is not
// under a recognized /bin or /build/bin directory.
std::string app_get_install_root() {
    return "";
}

// Stub for app_get_cache_dir (production reads the cascade; tests do not need a root)
// Returns empty string — matches the production fallback when cache resolution fails.
std::string app_get_cache_dir() {
    return "";
}

// Stub for app_get_runtime_dir (tests use a writable temp dir)
std::string app_get_runtime_dir() {
    std::string path = "/tmp/helix_test_runtime";
    std::filesystem::create_directories(path);
    return path;
}

// Stub for app_get_config_dir (tests don't have a resolvable install layout)
// Returns empty string — matches the production fallback when install root is unknown.
std::string app_get_config_dir() {
    return "";
}

// app_globals.o is excluded from the test link, so mirror the real
// helix_parse_truthy_env / updates_externally_managed logic here (kept
// byte-identical to src/app_globals.cpp) so the update-gate tests exercise
// the genuine parse behavior rather than a hollow stub.
bool helix_parse_truthy_env(const char* value) {
    if (!value || value[0] == '\0') {
        return false;
    }
    std::string v(value);
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    v.erase(v.begin(), std::find_if(v.begin(), v.end(), not_space));
    v.erase(std::find_if(v.rbegin(), v.rend(), not_space).base(), v.end());
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool compute_updates_externally_managed(const char* disable_auto_updates, bool platform_default) {
    // An explicit flag decides it, in either direction. helix_parse_truthy_env()
    // only answers "is this truthy", which cannot distinguish "0" from unset, so
    // presence is tested separately and a falsy value force-enables self-update
    // where the platform would otherwise default it off.
    //
    // Blank counts as ABSENT, not as falsy. helixscreen.env values routinely carry
    // a stray space (which is why parsing trims), and an all-whitespace value read
    // as an explicit "no" would silently switch self-update back on for a
    // firmware-managed install — the exact failure this predicate exists to stop.
    if (disable_auto_updates) {
        const char* p = disable_auto_updates;
        while (*p && std::isspace(static_cast<unsigned char>(*p))) {
            ++p;
        }
        if (*p != '\0') {
            return helix_parse_truthy_env(disable_auto_updates);
        }
    }
    return platform_default;
}

bool updates_externally_managed() {
    static const bool cached = compute_updates_externally_managed(
        std::getenv("HELIX_DISABLE_AUTO_UPDATES"), helix::platform_defaults_to_external_updates());
    return cached;
}

// Mirror of src/app_globals.cpp compute_self_update_supported / self_update_supported /
// update_install_suppressed / update_checks_suppressed (app_globals.o is
// excluded from the test link).
//
// Keep the branch structure identical to the original, comments aside. A mirror that
// drifts turns the tests below into a test of this file: the parent-only version of
// this predicate was a false negative that hid the updater on every /opt install, and
// nothing here would have noticed, because the assertions would have been passing
// against the same wrong logic.
#include "system/helix_paths.h"

#include <unistd.h> // geteuid
bool compute_self_update_supported(const std::string& install_root, bool can_escalate) {
    if (install_root.empty()) {
        return true;
    }
    const std::string parent = std::filesystem::path(install_root).parent_path().string();
    if (!parent.empty() && helix::paths::is_writable_dir(parent)) {
        return true; // atomic swap
    }
    if (parent.empty()) {
        return true;
    }
    if (helix::paths::is_writable_dir(install_root)) {
        return true; // in-place replacement
    }
    return can_escalate;
}

// Deliberately NOT a mirror: the real probe forks `sudo -n true`, and a test
// binary must not shell out to sudo. euid 0 is the one branch that is free to
// evaluate honestly. The pure predicate above is what the update-gate tests
// exercise for both escalation values, so this stub costs no coverage.
bool root_escalation_available() {
    return geteuid() == 0;
}

bool self_update_supported() {
    static const bool cached = []() {
        const std::string root = app_get_install_root();
        if (compute_self_update_supported(root, /*can_escalate=*/false)) {
            return true;
        }
        return compute_self_update_supported(root, root_escalation_available());
    }();
    return cached;
}

bool compute_update_install_suppressed(bool externally_managed, bool self_update_ok) {
    return externally_managed || !self_update_ok;
}

bool update_install_suppressed() {
    return compute_update_install_suppressed(updates_externally_managed(), self_update_supported());
}

bool update_checks_suppressed() {
    return updates_externally_managed();
}

// Stubs for the manager accessors in app_globals.h. Each getter reads a file-static
// that its matching setter writes, so a test (or production code linked into the test
// binary, e.g. Application::tear_down_printer_state()) sees the value it installed.
// Nothing installs one by default, so the default remains nullptr.
#include "moonraker_manager.h"

static MoonrakerManager* g_test_moonraker_manager = nullptr;
MoonrakerManager* get_moonraker_manager() {
    return g_test_moonraker_manager;
}
void set_moonraker_manager(MoonrakerManager* manager) {
    g_test_moonraker_manager = manager;
}

class PrintHistoryManager;
static PrintHistoryManager* g_test_print_history_manager = nullptr;
PrintHistoryManager* get_print_history_manager() {
    return g_test_print_history_manager;
}
void set_print_history_manager(PrintHistoryManager* manager) {
    g_test_print_history_manager = manager;
}

// Tests default to no temperature history manager, but a test can install a real one
// via set_test_temperature_history_manager() to exercise history backfill paths (#1124).
class TemperatureHistoryManager;
static TemperatureHistoryManager* g_test_history_manager = nullptr;
TemperatureHistoryManager* get_temperature_history_manager() {
    return g_test_history_manager;
}
void set_temperature_history_manager(TemperatureHistoryManager* manager) {
    g_test_history_manager = manager;
}
void set_test_temperature_history_manager(TemperatureHistoryManager* mgr) {
    g_test_history_manager = mgr;
}

// get_job_queue_state defaults to nullptr (no state manager in most tests),
// but is a real settable global — same pattern as get/set_print_history_manager
// above — so a widget test can install one to exercise the rebuild path.
class JobQueueState;
static JobQueueState* g_test_job_queue_state = nullptr;
JobQueueState* get_job_queue_state() {
    return g_test_job_queue_state;
}
void set_job_queue_state(JobQueueState* state) {
    g_test_job_queue_state = state;
}

// Restart/quit plumbing from app_globals.h. main.o owns the real implementations and
// stays out of the test link, so tests get an in-process equivalent: the quit flag is
// real (app_request_quit_signal_safe() sets what app_quit_requested() reads) and
// app_store_argv() is a no-op because nothing in the test binary re-execs.
static std::atomic<bool> g_test_quit_requested{false};
bool app_quit_requested() {
    return g_test_quit_requested.load();
}
void app_request_quit_signal_safe() {
    g_test_quit_requested.store(true);
}
void app_store_argv(int /*argc*/, char** /*argv*/) {
    // No-op in tests - nothing here re-execs the binary
}

// ============================================================================
// Stubs for LVGLUITestFixture - Full UI Integration Tests
// ============================================================================
// These stubs support tests that need more complete UI initialization
// but don't need real network/hardware connections.

// Stub for app_globals_init_subjects (creates test notification + edit mode subjects)
#include "platform_info.h"

static lv_subject_t s_test_notification_subject;
static lv_subject_t s_test_home_edit_mode_subject;
static lv_subject_t s_test_wizard_active_subject;
static lv_subject_t s_test_host_power_supported_subject;
// Mirrors app_globals.cpp's g_platform_extras_subject: 1 on every non-ESP32
// build, 0 only on the ESP32 v1 cut. Registered into the XML global scope so
// bindings like btn_camera's platform_extras_available cond resolve in tests.
static lv_subject_t s_test_platform_extras_subject;
static bool s_test_notification_subject_initialized = false;

void app_globals_init_subjects() {
    if (!s_test_notification_subject_initialized) {
        lv_subject_init_pointer(&s_test_notification_subject, nullptr);
        lv_subject_init_int(&s_test_home_edit_mode_subject, 0);
        lv_subject_init_int(&s_test_wizard_active_subject, 0);
#if defined(HELIX_PLATFORM_ESP32)
        lv_subject_init_int(&s_test_platform_extras_subject, 0);
#else
        lv_subject_init_int(&s_test_platform_extras_subject, 1);
#endif
        lv_xml_register_subject(nullptr, "platform_extras_available",
                                &s_test_platform_extras_subject);
        s_test_notification_subject_initialized = true;
        // Mirrors the real seeding in app_globals.cpp — the rule itself lives
        // in helix::platform_host_power_supported() (real code, linked here).
        lv_subject_init_int(&s_test_host_power_supported_subject,
                            helix::platform_host_power_supported() ? 1 : 0);
        if (!lv_xml_get_subject(nullptr, "platform_host_power_supported")) {
            lv_xml_register_subject(nullptr, "platform_host_power_supported",
                                    &s_test_host_power_supported_subject);
        }
        spdlog::debug("[Test Stub] app_globals_init_subjects: subjects initialized");
    }
}

void app_globals_deinit_subjects() {
    if (s_test_notification_subject_initialized) {
        lv_subject_deinit(&s_test_notification_subject);
        lv_subject_deinit(&s_test_home_edit_mode_subject);
        lv_subject_deinit(&s_test_wizard_active_subject);
        lv_subject_deinit(&s_test_host_power_supported_subject);
        lv_subject_deinit(&s_test_platform_extras_subject);
        s_test_notification_subject_initialized = false;
        spdlog::debug("[Test Stub] app_globals_deinit_subjects: subjects deinitialized");
    }
}

lv_subject_t& get_notification_subject() {
    if (!s_test_notification_subject_initialized) {
        app_globals_init_subjects();
    }
    return s_test_notification_subject;
}

lv_subject_t& get_home_edit_mode_subject() {
    if (!s_test_notification_subject_initialized) {
        app_globals_init_subjects();
    }
    return s_test_home_edit_mode_subject;
}

lv_subject_t& get_wizard_active_subject() {
    if (!s_test_notification_subject_initialized) {
        app_globals_init_subjects();
    }
    return s_test_wizard_active_subject;
}

// Stub for ui_notification_init_subjects (creates test subjects for notification badge)
static lv_subject_t s_test_notification_count_subject;
static bool s_test_notification_subjects_initialized = false;

void helix::ui::notification_init_subjects() {
    if (!s_test_notification_subjects_initialized) {
        lv_subject_init_int(&s_test_notification_count_subject, 0);
        s_test_notification_subjects_initialized = true;
        spdlog::debug("[Test Stub] ui_notification_init_subjects: subjects initialized");
    }
}

void helix::ui::notification_deinit_subjects() {
    if (s_test_notification_subjects_initialized) {
        lv_subject_deinit(&s_test_notification_count_subject);
        s_test_notification_subjects_initialized = false;
        spdlog::debug("[Test Stub] ui_notification_deinit_subjects: subjects deinitialized");
    }
}

void helix::ui::notification_register_callbacks() {
    spdlog::debug("[Test Stub] ui_notification_register_callbacks: no-op in tests");
}

void helix::ui::notification_manager_init() {
    spdlog::debug("[Test Stub] ui_notification_manager_init: no-op in tests");
}
