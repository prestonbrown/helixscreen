// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_presentation_gate.h"

#include "core/timer_guard.h"

#include <deque>
#include <functional>
#include <string>

namespace helix::plugin {

/// What a plugin asked to show.
enum class SurfaceKind { Dialog, Overlay, Confirm };

enum class Decision { Present, Wait };

/**
 * @brief Whether a plugin surface may appear now
 *
 * App covers always win. A plugin never waits on its own dialog (a Cancel
 * button asking "are you sure?" inside it), but waits while another plugin's
 * dialog shows, and a later request never jumps an earlier waiting one.
 *
 * @param app_blocked an app cover is up (ui::app_blocked)
 * @param showing_dialog_owner plugin whose dialog is on screen, "" for none
 * @param requester the plugin asking
 * @param earlier_waiting another request is already queued
 */
Decision decide(bool app_blocked, const std::string& showing_dialog_owner,
                const std::string& requester, bool earlier_waiting);

/**
 * @brief The queue every plugin surface request passes through
 *
 * Presents a request at once when decide() allows it, otherwise holds it,
 * first in first out, and retries while anything waits. Presenting wakes the
 * display first and re-checks the covers, since waking can engage the lock
 * screen. One per PluginHost. Main thread only.
 */
class PluginPresenter {
  public:
    /// Shows the surface; false when it could not be created.
    using PresentFn = std::function<bool()>;
    /// Reports the request withdrawn before it ever appeared.
    using CancelFn = std::function<void()>;
    using CoversFn = std::function<ui::AppCovers()>;
    using WakeFn = std::function<void()>;

    PluginPresenter();
    ~PluginPresenter();
    PluginPresenter(const PluginPresenter&) = delete;
    PluginPresenter& operator=(const PluginPresenter&) = delete;

    /// A ticket (> 0) for the request, or 0 when it was presented at once and
    /// could not be created.
    int request(const std::string& plugin_id, SurfaceKind kind, PresentFn present,
                CancelFn cancelled);
    /// Withdraws a queued request and runs its CancelFn. False when it is not
    /// queued (already presented, withdrawn or dropped).
    bool withdraw(int ticket);
    bool is_queued(int ticket) const;
    size_t queued_dialogs(const std::string& plugin_id) const;
    /// Forgets every queued request from @p plugin_id without running anything.
    void drop_plugin(const std::string& plugin_id);
    /// Presents from the head of the queue while decide() allows it.
    void pump();

    /// Where the covers are read; ui::current_app_covers by default.
    void set_covers_source(CoversFn covers);
    /// How the display is woken; DisplayManager::wake_display by default.
    void set_wake(WakeFn wake);

  private:
    struct Entry {
        int ticket;
        std::string plugin_id;
        SurfaceKind kind;
        PresentFn present;
        CancelFn cancelled;
    };

    bool may_present(const std::string& plugin_id, bool earlier_waiting) const;
    bool wake_and_recheck();
    void rearm();

    std::deque<Entry> queue_;
    int next_ticket_ = 1;
    bool presenting_ = false;
    CoversFn covers_;
    WakeFn wake_;
    ui::LvglTimerGuard retry_;
};

} // namespace helix::plugin
