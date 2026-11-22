// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_print_gate_choice_modal.h"

#include <lvgl.h>
#include <memory>

namespace helix::ui {

lv_obj_t* PrintGateChoiceModal::show_owned(Config config) {
    auto modal = std::make_unique<PrintGateChoiceModal>(std::move(config));
    const Config& c = modal->config_;
    const char* cancel = lv_tr("Cancel");
    const char* attrs[] = {
        "title",        c.title.c_str(),        "message",       c.message.c_str(),
        "first_choice", c.first_choice.c_str(), "second_choice", c.second_choice.c_str(),
        "proceed",      c.proceed.c_str(),      "cancel",        cancel,
        nullptr,
    };
    if (!modal->show(lv_screen_active(), attrs)) {
        return nullptr; // the unique_ptr frees the never-shown instance
    }
    lv_obj_t* dialog = modal->dialog();
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
    return dialog;
}

void PrintGateChoiceModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");
    wire_tertiary_button("btn_tertiary");
    wire_quaternary_button("btn_quaternary");
}

void PrintGateChoiceModal::answer(std::function<void()> action) {
    hide(ModalCloseReason::ButtonPress);
    if (action) {
        action();
    }
}

void PrintGateChoiceModal::on_ok() {
    answer(config_.on_first);
}

// The secondary button is the second way around, not a cancel.
void PrintGateChoiceModal::on_cancel() {
    answer(config_.on_second);
}

void PrintGateChoiceModal::on_escape() {
    answer(config_.on_cancel);
}

void PrintGateChoiceModal::on_tertiary() {
    answer(config_.on_proceed);
}

void PrintGateChoiceModal::on_quaternary() {
    answer(config_.on_cancel);
}

void PrintGateChoiceModal::on_dismissed() {
    // The owner closing its own dialog has decided already; every other close
    // (backdrop, ESC, a rebuild) is the user walking away.
    if (close_reason_ == ModalCloseReason::Programmatic) {
        return;
    }
    if (config_.on_cancel) {
        auto cancel = config_.on_cancel;
        cancel();
    }
}

} // namespace helix::ui
