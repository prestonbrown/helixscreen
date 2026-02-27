// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "network_widget.h"

#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_overlay_network_settings.h"

#include "ethernet_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "panel_widget_registry.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"
#include "wifi_manager.h"

#include <spdlog/spdlog.h>

// Signal polling interval (5 seconds)
static constexpr uint32_t SIGNAL_POLL_INTERVAL_MS = 5000;

// Subjects owned by NetworkWidget module — created before XML bindings resolve
static lv_subject_t s_network_icon_state;
static lv_subject_t s_network_label_subject;
static char s_network_label_buffer[32];
static bool s_subjects_initialized = false;

static void network_widget_init_subjects() {
    if (s_subjects_initialized) {
        return;
    }

    // Integer subject: 0=disconnected, 1-4=wifi strength, 5=ethernet
    lv_subject_init_int(&s_network_icon_state, 0);
    lv_xml_register_subject(nullptr, "home_network_icon_state", &s_network_icon_state);
    SubjectDebugRegistry::instance().register_subject(
        &s_network_icon_state, "home_network_icon_state", LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);

    // String subject for network type label
    lv_subject_init_string(&s_network_label_subject, s_network_label_buffer, nullptr,
                           sizeof(s_network_label_buffer), "WiFi");
    lv_xml_register_subject(nullptr, "network_label", &s_network_label_subject);
    SubjectDebugRegistry::instance().register_subject(&s_network_label_subject, "network_label",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

    s_subjects_initialized = true;

    // Self-register cleanup with StaticSubjectRegistry (co-located with init)
    // Subjects must be deinitialized AFTER panels remove their observers (Phase 2)
    StaticSubjectRegistry::instance().register_deinit("NetworkWidgetSubjects", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            lv_subject_deinit(&s_network_label_subject);
            lv_subject_deinit(&s_network_icon_state);
            s_subjects_initialized = false;
            spdlog::trace("[NetworkWidget] Subjects deinitialized");
        }
    });

    spdlog::debug("[NetworkWidget] Subjects initialized (icon_state + label)");
}

namespace helix {
void register_network_widget() {
    register_widget_factory("network", []() { return std::make_unique<NetworkWidget>(); });
    register_widget_subjects("network", network_widget_init_subjects);

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "network_clicked_cb", NetworkWidget::network_clicked_cb);
}
} // namespace helix

using namespace helix;

NetworkWidget::NetworkWidget() = default;

NetworkWidget::~NetworkWidget() {
    detach();
}

void NetworkWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Store this pointer for event callback recovery
    lv_obj_set_user_data(widget_obj_, this);

    // Use module-owned subjects (initialized via network_widget_init_subjects)
    network_icon_state_ = &s_network_icon_state;
    network_label_subject_ = &s_network_label_subject;

    // Get WiFiManager for signal strength queries
    wifi_manager_ = get_wifi_manager();

    // Initialize EthernetManager for Ethernet status detection
    ethernet_manager_ = std::make_unique<EthernetManager>();

    // Detect actual network type (Ethernet vs WiFi vs disconnected)
    detect_network_type();

    // Start signal polling timer if on WiFi
    if (!signal_poll_timer_ && current_network_ == NetworkType::Wifi) {
        signal_poll_timer_ = lv_timer_create(signal_poll_timer_cb, SIGNAL_POLL_INTERVAL_MS, this);
        spdlog::debug("[NetworkWidget] Started signal polling timer ({}ms)",
                      SIGNAL_POLL_INTERVAL_MS);
    }

    spdlog::debug("[NetworkWidget] Attached");
}

void NetworkWidget::detach() {
    if (lv_is_initialized() && signal_poll_timer_) {
        lv_timer_delete(signal_poll_timer_);
        signal_poll_timer_ = nullptr;
    }

    ethernet_manager_.reset();
    wifi_manager_.reset();
    network_icon_state_ = nullptr;
    network_label_subject_ = nullptr;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;

    spdlog::debug("[NetworkWidget] Detached");
}

void NetworkWidget::on_activate() {
    // Re-detect network type in case it changed while on another panel
    detect_network_type();

    // Start signal polling timer when panel becomes visible (only for WiFi)
    if (!signal_poll_timer_ && current_network_ == NetworkType::Wifi) {
        signal_poll_timer_ = lv_timer_create(signal_poll_timer_cb, SIGNAL_POLL_INTERVAL_MS, this);
        spdlog::debug("[NetworkWidget] Started signal polling timer ({}ms interval)",
                      SIGNAL_POLL_INTERVAL_MS);
    }
}

void NetworkWidget::on_deactivate() {
    // Stop signal polling timer when panel is hidden (saves CPU)
    if (signal_poll_timer_) {
        lv_timer_delete(signal_poll_timer_);
        signal_poll_timer_ = nullptr;
        spdlog::debug("[NetworkWidget] Stopped signal polling timer");
    }
}

