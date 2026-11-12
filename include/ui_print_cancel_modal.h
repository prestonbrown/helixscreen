// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include <functional>

/**
 * @file ui_print_cancel_modal.h
 * @brief Confirmation dialog for canceling an active print
 *
 * Uses Modal for RAII lifecycle - dialog auto-hides when object is destroyed.
 * Shows a warning that all progress will be lost.
 *
 * @example
 *   cancel_modal_.set_on_confirm([this]() { execute_cancel_print(); });
 *   cancel_modal_.show(lv_screen_active());
 */

/**
 * @brief Confirmation dialog for canceling an active print
 *
 * Derives from Modal base class for RAII lifecycle management.
 * Provides callback mechanism for handling user confirmation.
 */
class PrintCancelModal : public Modal {
  public:
    using ConfirmCallback = std::function<void()>;

    const char* get_name() const override {
        return "Print Cancel";
    }

    const char* component_name() const override {
        return "print_cancel_confirm_modal";
    }

    /**
     * @brief Set callback to invoke when user confirms cancellation
     * @param cb Callback function (typically executes cancel_print API call)
     */
    void set_on_confirm(ConfirmCallback cb) {
        on_confirm_cb_ = std::move(cb);
    }

  protected:
    /**
     * @brief Called after modal is created and visible
     *
     * Wires up the "Stop" (ok) and "Keep Printing" (cancel) buttons.
     */
    void on_show() override {
        wire_ok_button("btn_primary");       // "Stop" button
        wire_cancel_button("btn_secondary"); // "Keep Printing" button
    }

    /**
     * @brief Called when user clicks "Stop" button
     *
     * Invokes the confirm callback if set, then hides the modal.
     */
    void on_ok() override {
        if (on_confirm_cb_) {
            on_confirm_cb_();
        }
        hide();
    }

  private:
    ConfirmCallback on_confirm_cb_;
};
