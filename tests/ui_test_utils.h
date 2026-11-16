// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_notification.h"
#include "ui_toast_manager.h"

#include "lvgl/lvgl.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

/**
 * @brief UI Test Utilities - Simulate user interactions and wait for UI updates
 *
 * Provides programmatic testing of LVGL UI components:
 * - Click/touch simulation
 * - Keyboard input simulation
 * - Async wait helpers (timers, animations, conditions)
 * - Widget state verification
 *
 * Usage:
 *   ui_test_init(screen);
 *   ui_test_click(button);
 *   ui_test_type_text(textarea, "password");
 *   ui_test_wait_ms(500);
 *   ui_test_cleanup();
 */

/**
 * @brief Safely initialize LVGL (idempotent - no warning if already initialized)
 *
 * Use this instead of calling lv_init() directly in tests to avoid
 * "lv_init: already initialized" warnings when tests run sequentially.
 */
void lv_init_safe();

/**
 * @brief Let a flush wait return on every display registered so far.
 *
 * A test display renders into a buffer nobody reads, so a flush is finished the
 * moment it is issued. LVGL cannot know that: `wait_for_flushing()` busy-waits
 * on `disp->flushing` whenever a display has no flush_wait_cb, and a display
 * with no flush callback never clears that flag, so the next refresh of it
 * spins at 100% CPU with no way out. A no-op flush_wait_cb takes the branch
 * that clears the flag instead.
 *
 * Idempotent and cheap. Called from HelixTestFixture::reset_all(), so it covers
 * every display a fixture, a TEST_CASE or a static initialiser has created by
 * then.
 */
void ensure_displays_never_block_on_flush();

/**
 * @brief WCAG contrast math shared by tests that assert readability
 *
 * Deliberately independent of theme_manager's implementation so a wrong
 * formula in the source cannot make the tests agree with it.
 */