void NetworkWidget::detect_network_type() {
    // Priority: Ethernet > WiFi > Disconnected
    // Ensures users on wired connections see the Ethernet icon even if WiFi is also available

    // Check Ethernet first (higher priority - more reliable connection)
    if (ethernet_manager_) {
        EthernetInfo eth_info = ethernet_manager_->get_info();
        if (eth_info.connected) {
            spdlog::debug("[NetworkWidget] Detected Ethernet connection on {} ({})",
                          eth_info.interface, eth_info.ip_address);
            set_network(NetworkType::Ethernet);
            return;
        }
    }

    // Check WiFi second
    if (wifi_manager_ && wifi_manager_->is_connected()) {
        spdlog::info("[NetworkWidget] Detected WiFi connection ({})",
                     wifi_manager_->get_connected_ssid());
        set_network(NetworkType::Wifi);
        return;
    }

    // Neither connected
    spdlog::info("[NetworkWidget] No network connection detected");
    set_network(NetworkType::Disconnected);
}

void NetworkWidget::set_network(NetworkType type) {
    current_network_ = type;

    // Update label text via subject
    if (network_label_subject_) {
        switch (type) {
        case NetworkType::Wifi:
            lv_subject_copy_string(network_label_subject_, lv_tr("WiFi"));
            break;
        case NetworkType::Ethernet:
            lv_subject_copy_string(network_label_subject_, lv_tr("Ethernet"));
            break;
        case NetworkType::Disconnected:
            lv_subject_copy_string(network_label_subject_, lv_tr("Disconnected"));
            break;
        }
    }

    // Update the icon state (will query WiFi signal strength if connected)
    update_network_icon_state();

    spdlog::debug("[NetworkWidget] Network type set to {} (icon state computed)",
                  static_cast<int>(type));
}

int NetworkWidget::compute_network_icon_state() {
    // State values:
    // 0 = Disconnected (wifi_off, disabled variant)
    // 1 = WiFi strength 1 (<=25%, warning variant)
    // 2 = WiFi strength 2 (26-50%, accent variant)
    // 3 = WiFi strength 3 (51-75%, accent variant)
    // 4 = WiFi strength 4 (>75%, accent variant)
    // 5 = Ethernet connected (accent variant)

    if (current_network_ == NetworkType::Disconnected) {
        spdlog::trace("[NetworkWidget] Network disconnected -> state 0");
        return 0;
    }

    if (current_network_ == NetworkType::Ethernet) {
        spdlog::trace("[NetworkWidget] Network ethernet -> state 5");
        return 5;
    }

    // WiFi - get signal strength from WiFiManager
    int signal = 0;
    if (wifi_manager_) {
        signal = wifi_manager_->get_signal_strength();
        spdlog::trace("[NetworkWidget] WiFi signal strength: {}%", signal);
    } else {
        spdlog::warn("[NetworkWidget] WiFiManager not available for signal query");
    }

    // Map signal percentage to icon state (1-4)
    int state;
    if (signal <= 25)
        state = 1; // Weak (warning)
    else if (signal <= 50)
        state = 2; // Fair
    else if (signal <= 75)
        state = 3; // Good
    else
        state = 4; // Strong

    spdlog::trace("[NetworkWidget] WiFi signal {}% -> state {}", signal, state);
    return state;
}

void NetworkWidget::update_network_icon_state() {
    if (!network_icon_state_) {
        return;
    }

    int new_state = compute_network_icon_state();
    int old_state = lv_subject_get_int(network_icon_state_);

    if (new_state != old_state) {
        lv_subject_set_int(network_icon_state_, new_state);
        spdlog::debug("[NetworkWidget] Network icon state: {} -> {}", old_state, new_state);
    }
}

void NetworkWidget::handle_network_clicked() {
    spdlog::info("[NetworkWidget] Network icon clicked - opening network settings directly");

    // Open Network settings overlay directly (same as Settings panel's Network row)
    auto& overlay = get_network_settings_overlay();

    if (!overlay.is_created()) {
        overlay.init_subjects();
        overlay.register_callbacks();
        overlay.create(parent_screen_);
    }

    overlay.show();
}

void NetworkWidget::signal_poll_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<NetworkWidget*>(lv_timer_get_user_data(timer));
    if (self && self->current_network_ == NetworkType::Wifi) {
        self->update_network_icon_state();
    }
}

void NetworkWidget::network_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[NetworkWidget] network_clicked_cb");

    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<NetworkWidget*>(lv_obj_get_user_data(target));
    if (self) {
        self->handle_network_clicked();
    } else {
        spdlog::warn("[NetworkWidget] network_clicked_cb: could not recover widget instance");
    }

    LVGL_SAFE_EVENT_CB_END();
}
