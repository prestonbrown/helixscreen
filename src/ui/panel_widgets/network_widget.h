// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "panel_widget.h"

#include <memory>

class EthernetManager;

#include "network_type.h"

namespace helix {
class WiFiManager;

class NetworkWidget : public PanelWidget {
  public:
    NetworkWidget();
    ~NetworkWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "network";
    }

    /// Called when panel activates — re-detects network and starts polling
    void on_activate() override;
    /// Called when panel deactivates — stops polling
    void on_deactivate() override;

  private:
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    // Module-level subjects owned by network_widget.cpp
    // (initialized via register_widget_subjects → PanelWidgetManager::init_widget_subjects)
    lv_subject_t* network_icon_state_ = nullptr;
    lv_subject_t* network_label_subject_ = nullptr;

    NetworkType current_network_ = NetworkType::Wifi;
    lv_timer_t* signal_poll_timer_ = nullptr;
    std::shared_ptr<WiFiManager> wifi_manager_;
    std::unique_ptr<EthernetManager> ethernet_manager_;

    void detect_network_type();
    int compute_network_icon_state();
    void update_network_icon_state();
    void set_network(NetworkType type);
    void handle_network_clicked();

    static void signal_poll_timer_cb(lv_timer_t* timer);
    static void network_clicked_cb(lv_event_t* e);
};

} // namespace helix
