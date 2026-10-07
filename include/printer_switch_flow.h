// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"

#include <functional>
#include <string>

class ApplicationTestAccess; // NAMESPACE_OK: test accessor, declared at global scope

namespace helix {
class Config;

/// The state machine behind switching to another printer, adding one through the setup
/// wizard, and backing out of that wizard. It decides what the config says and in which
/// order the restart steps run; the steps arrive as hooks, because the desktop rebuilds the
/// whole UI while the K-Touch retargets its one connection.
class PrinterSwitchFlow {
  public:
    /// Teardown releases the current printer, rebuild brings up the active one, land_home
    /// shows the home panel.
    struct Restart {
        std::function<void()> teardown;
        std::function<void()> rebuild;
        std::function<void()> land_home;
    };

    /// `config` is read through the reference because its owner assigns it after
    /// construction.
    PrinterSwitchFlow(Config*& config, AsyncLifetimeGuard& async, Restart restart);

    /// Switches to `printer_id`, asking first when the current printer is printing.
    /// Picking the connected printer does nothing.
    void request_switch(const std::string& printer_id);

    /// Adds the printer at `host`:`port` and switches to it the way request_switch() does. An
    /// address already in the list switches to that printer instead of adding a duplicate.
    void add_printer(const std::string& host, int port);

    /// The printer the app is connected to. Compared against instead of the config's active
    /// id, which a removal moves to another printer before the switch is requested.
    [[nodiscard]] const std::string& connected_printer_id() const {
        return m_connected_printer_id;
    }

    /// Records the printer the owner connected to outside a switch, at boot.
    void set_connected_printer_id(std::string printer_id) {
        m_connected_printer_id = std::move(printer_id);
    }

    /// Makes `printer_id` the active printer and restarts onto it. Ignored while a restart
    /// or a switch confirmation is running; an unknown id changes nothing.
    void switch_printer(const std::string& printer_id);

    /// Creates an empty printer entry, makes it active and restarts into its setup wizard.
    void add_printer_via_wizard();

    /// Abandons the printer the wizard was adding, restores the previous one and restarts
    /// onto it. The restart is deferred past the click handler that called this.
    void cancel_add_printer_wizard();

    /// The printer to restore if the add-printer wizard is cancelled; empty when no such
    /// wizard is running.
    [[nodiscard]] const std::string& wizard_previous_printer_id() const {
        return m_wizard_previous_printer_id;
    }

    /// The add-printer wizard finished: there is nothing left to cancel back to.
    void clear_wizard_previous_printer_id() {
        m_wizard_previous_printer_id.clear();
    }

  private:
    friend class ::ApplicationTestAccess;
    friend class PrinterSwitchFlowTestAccess;

    Config*& m_config;
    AsyncLifetimeGuard& m_async;
    Restart m_restart;

    /// A restart is running; a second switch or add is a no-op until it ends.
    bool m_soft_restart_in_progress = false;

    /// A "the printer is printing" confirmation is on screen.
    bool m_confirm_pending = false;

    std::string m_wizard_previous_printer_id;
    std::string m_connected_printer_id;

    /// Saves the config, telling the user when it could not.
    bool save_or_report();
};

} // namespace helix