namespace wcag {

/// WCAG relative luminance of one 8-bit sRGB channel.
inline double channel_luminance(uint8_t v) {
    const double c = v / 255.0;
    return (c <= 0.03928) ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

/// WCAG relative luminance of a color.
inline double luminance(lv_color_t c) {
    return 0.2126 * channel_luminance(c.red) + 0.7152 * channel_luminance(c.green) +
           0.0722 * channel_luminance(c.blue);
}

/// WCAG contrast ratio between two colors (1.0 = identical).
inline double contrast(lv_color_t a, lv_color_t b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

/// True when blending @p text toward the pole on its own side of @p fill
/// (white for text lighter than the fill, black for darker) can itself reach
/// a 4:1 ratio. White reaches 4:1 below fill luminance 0.2125, black above
/// 0.15; outside those bounds no tint on the text's side is readable and a
/// side-preservation assertion would be demanding the impossible.
inline bool own_pole_reaches_4_1(lv_color_t text, lv_color_t fill) {
    const double lf = luminance(fill);
    return (luminance(text) > lf) ? (lf <= 0.2125) : (lf >= 0.15);
}

} // namespace wcag

// Install a real TemperatureHistoryManager for get_temperature_history_manager()
// to return (tests default to nullptr). Lets a test exercise history backfill
// paths; pass nullptr to restore the default. See #1124.
class TemperatureHistoryManager;
void set_test_temperature_history_manager(TemperatureHistoryManager* mgr);

namespace helix {
namespace ui {

/*
 * Observation hooks over the real notification and toast objects.
 *
 * ui_notification.o and ui_toast_manager.o are linked into the test binary.
 * Each reports every call to one observer (helix::ui::set_notification_observer(),
 * ToastManager::set_observer()) on the calling thread, before threading, the
 * pre-init startup queue or the user's toast threshold decide what is shown;
 * ui_test_utils.cpp installs both and routes each event to the hook below for
 * its kind. A hook is empty until a test sets it; pass nullptr to clear.
 */

/**
 * @brief Hook for timed warnings: ui_notification_warning() (message only,
 *        never the title) and ui_notification_warning_with_detail() (the
 *        history row, "message - detail").
 */
void set_test_notification_warning_hook(std::function<void(const std::string&)> hook);

/**
 * @brief Hook for ui_notification_warning_sticky(), which stays on screen until
 *        dismissed. Separate from the warning hook so a test can tell the two
 *        channels apart, not just read the text. Receives "message - detail".
 */
void set_test_notification_sticky_warning_hook(std::function<void(const std::string&)> hook);

/**
 * @brief Hook for ui_notification_error(), ui_notification_printer_fault() and
 *        ui_notification_error_with_detail(), modal or not.
 */
void set_test_notification_error_hook(std::function<void(const std::string&)> hook);

/**
 * @brief Hook for ui_notification_info(). INFO notices GUIDE rather than alarm
 *        ("filament is clear, pull it out"), so this is how a test asserts the
 *        user was told what to do next.
 */
void set_test_notification_info_hook(std::function<void(const std::string&)> hook);

/**
 * @brief Hook for ui_notification_success(), for flows whose outcome is a
 *        success toast: a test that a failure is never reported as success
 *        needs to see the success path too.
 */
void set_test_notification_success_hook(std::function<void(const std::string&)> hook);

/**
 * @brief Hook for the history-only ui_notification_*_with_action() rows.
 *
 * Receives the row's severity, message and action id, so a test can prove an
 * entry was added and that tapping it would dispatch the right action.
 */
void set_test_notification_action_hook(
    std::function<void(ToastSeverity, const std::string&, const std::string&)> hook);

/**
 * @brief Hook for every ui_notification_* call, as the whole event: title,
 *        severity, history-row text, action and stickiness. Runs before the
 *        per-kind hooks above.
 */
void set_test_notification_event_hook(std::function<void(const NotificationEvent&)> hook);

/**
 * @brief Hook for every ToastManager::show*() call, including the ones
 *        ui_notification_* makes once the toast system is initialized.
 *
 * Carries the severity so a wrong-severity toast fails on severity, not just
 * wording, and the duration so a sticky (0) call cannot read as a timed one.
 * A show_with_detail() call arrives as "message - detail".
 */
void set_test_toast_hook(std::function<void(ToastSeverity, const std::string&, uint32_t)> hook);

/**
 * @brief Press the action button of the most recent action toast
 *
 * Calls the last show_with_action() callback with its user_data, as the user
 * tapping the button would. Returns false when no action toast has been shown
 * since the last call or since set_test_toast_hook(), which forgets the last
 * action.
 */
bool fire_last_toast_action();

/**
 * @brief True when the most recent action toast was dismissed through
 *        ToastManager::hide(action_callback, user_data) with its own pair.
 *
 * Cleared, like the last action, by set_test_toast_hook().
 */
bool last_action_toast_dismissed();
bool last_shown_toast_has_closed_cb();

/**
 * @brief Clear every hook above and the recorded action toasts.
 *
 * HelixTestFixture::reset_all() runs it, so a hook capturing a test's stack
 * locals cannot outlive the test case.
 */
void clear_test_hooks();

} // namespace ui
} // namespace helix

namespace UITest {

/**
 * @brief Initialize UI test system with virtual input device
 * @param screen LVGL screen to attach input device to
 */
void init(lv_obj_t* screen);

/**
 * @brief Cleanup UI test system and remove virtual input device
 */
void cleanup();

/**
 * @brief Simulate click/touch on widget at its center
 * @param widget Widget to click
 * @return true if click was simulated successfully
 */
bool click(lv_obj_t* widget);

/**
 * @brief Simulate click/touch at specific coordinates
 * @param x X coordinate (absolute)
 * @param y Y coordinate (absolute)
 * @return true if click was simulated successfully
 */
bool click_at(int32_t x, int32_t y);

/**
 * @brief Press and HOLD at specific coordinates (no release)
 *
 * Split out of click_at() so a test can do work while the press is still down -
 * e.g. tear a dialog down mid-press and then release, which is how LVGL delivers
 * LV_EVENT_CLICKED to an object captured before the teardown. Pair with release().
 */
bool press_at(int32_t x, int32_t y);

/**
 * @brief Move the pointer while a press started by press_at() is held
 *
 * One read per call, so a single large jump is one sample: past the indev's gesture
 * distance it is a swipe. Pair with press_at() and release().
 */
bool move_to(int32_t x, int32_t y);

/**
 * @brief Release the current press started by press_at()
 */
bool release();

/**
 * @brief Type text into focused textarea character by character
 * @param text Text to type
 * @return true if text was sent successfully
 *
 * Note: Textarea must have focus before calling this function
 */
bool type_text(const std::string& text);

/**
 * @brief Type text into specific textarea (gives it focus first)
 * @param textarea Textarea widget to type into
 * @param text Text to type
 * @return true if text was sent successfully
 */
bool type_text(lv_obj_t* textarea, const std::string& text);

/**
 * @brief Send key press event (for special keys like Enter, Backspace)
 * @param key LV_KEY_* constant
 * @return true if key event was sent successfully
 */
bool send_key(uint32_t key);

/**
 * @brief Advance LVGL's VIRTUAL clock by @p ms in 5ms steps, pumping timers
 *
 * Never sleeps: no real time passes beyond the CPU the timers take, so another
 * thread gets no time slice from it. The input helpers above use it, as does
 * LVGLTestFixture::process_lvgl().
 */
void advance_ms(uint32_t ms);

/**
 * @brief Advance LVGL's VIRTUAL clock in @p step_ms steps until @p condition holds
 * @return true if it held within @p budget_ms of virtual time
 *
 * Each step advances the tick and pumps timers, then evaluates the condition, so
 * it runs at least once. Never sleeps, like advance_ms(): for a wait on another
 * thread use wait_until().
 */
bool advance_until(const std::function<bool()>& condition, uint32_t budget_ms,
                   uint32_t step_ms = 5);

/**
 * @brief Wait @p ms of REAL time, pumping LVGL and advancing its clock in step
 *
 * Sleeps in 5ms slices, advancing the virtual tick by 5ms per slice and running
 * lv_timer_handler_safe(). Real and virtual time therefore track each other,
 * unlike advance_ms().
 */
void wait_ms(uint32_t ms);

/**
 * @brief Wait until condition becomes true or timeout expires
 * @param condition Function returning true when wait should end
 * @param timeout_ms Maximum time to wait in milliseconds, on the real clock
 * @return true if condition became true, false if timeout
 *
 * Checks the condition every 5ms, advancing the virtual tick and pumping LVGL
 * between checks. Prefer LVGLTestFixture::wait_until() when you are already in
 * a fixture — same semantics, and it evaluates the condition at least once.
 */
bool wait_until(std::function<bool()> condition, uint32_t timeout_ms = 5000);

/**
 * @brief Wait for widget to become visible
 * @param widget Widget to wait for
 * @param timeout_ms Maximum time to wait
 * @return true if widget became visible, false if timeout
 */
bool wait_for_visible(lv_obj_t* widget, uint32_t timeout_ms = 5000);

/**
 * @brief Wait for widget to become hidden
 * @param widget Widget to wait for
 * @param timeout_ms Maximum time to wait
 * @return true if widget became hidden, false if timeout
 */
bool wait_for_hidden(lv_obj_t* widget, uint32_t timeout_ms = 5000);

/**
 * @brief Wait for all pending timers to complete
 * @param timeout_ms Maximum time to wait
 * @return true if all timers completed, false if timeout
 *
 * Useful for waiting for async operations (scans, connections, etc.)
 */
bool wait_for_timers(uint32_t timeout_ms = 10000);

/**
 * @brief Check if widget is visible (not hidden)
 * @param widget Widget to check
 * @return true if widget is visible
 */
bool is_visible(lv_obj_t* widget);

/**
 * @brief Get text content from label or textarea
 * @param widget Label or textarea widget
 * @return Text content or empty string if not found
 */
std::string get_text(lv_obj_t* widget);

/**
 * @brief Last lv_label child of a button: ui_button's text label
 *
 * The icon glyph is an lv_label too and precedes the text once stacked
 * (stacked layout, icon_position="left"), so "first label" would hand back
 * the icon instead of the text.
 */
inline lv_obj_t* button_label(lv_obj_t* btn) {
    lv_obj_t* found = nullptr;
    uint32_t count = lv_obj_get_child_count(btn);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(btn, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            found = child;
        }
    }
    return found;
}

/// Caption of the named button inside `root`, empty if absent.
inline std::string button_text(lv_obj_t* root, const char* btn_name) {
    lv_obj_t* btn = lv_obj_find_by_name(root, btn_name);
    lv_obj_t* label = btn ? button_label(btn) : nullptr;
    return label ? lv_label_get_text(label) : std::string();
}

/**
 * @brief Check if widget is in checked/selected state
 * @param widget Checkbox, switch, or button widget
 * @return true if widget is checked/selected
 */
bool is_checked(lv_obj_t* widget);

/**
 * @brief Find widget by name within parent (recursive search)
 * @param parent Parent widget to search within
 * @param name Widget name to find
 * @return Widget if found, nullptr otherwise
 */
lv_obj_t* find_by_name(lv_obj_t* parent, const std::string& name);

/**
 * @brief Count children with specific user_data marker
 * @param parent Parent widget
 * @param marker User data marker string to match
 * @return Number of matching children
 *
 * Useful for counting dynamically created items (e.g., network list items)
 */
int count_children_with_marker(lv_obj_t* parent, const char* marker);

} // namespace UITest

/**
 * @brief Safe wrapper around lv_timer_handler() for tests
 *
 * Drains the UpdateQueue, then processes LVGL timers without triggering the
 * infinite do-while loop in lv_timer_handler().
 *
 * Strategy:
 *   1. Drain UpdateQueue (executes pending callbacks, subject observers fire)
 *   2. Pause ALL timers
 *   3. Manually fire ready one-shot timers (lv_async_call, retry timers)
 *   4. Re-pause ALL timers (one-shot callbacks may have unpaused timers,
 *      e.g. lv_obj_delete → lv_anim_delete → anim_mark_list_change resumes
 *      the animation timer)
 *   5. Call lv_timer_handler() with everything paused (updates internal state)
 *   6. Resume all timers
 *
 * Without step 4, unpaused timers with stale last_run timestamps enter
 * lv_timer_handler()'s do-while loop and never terminate because every fire
 * creates/deletes timers, restarting iteration from the head indefinitely.
 */
uint32_t lv_timer_handler_safe();
