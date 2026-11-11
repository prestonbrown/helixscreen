// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_timelapse_macro_card.h
 * @brief The card that gets moonraker-timelapse's Klipper macros loaded
 *
 * One piece the timelapse install wizard and the timelapse settings screen both
 * show (ui_xml/components/timelapse_macro_card.xml). refresh() reads Klipper's
 * objects through decide_timelapse_macro_setup():
 *
 * - NotYetKnown (Klipper's objects not in yet) or Loaded: no card.
 * - IncludeFromHelixMacros: helix_macros.cfg gains an include of the checkout's
 *   macro file by absolute path (MacroManager::include_timelapse_macros) as the
 *   screen's own step, then the
 *   card offers a Klipper restart. The restart is only ever the user's tap, and
 *   it is refused while a job holds the machine.
 * - ShowIncludeLine (no helper macros, or Moonraker not on this host so the
 *   checkout's path on the Klipper host is unknown), or a helix_macros.cfg that
 *   could not be written: the card shows the line to add to printer.cfg.
 *
 * Whenever the checkout is known on this host, refresh() also puts a checkout
 * left on a detached HEAD back on its branch (attach_checkout_to_branch), so
 * Moonraker's update manager tracks it again. That runs git on the card's own
 * worker, never on a remote screen, and only logs: the card shows nothing for it.
 * The card is process-wide, so the worker outlives the screen that started it.
 */

#include "async_lifetime_guard.h"
#include "subject_managed_panel.h"
#include "system/process_runner.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace helix {
class IMoonrakerAPI;
class MacroManager;
class PrinterDiscovery;
} // namespace helix

namespace helix::ui {

class TimelapseMacroCard {
  public:
    /// What the card shows; the timelapse_macro_card subject's values.
    enum class Card { None = 0, IncludeLine = 1, RestartKlipper = 2 };

    TimelapseMacroCard() = default;
    ~TimelapseMacroCard();
    TimelapseMacroCard(const TimelapseMacroCard&) = delete;
    TimelapseMacroCard& operator=(const TimelapseMacroCard&) = delete;

    /// Register the card's subjects; later calls do nothing.
    void init_subjects();
    /// Register the restart button's XML callback; later calls do nothing.
    void register_callbacks();

    /**
     * @brief Decide the card from @p discovery and act on it
     *
     * @param api Moonraker; null shows the include line when an include is needed
     * @param discovery Klipper's objects; must outlive the refresh's reply
     * @param moonraker_local Moonraker runs on this host, so the checkout's path is ours to know
     * @param on_shown Called on the main thread whenever a card appears, so the
     *                 screen can scroll it into view; may be empty
     */
    void refresh(IMoonrakerAPI* api, const PrinterDiscovery& discovery, bool moonraker_local,
                 std::function<void()> on_shown = {});

    /// Hide the card; replies of an earlier refresh are dropped.
    void clear();

    /// The Restart Klipper button: refused while a job holds the machine.
    void restart_klipper();

    [[nodiscard]] Card card() const;

  private:
    friend struct TimelapseMacroCardTestAccess;

    void show(Card card);
    /// Main thread: start the detached-HEAD repair of @p repo unless one is running.
    void repair_checkout(const std::string& repo);
    /// Main thread: join a repair worker that has finished.
    void join_finished_repair();

    SubjectManager subjects_;
    bool subjects_initialized_ = false;
    bool callbacks_registered_ = false;
    lv_subject_t card_subject_{};
    lv_subject_t include_line_subject_{};
    char include_line_buf_[64] = {};

    std::unique_ptr<MacroManager> macro_manager_;
    std::function<void()> on_shown_;
    /// Bumped by refresh() and clear(); a reply carrying an older one is dropped.
    unsigned generation_ = 0;

    std::thread repair_thread_;
    std::atomic<bool> repair_running_{false};
    /// Ends the repair's running git command, so the destructor's join is short.
    std::atomic<bool> repair_cancel_{false};
    ProcessRunner runner_override_;
    helix::AsyncLifetimeGuard lifetime_;
};

/// The process-wide card both timelapse screens bind to.
TimelapseMacroCard& timelapse_macro_card();

} // namespace helix::ui
