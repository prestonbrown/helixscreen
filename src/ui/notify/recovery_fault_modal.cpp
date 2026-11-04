// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "recovery_fault_modal.h"

#include <spdlog/spdlog.h>

#include <array>

namespace helix::ui {

namespace {

/// modal_button_row's slots in the order actions fill them.
constexpr std::array<const char*, RecoveryFaultModal::kMaxActions> kSlots = {
    "primary", "secondary", "tertiary", "quaternary"};

} // namespace

const char* RecoveryFaultModal::variant_for_style(const std::string& style) {
    if (style == "primary") {
        return "primary";
    }
    if (style == "danger") {
        return "danger";
    }
    return "secondary";
}

bool RecoveryFaultModal::show_fault(lv_obj_t* parent, const std::string& title,
                                    const std::string& detail,
                                    const std::vector<helix::RecoveryAction>& actions) {
    // Close the one on screen first: its on_hide() hands the diagram back, which
    // would undo a publish made for the replacement.
    if (is_visible()) {
        hide();
    }

    actions_ = actions;
    // A fault the backend offers nothing for still needs a way to close it.
    if (actions_.empty()) {
        actions_.push_back({lv_tr("OK"), "", "recovery_fault::dismiss", "primary"});
    }
    if (actions_.size() > kMaxActions) {
        spdlog::warn("[{}] {} recovery actions, showing the first {}", get_name(), actions_.size(),
                     kMaxActions);
        actions_.resize(kMaxActions);
    }

    // Some firmware welds a monospace position diagram onto its lane faults,
    // which our proportional font renders as noise (#1184). Publish the stop point to the
    // <fault_path> graphic and drop the art rows from the text. An
    // unrecognised detail sets the subject to 0 (graphic hidden) and comes back
    // unchanged.
    const std::string message = fault_path_.apply(detail);

    // Every string the attr array points into lives until show() returns.
    std::vector<std::string> values;
    values.reserve(2 + 3 * kMaxActions);
    std::vector<std::string> keys;
    keys.reserve(2 + 3 * kMaxActions);
    auto add = [&](std::string key, std::string value) {
        keys.push_back(std::move(key));
        values.push_back(std::move(value));
    };
    add("title", title);
    add("message", message);
    for (size_t i = 0; i < kMaxActions; ++i) {
        const std::string slot = kSlots[i];
        const bool used = i < actions_.size();
        add(slot + "_text", used ? actions_[i].label : std::string{});
        add(slot + "_variant", variant_for_style(used ? actions_[i].style : std::string{}));
        if (i > 0) {
            add("hide_" + slot, used ? "false" : "true");
        }
    }
    std::vector<const char*> attrs;
    attrs.reserve(keys.size() * 2 + 1);
    for (size_t i = 0; i < keys.size(); ++i) {
        attrs.push_back(keys[i].c_str());
        attrs.push_back(values[i].c_str());
    }
    attrs.push_back(nullptr);

    if (!Modal::show(parent, attrs.data())) {
        fault_path_.release();
        return false;
    }
    return true;
}

void RecoveryFaultModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");
    wire_tertiary_button("btn_tertiary");
    wire_quaternary_button("btn_quaternary");
}

void RecoveryFaultModal::on_hide() {
    fault_path_.release();
    if (close_reason_ != ModalCloseReason::Programmatic && dismiss_callback_) {
        dismiss_callback_();
    }
}

void RecoveryFaultModal::run_action(size_t index) {
    if (index >= actions_.size()) {
        hide(ModalCloseReason::ButtonPress);
        return;
    }
    const std::string gcode = actions_[index].gcode;
    // The dialog closes after the callback, so the callback must not present a
    // new fault synchronously: it would be hidden here.
    // An empty gcode is the dismiss affordance: close, send nothing.
    if (!gcode.empty() && action_callback_) {
        action_callback_(gcode);
    }
    hide(ModalCloseReason::ButtonPress);
}

} // namespace helix::ui
