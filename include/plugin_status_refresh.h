// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Keeps the HelixPrint plugin's feature map current between connections.
// Discovery reads server.helix.status once; a feature's availability can
// change after that (a camera the plugin cannot reach, then can), and every
// plugin-or-fallback choice reads the map. So the status is read again when a
// job starts and whenever the plugin announces a change
// (notify_helix_features_changed).

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "history_job_events.h"

namespace helix {

class IMoonrakerClient;

class PluginStatusRefresh {
  public:
    PluginStatusRefresh();
    ~PluginStatusRefresh() = default;

    PluginStatusRefresh(const PluginStatusRefresh&) = delete;
    PluginStatusRefresh& operator=(const PluginStatusRefresh&) = delete;

    /// Read the plugin's status again, when a plugin is installed; an
    /// unchanged map touches nothing. Main thread.
    void refresh();

  private:
    void on_probed();

    ObserverGuard probe_observer_;
    HistoryJobEvents history_events_;
    /// The client the change notification is registered on.
    const IMoonrakerClient* notified_client_ = nullptr;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
