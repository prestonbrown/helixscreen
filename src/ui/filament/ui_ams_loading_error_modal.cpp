// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_loading_error_modal.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsLoadingErrorModal::AmsLoadingErrorModal() {
    spdlog::debug("[AmsLoadingErrorModal] Constructed");
}

AmsLoadingErrorModal::~AmsLoadingErrorModal() {
    spdlog::trace("[AmsLoadingErrorModal] Destroyed");
}

// ============================================================================
// Public API
// ============================================================================

bool AmsLoadingErrorModal::show(lv_obj_t* parent, const std::string& error_message,
                                RetryCallback retry_callback) {
    return show(parent, error_message, "Check the filament path and try again.", retry_callback);
}

bool AmsLoadingErrorModal::show(lv_obj_t* parent, const std::string& error_message,
                                const std::string& hint_message, RetryCallback retry_callback) {
    // Store state
    error_message_ = error_message;
    hint_message_ = hint_message;
    retry_callback_ = std::move(retry_callback);

    // Show the modal via Modal base class (buttons wired in on_show())
    if (!Modal::show(parent)) {
        return false;
    }

    spdlog::info("[AmsLoadingErrorModal] Shown with message: {}", error_message_);
    return true;
}

void AmsLoadingErrorModal::dismiss_silently() {
    if (!is_visible()) {
        return;
    }

    spdlog::debug("[AmsLoadingErrorModal] Programmatic dismiss - suppressing dismiss callback");
    silent_dismiss_ = true;
    hide();
    // hide() can return before reaching on_hide() (e.g. an exit animation is
    // already running), so clear here too: a flag left armed would swallow the
    // next user dismissal and its fault-clearing gcode.
    silent_dismiss_ = false;
}

// ============================================================================
// Modal Hooks
// ============================================================================

void AmsLoadingErrorModal::on_show() {
    // Wire buttons via Modal base class — uses per-callback user_data,
    // safe from the user_data/name walk-up issues (prestonbrown/helixscreen#735)
    wire_ok_button("btn_primary");       // Retry
    wire_cancel_button("btn_secondary"); // Close (bottom)
    wire_cancel_button("btn_close");     // X button (top-right)

    // Update error message label
    lv_obj_t* message_label = find_widget("error_message");
    if (message_label) {
        lv_label_set_text(message_label, error_message_.c_str());
    }

    // Update hint message label
    lv_obj_t* hint_label = find_widget("error_hint");
    if (hint_label) {
        lv_label_set_text(hint_label, hint_message_.c_str());
    }
}

void AmsLoadingErrorModal::on_hide() {
    // Fire dismiss callback for all close paths EXCEPT Retry.
    // This catches: Close button, X button, Cancel, backdrop click, ESC key.
    // The dismiss callback clears AFC error state (RESET_FAILURE + AFC_CLEAR_MESSAGE)
    // so the error dialog doesn't reappear immediately.
    // dismiss_silently() suppresses the same way Retry does: the error resolved
    // on its own, so there is no backend fault left to clear.
    if (!retry_in_progress_ && !silent_dismiss_ && dismiss_callback_) {
        spdlog::debug("[AmsLoadingErrorModal] Firing dismiss callback");
        dismiss_callback_();
    }
    retry_in_progress_ = false;
    silent_dismiss_ = false;
    spdlog::debug("[AmsLoadingErrorModal] on_hide()");
}

// ============================================================================
// Virtual Button Handlers
// ============================================================================

void AmsLoadingErrorModal::on_ok() {
    spdlog::info("[AmsLoadingErrorModal] Retry requested");

    // Mark retry so on_hide() doesn't fire the dismiss callback
    retry_in_progress_ = true;

    if (retry_callback_) {
        retry_callback_();
    }

    hide();
}

void AmsLoadingErrorModal::on_cancel() {
    spdlog::debug("[AmsLoadingErrorModal] Close requested");
    hide();
}

} // namespace helix::ui
