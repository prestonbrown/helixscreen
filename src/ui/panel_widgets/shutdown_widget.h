// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_shutdown_modal.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"

#include <functional>
#include <string>

class IMoonrakerAPI;

namespace helix {

class ShutdownWidget : public PanelWidget {
  public:
    explicit ShutdownWidget(IMoonrakerAPI* api);
    ~ShutdownWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "shutdown";
    }

    // XML event callback (public for early registration)
    static void shutdown_clicked_cb(lv_event_t* e);

  private:
    IMoonrakerAPI* api_;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* shutdown_btn_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    ShutdownModal shutdown_modal_;

    // Lifetime guard for async callback safety
    helix::AsyncLifetimeGuard lifetime_;

    void handle_click();
};

void register_shutdown_widget();

/// Configure @p modal with single-/dual-scope callbacks for @p api (matching
/// the home-panel widget's behavior — including local-fallback when Moonraker
/// is disconnected) and show it as a child of @p parent_screen.
///
/// Caller owns @p modal and @p lifetime; both must outlive the modal. The
/// lifetime guard is required for the "shutdown both" / "reboot both" flows
/// that defer the local SystemPower call until the printer-side ack.
void show_shutdown_dialog(IMoonrakerAPI* api, ShutdownModal& modal, AsyncLifetimeGuard& lifetime,
                          lv_obj_t* parent_screen);

/// Handle a failed Moonraker machine.reboot / machine.shutdown.
///
/// Hosts running Moonraker with `provider: none` implement those RPCs as
/// `sudo systemctl reboot|poweroff`. Non-systemd printer hosts (OpenWrt/procd
/// K2, K1C, AD5M, CC1, U1) ship neither binary, so the RPC always fails even
/// though busybox /sbin/reboot works — which is what SystemPower falls back to.
///
/// @param err_message          Moonraker's error text, for the log.
/// @param is_reboot            Reboot vs. shutdown, selects the user-facing toast.
/// @param allow_local_fallback True only when Moonraker runs on THIS host, so
///                             powering down locally is the same act the user
///                             asked for. Never set it for a remote printer.
/// @param local_action         Performs the local power action; returns success.
/// @return true if the local fallback ran and succeeded. Otherwise an error
///         toast is shown and false is returned.
bool handle_machine_power_failure(const std::string& err_message, bool is_reboot,
                                  bool allow_local_fallback,
                                  const std::function<bool()>& local_action);

} // namespace helix
