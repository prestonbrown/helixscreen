// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_timer_guard.h"
#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "helix_shared_callback.h"

#include <functional>
#include <lvgl.h>
#include <string>

class ApplicationTestAccess; // NAMESPACE_OK: test accessor, declared at global scope

namespace helix {
class Config;

/// Whether the app's Moonraker connection is up or on its way up; false only when it is
/// disconnected or has failed. Main thread only.
[[nodiscard]] bool printer_connection_live();

/// The state machine behind switching to another printer, adding one through the setup
/// wizard, and backing out of that wizard. It decides what the config says and in which
/// order the restart steps run; the steps arrive as hooks, because the desktop rebuilds the
/// whole UI while the K-Touch retargets its one connection.
///
/// Every restart runs from a top-level LVGL timer tick, never inside an UpdateQueue drain:
/// panel code calls UpdateQueue::drain() from its own methods, and a restart run by that
/// drain would destroy the panel still on the stack under it. A public call made during a
/// drain is moved to the next tick and returns false. One request waits for that tick at a
/// time, and the latest one wins: a later switch, add or confirmed switch replaces the one
/// waiting, and a wizard cancel drops it. A cancel restores the previous printer at once and
/// leaves only its restart for the tick, which runs before any request made after it.
class PrinterSwitchFlow {
  public:
    /// Teardown releases the current printer, rebuild brings up the active one and reports
    /// whether it started connecting, land_home shows the home panel.
    struct Restart {
        helix::SharedCallback<void()> teardown;
        /// Returns whether a connection to the new printer was started.
        helix::SharedCallback<bool()> rebuild;
        helix::SharedCallback<void()> land_home;
    };

    /// `config` is read through the reference because its owner assigns it after
    /// construction.
    PrinterSwitchFlow(Config*& config, AsyncLifetimeGuard& async, Restart restart);
    ~PrinterSwitchFlow();

    PrinterSwitchFlow(const PrinterSwitchFlow&) = delete;
    PrinterSwitchFlow& operator=(const PrinterSwitchFlow&) = delete;

    /// How long the switch card waits for the new printer to connect before it steps aside.
    static constexpr uint32_t kConnectWaitMs = 30000;

    /// Switches to `printer_id`, asking first when the current printer is printing.
    /// Picking the connected printer does nothing while its connection is up, and connects it
    /// otherwise. A pick that switches clears a boot-crash connection hold. Returns whether it
    /// switched before returning; asking first, or moving to the next tick, returns false.
    bool request_switch(const std::string& printer_id);

    /// Adds the printer at `host`:`port` and switches to it the way request_switch() does. An
    /// address already in the list switches to that printer instead of adding a duplicate.
    /// Returns whether it switched before returning.
    bool add_printer(const std::string& host, int port);

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
    /// or a switch confirmation is running; an unknown id changes nothing. Returns whether
    /// it switched.
    bool switch_printer(const std::string& printer_id);

    /// Creates an empty printer entry, makes it active and restarts into its setup wizard.
    void add_printer_via_wizard();

    /// Abandons the printer the wizard was adding, restores the previous one and restarts
    /// onto it. The restart runs on the next tick, past the click handler that called this.
    void cancel_add_printer_wizard();

    /// The printer to restore if the add-printer wizard is cancelled; empty when no such
    /// wizard is running.
    [[nodiscard]] const std::string& wizard_previous_printer_id() const {
        return m_wizard_previous_printer_id;
    }

    /// The add-printer wizard finished, or a switch left it: there is nothing left to cancel
    /// back to.
    void clear_wizard_previous_printer_id() {
        m_wizard_previous_printer_id.clear();
        m_wizard_replaced_record = {};
    }

  private:
    friend class ::ApplicationTestAccess;
    friend class PrinterSwitchFlowTestAccess;

    Config*& m_config;
    AsyncLifetimeGuard& m_async;
    Restart m_restart;

    /// A restart is running; a second switch or add is a no-op until it ends.
    bool m_soft_restart_in_progress = false;

    /// The last "the printer is printing" confirmation shown; may already be closed.
    lv_obj_t* m_confirm_dialog = nullptr;

    /// Whether that confirmation is still on screen. A hidden one is closed and forgotten.
    bool confirm_pending();

    /// The boot-crash bookkeeping (boot_crash_guard.h) as a move found it.
    struct BootCrashRecord {
        bool connect_held = false;
        std::string previous_printer_id;
        int crash_streak = 0;
    };

    std::string m_wizard_previous_printer_id;
    /// The record the add-printer wizard's move replaced; cancelling the wizard puts it back.
    BootCrashRecord m_wizard_replaced_record;
    std::string m_connected_printer_id;

    /// The card on the top layer that covers a restart; null when none is up.
    helix::ui::WidgetRef m_interstitial;
    ObserverGuard m_connect_observer;
    ui::LvglTimerGuard m_connect_timeout;

    /// Puts up the switch card reading `title` and `phase`, replacing any card already up.
    void show_interstitial(const std::string& title, const char* phase);
    /// Puts up the switch card reading "Loading..." and paints it before the restart blocks.
    void paint_loading_card(const std::string& title);
    /// The restart is done: the card reads "Connecting..." until the new printer connects,
    /// fails, or kConnectWaitMs passes. A setup wizard on screen, or a rebuild that never
    /// started connecting (`connecting` false), takes the card down at once.
    void await_connection(const std::string& title, bool connecting);
    void dismiss_interstitial();

    /// The latest switch or add request waiting for the next tick; empty when none is.
    helix::SharedCallback<void()> m_pending_request;
    /// A wizard cancel restored the previous printer and its restart waits for the tick.
    bool m_cancel_restart_pending = false;
    /// The tick that runs the waiting work is already posted.
    bool m_restart_tick_armed = false;

    /// Posts the one LVGL timer that runs the waiting work, unless it is already posted.
    /// Skipped when the owner's guard has expired by then.
    void arm_restart_tick();
    /// The tick: a waiting cancel's restart, then the waiting request.
    void run_waiting_restarts();
    /// Makes `request` the one that runs on the next tick, replacing any request waiting.
    void post_request(helix::SharedCallback<void()> request);

    /// During an UpdateQueue drain, posts `work` through post_request() and returns true.
    /// Otherwise this call is newer than anything waiting: it runs a waiting cancel's
    /// restart in place, drops a waiting request, and returns false for the caller to go on.
    bool deferred_out_of_drain(const char* entry, helix::SharedCallback<void()> work);

    /// The teardown hook. Entered inside an UpdateQueue drain it asserts in debug builds and
    /// logs an error in release builds, where the teardown still runs.
    void tear_down();

    /// The restart cancel_add_printer_wizard() leaves for the tick.
    void restart_onto_restored_printer();

    /// Saves the config, telling the user when it could not.
    bool save_or_report();

    /// The boot-crash bookkeeping (boot_crash_guard.h) of a user's move to `to_id`, unsaved:
    /// any move ends a connection hold, and a move to another printer starts a new crash run
    /// whose fallback is `from_id`. Re-picking the same printer keeps both as they are.
    /// Returns the record as it was, for restore_boot_crash_record() to undo the move.
    BootCrashRecord record_switch_away(const std::string& from_id, const std::string& to_id);

    /// Puts back a record that record_switch_away() returned, unsaved.
    void restore_boot_crash_record(const BootCrashRecord& record);
};

} // namespace helix
