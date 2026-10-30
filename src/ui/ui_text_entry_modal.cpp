// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_text_entry_modal.h"

#include "ui_error_reporting.h"
#include "ui_keyboard_manager.h"

#include "lvgl.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::ui {

TextEntryModal::TextEntryModal(Config config) : config_(std::move(config)) {}

lv_obj_t* TextEntryModal::show_owned(Config config) {
    auto modal = std::make_unique<TextEntryModal>(std::move(config));
    const std::string accept =
        modal->config_.accept_text.empty() ? std::string(lv_tr("OK")) : modal->config_.accept_text;
    const char* attrs[] = {"title",       modal->config_.title.c_str(),
                           "placeholder", modal->config_.placeholder.c_str(),
                           "accept_text", accept.c_str(),
                           nullptr};
    if (!modal->show(lv_screen_active(), attrs))
        return nullptr;
    lv_obj_t* dialog = modal->dialog();
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
    return dialog;
}

void TextEntryModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");
    input_ = find_widget("text_entry_input");
    if (!input_) {
        spdlog::error("[{}] text_entry_input not found", get_name());
        return;
    }
    if (config_.max_length > 0)
        lv_textarea_set_max_length(input_, config_.max_length);
    lv_textarea_set_password_mode(input_, config_.password);
    lv_textarea_set_text(input_, config_.initial.c_str());
    KeyboardManager::instance().show(input_);
}

void TextEntryModal::on_ok() {
    const std::string text = input_ ? lv_textarea_get_text(input_) : config_.initial;
    if (config_.validate) {
        const std::string why = config_.validate(text);
        if (!why.empty()) {
            NOTIFY_ERROR("{}", why);
            return;
        }
    }
    answered_ = true;
    auto accept = config_.on_accept;
    hide(ModalCloseReason::ButtonPress);
    if (accept)
        accept(text);
}

void TextEntryModal::on_cancel() {
    hide(ModalCloseReason::ButtonPress);
    cancel_once();
}

void TextEntryModal::on_dismissed() {
    cancel_once();
}

void TextEntryModal::cancel_once() {
    if (answered_)
        return;
    answered_ = true;
    if (config_.on_cancel)
        config_.on_cancel();
}

} // namespace helix::ui
