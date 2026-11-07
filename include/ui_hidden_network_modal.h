// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "async_lifetime_guard.h"
#include "subject_managed_panel.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace helix {
class WiFiManager;
}

namespace helix::ui {

/// What a hidden-network join reports to the screen that opened the modal.
enum class HiddenJoin {
    Started, ///< The typed SSID passed the form's checks and the join is sent
    Joined,  ///< The join succeeded; the modal has closed
    Failed,  ///< The join failed; the modal shows why and stays open
};

/// Called on the main thread with the event and the typed SSID. A join that ends after
/// the modal closed still reports Joined or Failed.
using HiddenJoinCallback = std::function<void(HiddenJoin event, const std::string& ssid)>;

/// Shows "Add Hidden Network": a typed SSID, its security and password, joined through
/// @p wifi as a hidden network (the backend must not look for it in its scan cache).
/// One shared instance serves the network settings overlay and the setup wizard.
/// Returns false when the dialog could not be built.
bool show_hidden_network_modal(std::shared_ptr<helix::WiFiManager> wifi,
                               HiddenJoinCallback on_event);

class HiddenNetworkModal : public Modal {
  public:
    HiddenNetworkModal() = default;
    ~HiddenNetworkModal() override;

    bool show_modal(std::shared_ptr<helix::WiFiManager> wifi, HiddenJoinCallback on_event);

    const char* get_name() const override {
        return "Hidden Network";
    }
    const char* component_name() const override {
        return "hidden_network_modal";
    }

  protected:
    void on_show() override;
    void on_hide() override;
    void on_ok() override;

  private:
    void init_subjects();
    void show_error(const std::string& message);

    SubjectManager subjects_;
    /// 1 while a join runs: the form and Connect give way to a spinner.
    lv_subject_t connecting_{};
    /// The form's error line; hidden while empty.
    lv_subject_t error_{};
    lv_subject_t has_error_{};
    char error_buf_[256] = {0};
    bool subjects_initialized_ = false;

    std::shared_ptr<helix::WiFiManager> wifi_;
    HiddenJoinCallback on_event_;
    /// Numbers each join, so one that ends after a later one started leaves the form alone.
    uint32_t join_generation_ = 0;
    /// Outlives hide(): a join keeps running when the modal closes, and still reports.
    helix::AsyncLifetimeGuard join_lifetime_;
};

} // namespace helix::ui
