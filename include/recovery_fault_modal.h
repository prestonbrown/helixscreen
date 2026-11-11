// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_fault_path.h"
#include "ui_modal.h"

#include "error_event.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief The dialog for a filament system's fault and its recovery actions
 *
 * The standard dialog card with the fault's recovery actions as the
 * modal_button_row: the first action takes the primary slot at the right, the
 * next ones the secondary, tertiary and quaternary slots leftward. Each button
 * takes its action's style ("primary", "danger", neutral). Embeds the
 * fault-path diagram, which shows when the fault text names a stop point.
 *
 * Klipper's own action:prompt macros render through ActionPromptModal instead.
 */
class RecoveryFaultModal : public Modal {
  public:
    /// Receives the action the user tapped: one with a gcode or a guided flow.
    using ActionCallback = std::function<void(const helix::RecoveryAction& action)>;
    /// Fires for every close this class's owner did not make with hide(): a
    /// button (including a dismiss-only one), backdrop tap, ESC or a sweep.
    /// Runs inside Modal::hide(), so it must not show a modal.
    using DismissCallback = std::function<void()>;

    /// The button row's four slots.
    static constexpr size_t kMaxActions = 4;

    RecoveryFaultModal() = default;
    ~RecoveryFaultModal() override = default;

    RecoveryFaultModal(const RecoveryFaultModal&) = delete;
    RecoveryFaultModal& operator=(const RecoveryFaultModal&) = delete;

    /**
     * @brief Show the fault, replacing the one on screen if any
     *
     * Actions past kMaxActions are not shown. An action with neither a gcode
     * nor a guided flow closes the dialog and sends nothing; with no actions,
     * an OK button does.
     *
     * @return true if the dialog is on screen
     */
    bool show_fault(lv_obj_t* parent, const std::string& title, const std::string& detail,
                    const std::vector<helix::RecoveryAction>& actions);

    void set_action_callback(ActionCallback callback) {
        action_callback_ = std::move(callback);
    }
    void set_dismiss_callback(DismissCallback callback) {
        dismiss_callback_ = std::move(callback);
    }

    [[nodiscard]] const char* get_name() const override {
        return "Recovery Fault Modal";
    }
    [[nodiscard]] const char* component_name() const override {
        return "recovery_fault_modal";
    }

    /// The modal_button_row variant for a RecoveryAction::style.
    static const char* variant_for_style(const std::string& style);

  protected:
    void on_show() override;
    void on_hide() override;
    void on_ok() override {
        run_action(0);
    }
    void on_cancel() override {
        run_action(1);
    }
    void on_tertiary() override {
        run_action(2);
    }
    void on_quaternary() override {
        run_action(3);
    }
    /// ESC closes without choosing: the secondary slot holds a recovery action,
    /// not a cancel.
    void on_escape() override {
        hide(ModalCloseReason::EscKey);
    }

  private:
    void run_action(size_t index);

    std::vector<helix::RecoveryAction> actions_;
    ActionCallback action_callback_;
    DismissCallback dismiss_callback_;
    FaultPathHold fault_path_;
};

} // namespace helix::ui
