// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_modal.h"

#include <functional>
#include <string>

namespace helix::ui {

/**
 * @brief A print-start warning that offers two ways around the problem
 *        beside starting as is and cancelling.
 *
 * Every close resolves exactly once: a button runs its callback, and a
 * backdrop tap, ESC or any other dismissal runs on_cancel.
 */
class PrintGateChoiceModal : public Modal {
  public:
    struct Config {
        std::string title;
        std::string message;
        std::string first_choice;  ///< The recommended way around: the primary button
        std::string second_choice; ///< The other way around
        std::string proceed;       ///< Start as is
        std::function<void()> on_first;
        std::function<void()> on_second;
        std::function<void()> on_proceed;
        std::function<void()> on_cancel;
    };

    explicit PrintGateChoiceModal(Config config) : config_(std::move(config)) {}

    const char* get_name() const override {
        return "Print Gate Choice";
    }
    const char* component_name() const override {
        return "print_gate_choice_modal";
    }

    /// Show on the active screen and hand the instance to ModalStack. The
    /// dialog widget, or nullptr when it could not be built.
    static lv_obj_t* show_owned(Config config);

  protected:
    void on_show() override;
    void on_ok() override;
    void on_cancel() override;
    void on_escape() override;
    void on_tertiary() override;
    void on_quaternary() override;
    void on_dismissed() override;

  private:
    /// Close as a button press, then run @p action (copied first: the close may free this).
    void answer(std::function<void()> action);

    Config config_;
};

} // namespace helix::ui
