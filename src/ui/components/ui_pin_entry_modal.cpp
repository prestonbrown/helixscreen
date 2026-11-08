// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_pin_entry_modal.cpp
 * @brief PIN entry modal implementation — numeric keypad for PIN set/change/remove flows.
 */

#include "ui_pin_entry_modal.h"

#include "ui_callback_helpers.h"
#include "ui_effects.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_next_tick.h"
#include "ui_open_instances.h"
#include "ui_pin_utils.h"
#include "ui_utils.h"

#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <vector>

namespace helix::ui {

namespace {

/// The open pad, at most one. Never freed: a pad deleted on the static-destruction
/// path still removes itself here.
OpenInstances<PinEntryModal>& open_pads() {
    static auto* pads = new OpenInstances<PinEntryModal>();
    return *pads;
}

} // namespace

PinEntryModal* PinEntryModal::from_event(lv_event_t* e) {
    return e ? open_pads().owner_of(lv_event_get_current_target_obj(e)) : nullptr;
}

// ============================================================================
// Public static API
// ============================================================================

void PinEntryModal::show_pin_entry(const std::string& heading, PinCallback on_complete) {
    spdlog::debug("[PinEntryModal] show_pin_entry: heading='{}'", heading);

    // Close any open pad first. Its callback is not called: the pad replacing it
    // usually comes from that very callback.
    for (PinEntryModal* pad : std::vector<PinEntryModal*>(open_pads().all())) {
        spdlog::warn("[PinEntryModal] Replacing active modal without callback");
        pad->destroy_async();
    }

    auto* pad = new PinEntryModal(heading, std::move(on_complete));
    if (!pad->create()) {
        delete pad;
    }
}

void PinEntryModal::dismiss() {
    spdlog::debug("[PinEntryModal] dismiss()");
    for (PinEntryModal* pad : std::vector<PinEntryModal*>(open_pads().all())) {
        pad->on_cancel();
    }
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

PinEntryModal::PinEntryModal(const std::string& heading, PinCallback on_complete)
    : heading_(heading), on_complete_(std::move(on_complete)) {
    spdlog::trace("[PinEntryModal] Created for '{}'", heading_);
}

PinEntryModal::~PinEntryModal() {
    open_pads().remove(this);
    spdlog::trace("[PinEntryModal] Destroyed");
}

// ============================================================================
// UI Creation / Destruction
// ============================================================================

bool PinEntryModal::create() {
    lv_obj_t* screen = lv_screen_active();
    if (!screen) {
        spdlog::error("[PinEntryModal] No active screen, cannot create modal");
        return false;
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
    cover_chrome();
    helix::ui::bring_to_front(backdrop_);

    // Create the dialog from XML component
    dialog_ = static_cast<lv_obj_t*>(lv_xml_create(backdrop_, "pin_entry_modal", nullptr));
    if (!dialog_) {
        spdlog::error("[PinEntryModal] Failed to create pin_entry_modal from XML");
        uncover_chrome();
        lv_obj_delete(backdrop_);
        backdrop_ = nullptr;
        return false;
    }

    // Set heading text
    lv_obj_t* heading_lbl = helix::ui::find_required(dialog_, "pin_heading", "PinEntryModal");
    if (heading_lbl) {
        lv_label_set_text(heading_lbl, heading_.c_str());
    }

    // Reset state
    clear_digits();

    open_pads().add(this);
    spdlog::debug("[PinEntryModal] Created for '{}'", heading_);
    return true;
}

// The PIN pad is modal: while it is open the navbar, E-stop included, stays
// under its backdrop. Each instance holds at most one cover.
void PinEntryModal::cover_chrome() {
    if (!covering_chrome_) {
        covering_chrome_ = true;
        ModalStack::instance().add_external_cover();
    }
}

void PinEntryModal::uncover_chrome() {
    if (covering_chrome_) {
        covering_chrome_ = false;
        ModalStack::instance().remove_external_cover();
    }
}

void PinEntryModal::destroy_async() {
    if (closing_) {
        return;
    }
    closing_ = true;
    open_pads().remove(this);
    lifetime_.invalidate();
    // Use lv_obj_delete_async for LVGL objects — auto-cancelled if parent is
    // deleted first, preventing use-after-free in lv_event_mark_deleted (#543).
    uncover_chrome();
    if (backdrop_) {
        helix::ui::safe_delete_deferred(backdrop_);
        dialog_ = nullptr;
    }
    digit_buffer_.clear();
    // Defer C++ cleanup (destructor is trivial — just logging)
    helix::ui::run_next_tick([self = this]() { delete self; });
    spdlog::debug("[PinEntryModal] Async destruction scheduled");
}

// ============================================================================
// Digit input handling
// ============================================================================

void PinEntryModal::on_digit(int digit) {
    if (!dialog_) {
        return;
    }
    if (static_cast<int>(digit_buffer_.size()) >= kMaxDigits) {
        spdlog::debug("[PinEntryModal] Max digits reached, ignoring");
        return;
    }

    digit_buffer_ += static_cast<char>('0' + digit);
    spdlog::debug("[PinEntryModal] Digit entered, buffer length={}", digit_buffer_.size());
    update_dots();
    hide_error();

    // Auto-submit at max digits, deferred so on_digit() returns before on_confirm()
    // can delete this modal. A pad closed in the meantime drops the submit.
    if (static_cast<int>(digit_buffer_.size()) == kMaxDigits) {
        lifetime_.defer("PinEntryModal::auto_submit", [this]() { on_confirm(); });
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
    if (static_cast<int>(digit_buffer_.size()) < kMinDigits) {
        spdlog::debug("[PinEntryModal] PIN too short ({} of {} required)", digit_buffer_.size(),
                      kMinDigits);
        clear_digits();
        show_error("PIN must be at least 4 digits");
        return;
    }

    std::string pin = digit_buffer_;
    spdlog::debug("[PinEntryModal] Confirm with {} digit PIN", pin.size());

    if (on_complete_) {
        // Callback may call show_pin_entry(), which closes this pad and opens a new one.
        auto callback = std::move(on_complete_);
        on_complete_ = nullptr;
        std::string error = callback(pin);
        if (!error.empty()) {
            // Callback rejected the PIN — show error and clear for retry.
            // But only if the callback did not replace this pad.
            if (!closing_) {
                spdlog::debug("[PinEntryModal] PIN rejected: {}", error);
                on_complete_ = std::move(callback);
                clear_digits();
                show_error(error.c_str());
            }
            return;
        }
    }

    // Accepted: defer destruction to avoid deleting objects in input event handler.
    // A no-op when the callback already replaced this pad.
    destroy_async();
}

void PinEntryModal::on_cancel() {
    spdlog::debug("[PinEntryModal] Cancel");

    auto callback = std::move(on_complete_);
    on_complete_ = nullptr;
    // Close first, so a pad the callback opens is not replacing this one.
    destroy_async();

    if (callback) {
        callback(""); // Return value ignored on cancel
    }
}

// ============================================================================
// Dot indicator updates
// ============================================================================

void PinEntryModal::update_dots() {
    update_pin_dots(dialog_, "pin_dot_", kMaxDigits, static_cast<int>(digit_buffer_.size()));
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
    PinEntryModal* pad = from_event(e);
    if (!pad) {
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
        pad->on_digit(digit);
    } else {
        spdlog::warn("[PinEntryModal] Could not parse digit from button name '{}'", name);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PinEntryModal::on_backspace_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("pin_modal_backspace_clicked");
    if (PinEntryModal* pad = from_event(e)) {
        pad->on_backspace();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PinEntryModal::on_confirm_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("pin_modal_confirm_clicked");
    if (PinEntryModal* pad = from_event(e)) {
        pad->on_confirm();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PinEntryModal::on_cancel_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("pin_modal_cancel_clicked");
    if (PinEntryModal* pad = from_event(e)) {
        pad->on_cancel();
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
