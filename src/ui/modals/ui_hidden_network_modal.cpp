// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_hidden_network_modal.h"

#include "ui_keyboard_manager.h"
#include "ui_utils.h"

#include "log_redact.h"
#include "static_panel_registry.h"
#include "wifi_manager.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <string>
#include <utility>

namespace helix::ui {

HiddenNetworkModal::~HiddenNetworkModal() {
    join_lifetime_.invalidate();
    if (subjects_initialized_) {
        subjects_.deinit_all();
    }
}

void HiddenNetworkModal::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    lv_subject_init_int(&connecting_, 0);
    lv_subject_init_string(&error_, error_buf_, nullptr, sizeof(error_buf_), "");
    lv_subject_init_int(&has_error_, 0);
    subjects_.publish("hidden_connecting", &connecting_);
    subjects_.publish("hidden_network_error", &error_);
    subjects_.publish("hidden_network_has_error", &has_error_);
    subjects_initialized_ = true;
}

bool HiddenNetworkModal::show_modal(std::shared_ptr<helix::WiFiManager> wifi,
                                    HiddenJoinCallback on_event) {
    init_subjects();
    wifi_ = std::move(wifi);
    on_event_ = std::move(on_event);
    lv_subject_set_int(&connecting_, 0);
    show_error("");
    return show(lv_screen_active());
}

void HiddenNetworkModal::on_show() {
    wire_ok_button("connect_btn");
    wire_cancel_button("cancel_btn");

    if (lv_obj_t* ssid_input = find_widget("ssid_input")) {
        modal_register_keyboard(dialog(), ssid_input);
        // The SSID is the first thing typed.
        // Security is the next choice to make, so it stays above the keyboard
        // while the SSID is typed.
        KeyboardManager::instance().set_reveal_target(ssid_input, find_widget("security_dropdown"));
        if (lv_group_get_default()) {
            lv_group_focus_obj(ssid_input);
        }
    }
    if (lv_obj_t* password_input = find_widget("password_input")) {
        modal_register_keyboard(dialog(), password_input);
    }
}

void HiddenNetworkModal::on_hide() {
    // A join in flight keeps what it captured; the next showing brings its own.
    wifi_.reset();
    on_event_ = nullptr;
}

void HiddenNetworkModal::show_error(const std::string& message) {
    lv_subject_copy_string(&error_, message.c_str());
    lv_subject_set_int(&has_error_, message.empty() ? 0 : 1);
}

void HiddenNetworkModal::on_ok() {
    lv_obj_t* ssid_input = find_widget("ssid_input");
    const char* typed = ssid_input ? lv_textarea_get_text(ssid_input) : nullptr;
    const std::string ssid = typed ? typed : "";
    if (ssid.empty()) {
        show_error(lv_tr("Network name cannot be empty"));
        return;
    }

    // Security index 0 is "None", which takes no password.
    lv_obj_t* security_dropdown = find_widget("security_dropdown");
    const uint32_t security = security_dropdown ? lv_dropdown_get_selected(security_dropdown) : 0;
    std::string password;
    if (security > 0) {
        lv_obj_t* password_input = find_widget("password_input");
        const char* pwd = password_input ? lv_textarea_get_text(password_input) : nullptr;
        password = pwd ? pwd : "";
        if (password.empty()) {
            show_error(lv_tr("Password is required for secured networks"));
            return;
        }
    }

    if (!wifi_) {
        spdlog::error("[HiddenNetworkModal] No WiFi manager");
        show_error(lv_tr("WiFi unavailable"));
        return;
    }

    spdlog::info("[HiddenNetworkModal] Joining hidden network {} (security {})",
                 helix::redact::ssid(ssid), security);
    show_error("");
    lv_subject_set_int(&connecting_, 1);
    const uint32_t generation = ++join_generation_;
    if (on_event_) {
        on_event_(HiddenJoin::Started, ssid);
    }

    auto token = join_lifetime_.token();
    wifi_->connect(
        ssid, password,
        [this, token, ssid, generation,
         on_event = on_event_](bool success, const std::string& error, WiFiResult result) {
            token.defer([this, ssid, generation, on_event, success, error, result]() {
                // A later join owns the form; this one only reports.
                const bool current = generation == join_generation_ && is_visible();
                if (current) {
                    lv_subject_set_int(&connecting_, 0);
                }
                if (success) {
                    spdlog::info("[HiddenNetworkModal] Joined hidden network {}",
                                 helix::redact::ssid(ssid));
                    if (current) {
                        hide(ModalCloseReason::ButtonPress);
                    }
                } else {
                    spdlog::error("[HiddenNetworkModal] Joining hidden network failed: {}", error);
                    if (current) {
                        show_error(helix::connect_failure_message(
                            result, error, lv_tr("Connection failed. Check credentials.")));
                    }
                }
                if (on_event) {
                    on_event(success ? HiddenJoin::Joined : HiddenJoin::Failed, ssid);
                }
            });
        },
        // The SSID was typed, not picked from the scan list: the backend must not
        // match it against a scan cache.
        /*is_hidden=*/true);
}

bool show_hidden_network_modal(std::shared_ptr<helix::WiFiManager> wifi,
                               HiddenJoinCallback on_event) {
    auto& modal = helix::lazy_global<HiddenNetworkModal>("HiddenNetworkModal");
    if (modal.is_visible()) {
        return true;
    }
    if (!modal.show_modal(std::move(wifi), std::move(on_event))) {
        spdlog::error("[HiddenNetworkModal] Failed to show");
        return false;
    }
    return true;
}

} // namespace helix::ui
