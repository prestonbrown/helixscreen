// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_shutdown_modal.h"

#include "async_lifetime_guard.h"
#include "src/ui/panel_widgets/tiled_panel_widget.h"
#include "system_power.h"

#include <functional>
#include <string>

namespace helix {
class IMoonrakerAPI;

class ShutdownWidget : public TiledPanelWidget {
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

  protected:
    /// Drop the cached tile pointers and expire the guard when the tile tree
    /// dies without a detach() — screen teardown.
    void on_hooked_root_deleted() override;

  private:
    friend class ShutdownWidgetTestAccess;

    IMoonrakerAPI* api_;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    ShutdownModal shutdown_modal_;

    // Lifetime guard for async callback safety
    helix::AsyncLifetimeGuard lifetime_;

    void handle_click();
};

void register_shutdown_widget();

/// Translated toast text for a failed screen power action, or nullptr when
/// @p result is Dispatched. A missing polkit grant gets its own text, since
/// restarting the service is what installs the rule.
const char* screen_power_failure_message(PowerResult result, bool is_reboot);

/// Which power dialog a screen gets.
enum class ShutdownDialogMode {
    SingleViaMoonraker,  ///< Same host, Moonraker up: machine.shutdown takes the device down.
    SingleViaLocalPower, ///< Same host, Moonraker down or unable to: SystemPower on this host.
    Dual,                ///< Different hosts: Both / Printer / Screen split buttons.
};

struct ShutdownDialogPlan {
    ShutdownDialogMode mode;
    /// Scope the split buttons open on. Read only in Dual mode.
    ShutdownScope dual_default;
};

/// Decide the dialog for this screen. Pure.
///
/// While connected, the live verdict (@p live_moonraker_remote, published on
/// every CONNECTED edge) decides same-host vs remote. Before the first
/// CONNECTED edge that verdict sits at its default (local), so the configured
/// host (@p configured_host_same, from is_moonraker_on_same_host()) decides.
///
/// A same-host screen goes straight to SystemPower when this host does not run
/// systemd (@p this_host_runs_systemd, from host_runs_systemd()): Moonraker's
/// machine.reboot/shutdown there shells out to sudo/systemctl and always fails.
///
/// A remote screen opens on Screen: the user is standing at the screen, and
/// powering down a printer on another machine has to be chosen on purpose.
ShutdownDialogPlan plan_shutdown_dialog(bool api_connected, bool live_moonraker_remote,
                                        bool configured_host_same, bool this_host_runs_systemd);

/// Run @p power; only when it dispatched, @p stop_drawing and then @p clear.
/// LVGL repaints over a clear within a frame unless drawing has stopped, and
/// a clear made before a slow or failing request leaves a frozen black screen.
/// Returns what @p power returned. Pure apart from the callbacks.
PowerResult power_then_hold_black(const std::function<PowerResult()>& power,
                                  const std::function<void()>& stop_drawing,
                                  const std::function<void()>& clear);

/// What a same-host screen does after asking SystemPower to power this host.
enum class HostPowerFollowUp {
    Done,              ///< Dispatched; init takes it from here.
    RetryViaMoonraker, ///< Failed locally; a connected Moonraker gets the request.
    Toast,             ///< Failed with no Moonraker to ask: machine_power_failure_message().
};

/// Pure. Dispatched is Done; anything else retries through a connected
/// Moonraker, or toasts.
HostPowerFollowUp host_power_follow_up(PowerResult local_result, bool api_connected);

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
/// even where SystemPower can power the host itself. (A same-host, non-systemd
/// dialog tries SystemPower first and sends the RPC only as its retry.)
///
/// @param err_message          Moonraker's error text, for the log.
/// @param is_reboot            Reboot vs. shutdown, selects the user-facing toast.
/// @param allow_local_fallback True only when Moonraker runs on THIS host, so
///                             powering down locally is the same act the user
///                             asked for. Never set it for a remote printer.
/// @param local_action         Performs the local power action.
/// @return Dispatched if the local fallback ran and succeeded. Otherwise the
///         local result (Failed when no fallback ran), and the toast from
///         machine_power_failure_message() is shown.
PowerResult handle_machine_power_failure(const std::string& err_message, bool is_reboot,
                                         bool allow_local_fallback,
                                         const std::function<PowerResult()>& local_action);

/// Translated toast text for a failed printer-host power action, given what
/// the local fallback returned (Failed when none ran), or nullptr when it was
/// Dispatched. An authorization refusal gets the same text as the screen path.
const char* machine_power_failure_message(PowerResult local_result, bool is_reboot);

} // namespace helix
