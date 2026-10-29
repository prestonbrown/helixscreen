// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_connection.h
 * @brief Connection settings overlay - printers, network, Moonraker host, screen sharing
 *
 * Owns its row callbacks (on_network_clicked, on_printers_clicked,
 * on_change_host_clicked, on_screen_sharing_clicked) and the printer_host_value and
 * printer_api_key_value subjects the Host and API Key rows bind.
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 */

#pragma once

#include "overlay_base.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"

#include <string>

namespace helix::settings {

class ConnectionSettingsOverlay : public OverlayBase {
  public:
    ~ConnectionSettingsOverlay() override;

    const char* get_name() const override {
        return "Connection";
    }
    const char* xml_component() const override {
        return "settings_connection_overlay";
    }

    void init_subjects() override;
    void deinit_subjects();
    void register_callbacks() override;
    void on_activate() override;

    /// "host:port" of the configured printer, or an em dash when none is set.
    static std::string printer_host_display();

    /// Whether the configured printer has an API key; never the key itself.
    static std::string printer_api_key_display();

  private:
    void refresh_printer_host();

    SubjectManager subjects_;
    lv_subject_t printer_host_value_subject_{};
    char printer_host_value_buf_[96] = {};
    lv_subject_t printer_api_key_value_subject_{};
    char printer_api_key_value_buf_[48] = {};
};

inline ConnectionSettingsOverlay& get_connection_settings_overlay() {
    return lazy_global<ConnectionSettingsOverlay>("ConnectionSettingsOverlay");
}

} // namespace helix::settings
