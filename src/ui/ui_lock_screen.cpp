// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_lock_screen.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_pin_utils.h"
#include "ui_utils.h"

#include "lock_manager.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

namespace helix::ui {

// ============================================================================
// Static callback forward declarations
// ============================================================================

static void lock_digit_clicked(lv_event_t* e);
static void lock_backspace_clicked(lv_event_t* e);
static void lock_confirm_clicked(lv_event_t* e);

// ============================================================================
// Singleton
// ============================================================================

LockScreenOverlay& LockScreenOverlay::instance() {
    static LockScreenOverlay inst;
    return inst;
}

// ============================================================================
// Public API
// ============================================================================

bool LockScreenOverlay::is_visible() const {
    return overlay_ != nullptr;
}

void LockScreenOverlay::show() {
    if (is_visible()) {
        spdlog::debug("[LockScreen] Already visible, ignoring show()");
        return;
    }

    spdlog::info("[LockScreen] Showing lock screen");
    create_overlay();
}

void LockScreenOverlay::hide() {
    if (!is_visible()) {
        return;
    }

    spdlog::info("[LockScreen] Hiding lock screen");
    destroy_overlay();
}

// ============================================================================
// Overlay lifecycle
// ============================================================================

void LockScreenOverlay::create_overlay() {
    // Full-screen clickable overlay on lv_layer_top() — absorbs all touch
    overlay_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay_, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(overlay_, 0, 0);
    lv_obj_set_style_pad_all(overlay_, 0, 0);
    lv_obj_set_style_radius(overlay_, 0, 0);

    // Bring above all other content on lv_layer_top()
    lv_obj_move_foreground(overlay_);

    // Instantiate the lock_screen XML component (defines background, card, keypad)
    lv_xml_create(overlay_, "lock_screen", nullptr);

    // Reset digit state
    clear_digits();

    spdlog::debug("[LockScreen] Overlay created on lv_layer_top()");
}

void LockScreenOverlay::destroy_overlay() {
    if (overlay_) {
        // Cancel pending shake animations to prevent dangling pointer callbacks
        lv_obj_t* dots = lv_obj_find_by_name(overlay_, "lock_dots_container");
        if (dots) {
            lv_anim_delete(dots, nullptr);
        }
        // Use lv_obj_delete_async — if called from an lv_async_call chain (e.g.,
        // on_confirm → hide → destroy_overlay), synchronous deletion can corrupt
        // LVGL's event linked list in lv_event_mark_deleted (#543).
        lv_obj_delete_async(overlay_);
        overlay_ = nullptr;
        digit_buffer_.clear();
        spdlog::debug("[LockScreen] Overlay destroyed");
    }
}

// ============================================================================
// Digit input handling
// ============================================================================

void LockScreenOverlay::on_digit(int digit) {
    if (!is_visible()) {
        return;
    }

    if (static_cast<int>(digit_buffer_.size()) >= MAX_DIGITS) {
        spdlog::debug("[LockScreen] Max digits ({}) reached, ignoring", MAX_DIGITS);
        return;
    }

    digit_buffer_ += static_cast<char>('0' + digit);
    spdlog::debug("[LockScreen] Digit entered, buffer length={}", digit_buffer_.size());

    update_dots();
    hide_error();

    // Auto-submit when max digits reached — defer to avoid UB if on_confirm()
    // destroys the overlay while we're still in on_digit()
    if (static_cast<int>(digit_buffer_.size()) == MAX_DIGITS) {
        lv_async_call([](void*) { LockScreenOverlay::instance().on_confirm(); }, nullptr);
    }
}

void LockScreenOverlay::on_backspace() {
    if (!is_visible() || digit_buffer_.empty()) {
        return;
    }

    digit_buffer_.pop_back();
    spdlog::debug("[LockScreen] Backspace, buffer length={}", digit_buffer_.size());

    update_dots();
    hide_error();
}

void LockScreenOverlay::on_confirm() {
    if (!is_visible()) {
        return;
    }

    if (static_cast<int>(digit_buffer_.size()) < MIN_DIGITS) {
        spdlog::debug("[LockScreen] PIN too short ({} digits, need {})", digit_buffer_.size(),
                      MIN_DIGITS);
        clear_digits();
        show_error("PIN must be at least 4 digits");
        return;
    }

    spdlog::debug("[LockScreen] Attempting unlock with {} digit PIN", digit_buffer_.size());

    if (helix::LockManager::instance().try_unlock(digit_buffer_)) {
        spdlog::info("[LockScreen] Unlock successful");
        // Defer hide() to avoid deleting objects in input event handler
        lv_async_call([](void*) { LockScreenOverlay::instance().hide(); }, nullptr);
    } else {
        spdlog::info("[LockScreen] Wrong PIN entered");
        show_error("Wrong PIN");
        shake_dots();
        // clear_digits() is called from shake_dots() completed callback
    }
}

// ============================================================================
// Dot indicator updates (imperative — allowed exception for animation state)
// ============================================================================

void LockScreenOverlay::update_dots() {
    update_pin_dots(overlay_, "lock_dot_", MAX_DIGITS, static_cast<int>(digit_buffer_.size()));
}

void LockScreenOverlay::clear_digits() {
    digit_buffer_.clear();
    update_dots();
    hide_error();
    spdlog::debug("[LockScreen] Digits cleared");
}

// ============================================================================
// Error label visibility
// ============================================================================

void LockScreenOverlay::show_error(const char* text) {
    show_pin_error(overlay_, "lock_error_label", text);
}

void LockScreenOverlay::hide_error() {
    hide_pin_error(overlay_, "lock_error_label");
}

// ============================================================================
// Shake animation on wrong PIN
// ============================================================================

void LockScreenOverlay::shake_dots() {
    if (!overlay_) {
        return;
    }

    lv_obj_t* dots = lv_obj_find_by_name(overlay_, "lock_dots_container");
    if (!dots) {
        spdlog::warn("[LockScreen] lock_dots_container not found for shake animation");
        // Still clear digits after a delay
        lv_async_call([](void*) { LockScreenOverlay::instance().clear_digits(); }, nullptr);
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dots);
    lv_anim_set_values(&a, -10, 10);
    lv_anim_set_duration(&a, 100);
    lv_anim_set_repeat_count(&a, 3);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
        lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), v, LV_PART_MAIN);
    });
    lv_anim_set_completed_cb(&a, [](lv_anim_t* anim) {
        // Reset translate after shake
        lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(anim->var), 0, LV_PART_MAIN);
        // Clear digit buffer after animation — deferred so it runs on main thread
        lv_async_call([](void*) { LockScreenOverlay::instance().clear_digits(); }, nullptr);
    });
    lv_anim_start(&a);

    spdlog::debug("[LockScreen] Shake animation started");
}

