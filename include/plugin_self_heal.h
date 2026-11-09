// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "core/timer_guard.h"
#include "helix_plugin_installer.h"

#include <functional>

namespace helix {
class PrinterState;
struct PluginSelfHealTestAccess; // test-only friend (tests/test_helpers/)

/**
 * @brief Keeps an installed HelixPrint plugin loading the code this app ships
 *
 * Once per app start, after discovery has reported the plugin: on this host,
 * install.sh --relink re-points a link left naming an install directory that
 * moved or was replaced, and Moonraker restarts when it re-pointed the link or
 * the running plugin reports a version other than the one bundled here. A
 * restart waits for any job to let go and then for its completion flow
 * (decide_plugin_turn(), as the install prompt does), and goes through
 * install.sh --restart-moonraker: Moonraker's own server.restart keeps the
 * plugin module it already imported, so only a new process loads the new
 * code. One the script holds off for a print waits for the job again. A version pair or a
 * re-pointed link a finished restart did not fix is remembered in the config
 * (PluginSelfHealMemory) and only warned about on later starts. A plugin set
 * to `enabled: False` is left to the install prompt. A remote Moonraker is
 * only reported in the log. decide_plugin_self_heal() holds the rule; this
 * class gathers its facts and acts. Main thread only.
 */
class PluginSelfHeal {
  public:
    /// What is on screen now, for decide_plugin_turn().
    using ScreenProbe = std::function<PluginScreenState()>;

    PluginSelfHeal(PrinterState& printer_state, HelixPluginInstaller& installer,
                   ScreenProbe screen);

    /// Discovery has reported the plugin (installed or not): run the check,
    /// the first time in this process.
    void on_plugin_reported();

    /// The print lifecycle moved: a restart waiting for the job may go now.
    void on_job_state_changed();

    /// The check is running, so a plugin read as absent may yet turn out installed.
    [[nodiscard]] bool checking() const {
        return checking_;
    }

  private:
    friend struct PluginSelfHealTestAccess;

    void act(bool local, PluginLinkCheck link);
    /// A pending restart looks again: on a lifecycle change or a timer tick.
    void recheck_restart();
    void restart_moonraker();
    void on_restarted(PluginRestartOutcome outcome);
    /// PluginSelfHealMemory as the config holds it now.
    static PluginSelfHealMemory load_memory();
    /// Store what @p facts.memory becomes (remember_plugin_self_heal()).
    static void remember(const PluginSelfHealFacts& facts, bool restart_finished);

    PrinterState& printer_state_;
    HelixPluginInstaller& installer_;
    ScreenProbe screen_;
    /// Stop when no restart is pending, else the wait it is in.
    PluginTurnStep restart_wait_ = PluginTurnStep::Stop;
    /// What the pending or running restart is for; remembered once it finishes.
    PluginSelfHealFacts restart_facts_;
    ui::LvglTimerGuard restart_timer_;
    bool checking_ = false;
    AsyncLifetimeGuard lifetime_;

    // Set when the check starts; a failed relink waits for the next start.
    static bool ran_this_start_;
};

} // namespace helix
