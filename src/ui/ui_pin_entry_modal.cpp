// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_pin_entry_modal.cpp
 * @brief PIN entry modal implementation — numeric keypad for PIN set/change/remove flows.
 */

#include "ui_pin_entry_modal.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_pin_utils.h"
#include "ui_utils.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

namespace helix::ui {

// ============================================================================
// Static state
// ============================================================================

PinEntryModal* PinEntryModal::g_active_modal = nullptr;

// ============================================================================
// Public static API
// ============================================================================

void PinEntryModal::show_pin_entry(const std::string& heading, PinCallback on_complete) {
    spdlog::debug("[PinEntryModal] show_pin_entry: heading='{}'", heading);

    // Dismiss any active modal first
    if (g_active_modal) {
        spdlog::warn("[PinEntryModal] Replacing active modal without callback");
        g_active_modal->destroy_async();
        g_active_modal = nullptr;
    }

    g_active_modal = new PinEntryModal(heading, std::move(on_complete));
    g_active_modal->create();
}

void PinEntryModal::dismiss() {
    if (!g_active_modal) {
        return;
    }
    spdlog::debug("[PinEntryModal] dismiss()");
    auto* modal = g_active_modal;
    g_active_modal = nullptr;
    if (modal->on_complete_) {
        modal->on_complete_(""); // Return value ignored on dismiss
    }
    modal->destroy_async();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

PinEntryModal::PinEntryModal(const std::string& heading, PinCallback on_complete)
    : heading_(heading), on_complete_(std::move(on_complete)) {
    spdlog::trace("[PinEntryModal] Created for '{}'", heading_);
}

PinEntryModal::~PinEntryModal() {
    spdlog::trace("[PinEntryModal] Destroyed");
}

// ============================================================================
// UI Creation / Destruction
// ============================================================================

void PinEntryModal::create() {
    lv_obj_t* screen = lv_screen_active();
    if (!screen) {
        spdlog::error("[PinEntryModal] No active screen, cannot create modal");
        return;
    }

    // Semi-transparent backdrop (blocks touches to underlying UI)
    backdrop_ = lv_obj_create(screen);
    lv_obj_set_size(backdrop_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(backdrop_, theme_manager_get_color("screen_bg"), 0);
    lv_obj_set_style_bg_opa(backdrop_, 100, 0);
    lv_obj_set_style_border_width(backdrop_, 0, 0);
    lv_obj_set_style_pad_all(backdrop_, 0, 0);
    lv_obj_set_style_radius(backdrop_, 0, 0);
    lv_obj_add_flag(backdrop_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(backdrop_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(backdrop_);

    // Create the dialog from XML component
    dialog_ = static_cast<lv_obj_t*>(lv_xml_create(backdrop_, "pin_entry_modal", nullptr));
    if (!dialog_) {
        spdlog::error("[PinEntryModal] Failed to create pin_entry_modal from XML");
        lv_obj_delete(backdrop_);
        backdrop_ = nullptr;
        return;
    }

    // Set heading text
    lv_obj_t* heading_lbl = lv_obj_find_by_name(dialog_, "pin_heading");
    if (heading_lbl) {
        lv_label_set_text(heading_lbl, heading_.c_str());
    }

    // Reset state
    clear_digits();

    spdlog::debug("[PinEntryModal] Created for '{}'", heading_);
}

void PinEntryModal::destroy() {
    if (backdrop_) {
        lv_obj_delete(backdrop_);
        backdrop_ = nullptr;
        dialog_ = nullptr;
    }
    digit_buffer_.clear();
    spdlog::debug("[PinEntryModal] Destroyed");
}

void PinEntryModal::destroy_async() {
    // Use lv_obj_delete_async for LVGL objects — auto-cancelled if parent is
    // deleted first, preventing use-after-free in lv_event_mark_deleted (#543).
    if (backdrop_) {
        helix::ui::safe_delete_deferred(backdrop_);
        dialog_ = nullptr;
    }
    digit_buffer_.clear();
    // Defer C++ cleanup (destructor is trivial — just logging)
    lv_async_call([](void* ud) { delete static_cast<PinEntryModal*>(ud); }, this);
    spdlog::debug("[PinEntryModal] Async destruction scheduled");
}

// ============================================================================
// Digit input handling
// ============================================================================

void PinEntryModal::on_digit(int digit) {
    if (!dialog_) {
        return;
    }
    if (static_cast<int>(digit_buffer_.size()) >= MAX_DIGITS) {
        spdlog::debug("[PinEntryModal] Max digits reached, ignoring");
        return;
    }

    digit_buffer_ += static_cast<char>('0' + digit);
    spdlog::debug("[PinEntryModal] Digit entered, buffer length={}", digit_buffer_.size());
    update_dots();
    hide_error();

    // Auto-submit at max digits — defer via lv_async_call so on_digit() returns
    // before on_confirm() can delete this modal (avoids UB from delete-this-in-member)
    if (static_cast<int>(digit_buffer_.size()) == MAX_DIGITS) {
        lv_async_call(
            [](void*) {
                if (g_active_modal) {
                    g_active_modal->on_confirm();
                }
            },
            nullptr);
    }
}

void PinEntryModal::on_backspace() {
    if (!dialog_ || digit_buffer_.empty()) {
        return;
    }
    digit_buffer_.pop_back();
    spdlog::debug("[PinEntryModal] Backspace, buffer length={}", digit_buffer_.size());
    update_dots();
    hide_error();
}

void PinEntryModal::on_confirm() {
    if (!dialog_) {
        return;
    }
    if (static_cast<int>(digit_buffer_.size()) < MIN_DIGITS) {
        spdlog::debug("[PinEntryModal] PIN too short ({} of {} required)", digit_buffer_.size(),
                      MIN_DIGITS);
        clear_digits();
        show_error("PIN must be at least 4 digits");
        return;
    }

    std::string pin = digit_buffer_;
    spdlog::debug("[PinEntryModal] Confirm with {} digit PIN", pin.size());

    if (on_complete_) {
        // Callback may call show_pin_entry() which destroys this modal and
        // creates a new one. Check g_active_modal after callback returns.
        auto callback = std::move(on_complete_);
        on_complete_ = nullptr;
        std::string error = callback(pin);
        if (!error.empty()) {
            // Callback rejected the PIN — show error and clear for retry.
            // But only if we're still the active modal (callback didn't replace us).
            if (g_active_modal == this) {
                spdlog::debug("[PinEntryModal] PIN rejected: {}", error);
                on_complete_ = std::move(callback);
                clear_digits();
                show_error(error.c_str());
            }
            return;
        }
        // If callback called show_pin_entry(), we've already been destroyed
        if (g_active_modal != this) {
            return; // We were replaced — don't double-delete
        }
    }

    // Accepted — defer destruction to avoid deleting objects in input event handler
    g_active_modal = nullptr;
    destroy_async();
}

void PinEntryModal::on_cancel() {
    spdlog::debug("[PinEntryModal] Cancel");

    auto callback = std::move(on_complete_);
    on_complete_ = nullptr;
    g_active_modal = nullptr;

    if (callback) {
        callback(""); // Return value ignored on cancel
    }

    destroy_async();
}

// ============================================================================
// Dot indicator updates
// ============================================================================

void PinEntryModal::update_dots() {
    update_pin_dots(dialog_, "pin_dot_", MAX_DIGITS, static_cast<int>(digit_buffer_.size()));
}

void PinEntryModal::clear_digits() {
    digit_buffer_.clear();
    update_dots();
    hide_error();
}

// ============================================================================
// Error label
// ============================================================================

void PinEntryModal::show_error(const char* text) {
    show_pin_error(dialog_, "pin_error_label", text);
}

void PinEntryModal::hide_error() {
    hide_pin_error(dialog_, "pin_error_label");
}

// ============================================================================
// Static XML event callbacks
// ============================================================================

void PinEntryModal::on_digit_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("pin_modal_digit_clicked");
    if (!g_active_modal) {
        return;
    }

    auto* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!btn) {
        return;
    }

    // Button names are "pin_digit_0" through "pin_digit_9"
    const char* name = lv_obj_get_name(btn);
    if (!name) {
        return;
    }

    // Parse digit from trailing character: "pin_digit_5" → 5
    size_t len = strlen(name);
    if (len >= 1 && name[len - 1] >= '0' && name[len - 1] <= '9') {
        int digit = name[len - 1] - '0';
        g_active_modal->on_digit(digit);
    } else {
        spdlog::warn("[PinEntryModal] Could not parse digit from button name '{}'", name);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PinEntryModal::on_backspace_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("pin_modal_backspace_clicked");
    if (g_active_modal) {
        g_active_modal->on_backspace();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PinEntryModal::on_confirm_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("pin_modal_confirm_clicked");
    if (g_active_modal) {
        g_active_modal->on_confirm();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PinEntryModal::on_cancel_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("pin_modal_cancel_clicked");
    if (g_active_modal) {
        g_active_modal->on_cancel();
    }
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// Callback registration
// ============================================================================

void PinEntryModal::register_callbacks() {
    register_xml_callbacks({
        {"pin_modal_digit_clicked", PinEntryModal::on_digit_clicked},
        {"pin_modal_backspace_clicked", PinEntryModal::on_backspace_clicked},
        {"pin_modal_confirm_clicked", PinEntryModal::on_confirm_clicked},
        {"pin_modal_cancel_clicked", PinEntryModal::on_cancel_clicked},
    });
    spdlog::debug("[PinEntryModal] Callbacks registered");
}

} // namespace helix::ui
