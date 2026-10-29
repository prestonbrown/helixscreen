// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_connection.h"

#include "ui_callback_helpers.h"
#include "ui_change_host_modal.h"
#include "ui_overlay_network_settings.h"
#include "ui_printer_list_overlay.h"
#include "ui_settings_screen_sharing.h"

#include "config.h"
#include "printer_retarget.h"
#include "utils/network_validation.h"

#include <spdlog/spdlog.h>

namespace helix::settings {

ConnectionSettingsOverlay::~ConnectionSettingsOverlay() {
    deinit_subjects();
}

std::string ConnectionSettingsOverlay::printer_host_display() {
    Config* config = Config::get_instance();
    const std::string host = config->get<std::string>(config->df() + "moonraker_host", "");
    if (host.empty()) {
        return "\xe2\x80\x94";
    }
    const int port =
        config->get<int>(config->df() + "moonraker_port", helix::kDefaultMoonrakerPort);
    return host + ":" + std::to_string(port);
}

std::string ConnectionSettingsOverlay::printer_api_key_display() {
    return active_printer_credential().api_key.empty() ? lv_tr("Not set") : lv_tr("Configured");
}

void ConnectionSettingsOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        // Seeded from config so the first paint shows the current host:port
        // rather than waiting for ChangeHostModal's completion callback.
        UI_MANAGED_SUBJECT_STRING(printer_host_value_subject_, printer_host_value_buf_,
                                  printer_host_display().c_str(), "printer_host_value", subjects_);
        UI_MANAGED_SUBJECT_STRING(printer_api_key_value_subject_, printer_api_key_value_buf_,
                                  printer_api_key_display().c_str(), "printer_api_key_value",
                                  subjects_);
    });
}

void ConnectionSettingsOverlay::deinit_subjects() {
    deinit_subjects_base(subjects_);
}

void ConnectionSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_network_clicked",
         [](lv_event_t*) {
             get_network_settings_overlay().show(get_connection_settings_overlay().parent_screen_);
         }},
        {"on_printers_clicked",
         [](lv_event_t*) {
             helix::ui::get_printer_list_overlay().show(
                 get_connection_settings_overlay().parent_screen_);
         }},
        {"on_change_host_clicked",
         [](lv_event_t*) {
             spdlog::debug("[Connection] Change Host clicked");
             // Ownership and the reconnect sequence live in show_change_host_modal(); the
             // connection-failed prompt reaches the same modal, and duplicating the
             // reconnect here is how the two would drift.
             helix::ui::show_change_host_modal([](bool changed) {
                 if (changed) {
                     get_connection_settings_overlay().refresh_printer_host();
                 }
             });
         }},
        {"on_screen_sharing_clicked",
         [](lv_event_t*) {
             get_screen_sharing_settings_overlay().show(
                 get_connection_settings_overlay().parent_screen_);
         }},
    });
}

void ConnectionSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    refresh_printer_host();
}

void ConnectionSettingsOverlay::refresh_printer_host() {
    if (!subjects_initialized_) {
        return;
    }
    lv_subject_copy_string(&printer_host_value_subject_, printer_host_display().c_str());
    lv_subject_copy_string(&printer_api_key_value_subject_, printer_api_key_display().c_str());
}

} // namespace helix::settings
