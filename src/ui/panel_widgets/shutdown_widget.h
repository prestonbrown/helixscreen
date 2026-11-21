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

/// Which power dialog a screen gets.
enum class ShutdownDialogMode {
    SingleViaMoonraker,  ///< Same host, Moonraker up: machine.shutdown takes the device down.
    SingleViaLocalPower, ///< Same host, Moonraker down or unable to: SystemPower on this host.
    Dual,                ///< Different hosts: Both / Printer / Screen split buttons.
};

/// Pure. A screen on Moonraker's own host (@p configured_host_same, from
/// is_moonraker_on_same_host()) gets one row of buttons; any other gets the
/// split buttons.
///
/// A same-host screen goes straight to SystemPower when Moonraker is not
/// connected, or when this host does not run systemd (@p this_host_runs_systemd,
/// from host_runs_systemd()): Moonraker's machine.reboot/shutdown there shells
/// out to sudo/systemctl and always fails.
ShutdownDialogMode plan_shutdown_dialog(bool configured_host_same, bool api_connected,
                                        bool this_host_runs_systemd);

/// Run @p power; only when it dispatched, @p stop_drawing and then @p clear.
/// LVGL repaints over a clear within a frame unless drawing has stopped, and
/// a clear made before a slow or failing request leaves a frozen black screen.
/// Returns what @p power returned (true = dispatched). Pure apart from the
/// callbacks.
bool power_then_hold_black(const std::function<bool()>& power,
                           const std::function<void()>& stop_drawing,
                           const std::function<void()>& clear);

/// What a same-host screen does after asking SystemPower to power this host.
enum class HostPowerFollowUp {
    Done,              ///< Dispatched; init takes it from here.
    RetryViaMoonraker, ///< Failed locally; a connected Moonraker gets the request.
    Toast,             ///< Failed with no Moonraker to ask: the machine's failure toast.
};

/// Pure. Dispatched is Done; a failure retries through a connected
/// Moonraker, or toasts.
HostPowerFollowUp host_power_follow_up(bool local_dispatched, bool api_connected);

/// True while a job holds the machine (preparing, printing or paused),
/// so powering it down would ruin the job. Every path that reboots the printer
/// outside the power dialog reads it. Main thread.
bool power_job_active();

/// True when power_job_active() reflects Klipper: Moonraker connected, Klipper
/// ready, and a print_stats frame applied since start. Before that the job
/// state reads its STANDBY default, so "no job" is not yet known. Main thread.
bool power_job_state_known();

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
/// `sudo systemctl reboot|poweroff`, which fails without passwordless sudo
/// even where SystemPower can power the host itself. A non-systemd same-host
/// screen goes to SystemPower first, but still reaches these RPCs when local
/// power fails with Moonraker connected, and through the Dual dialog.
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