// ============================================================================
// Static XML event callbacks
// ============================================================================

/**
 * Handles digit button clicks for all 10 digit buttons (0-9).
 * Parses the digit from the button name: "lock_digit_N" → N.
 */
static void lock_digit_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("lock_digit_clicked");

    auto* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!btn) {
        return;
    }

    // Button names are "lock_digit_0" through "lock_digit_9"
    // Parse the digit from the trailing character of the name
    const char* name = lv_obj_get_name(btn);
    if (!name) {
        spdlog::warn("[LockScreen] Digit button has no name");
        return;
    }

    // Name format: "lock_digit_N" — last character is the digit
    size_t len = strlen(name);
    if (len < 1) {
        spdlog::warn("[LockScreen] Digit button name '{}' too short to parse", name);
        return;
    }

    char last = name[len - 1];
    if (last < '0' || last > '9') {
        spdlog::warn("[LockScreen] Cannot parse digit from button name '{}'", name);
        return;
    }

    int digit = last - '0';
    spdlog::debug("[LockScreen] Digit {} clicked (button={})", digit, name);
    LockScreenOverlay::instance().on_digit(digit);

    LVGL_SAFE_EVENT_CB_END();
}

static void lock_backspace_clicked(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("lock_backspace_clicked");
    LockScreenOverlay::instance().on_backspace();
    LVGL_SAFE_EVENT_CB_END();
}

static void lock_confirm_clicked(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("lock_confirm_clicked");
    LockScreenOverlay::instance().on_confirm();
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// Registration helper — called from xml_registration.cpp
// ============================================================================

void register_lock_screen_callbacks() {
    register_xml_callbacks({
        {"lock_digit_clicked", lock_digit_clicked},
        {"lock_backspace_clicked", lock_backspace_clicked},
        {"lock_confirm_clicked", lock_confirm_clicked},
    });
}

} // namespace helix::ui
