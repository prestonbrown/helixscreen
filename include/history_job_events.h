// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// History's job notifications (notify_history_changed), read once for every
// owner that acts on a job starting or ending.

#include "async_lifetime_guard.h"
#include "json_fwd.h"

#include <functional>
#include <optional>
#include <string>

namespace helix {

class IMoonrakerAPI;
class IMoonrakerClient;

/// A job history added or finished.
struct HistoryJobEvent {
    std::string action; ///< "added" or "finished"
    std::string job_id;
    std::string filename;
    std::string status; ///< history's status; "in_progress" for an added job
    double start_time = 0.0;
};

/// One notify_history_changed message as a job event; nullopt for any other
/// action, a message without a job object, or a job without an id.
std::optional<HistoryJobEvent> parse_history_job_event(const nlohmann::json& msg);

/// One owner's subscription to history's job events.
class HistoryJobEvents {
  public:
    /// Hear job events on @p api's current client under @p handler_name:
    /// parsed on the WebSocket thread, handed to @p on_event on the main
    /// thread while @p token lives. Registers once per client, so calling it
    /// on every reconnect is safe. Main thread.
    void subscribe(IMoonrakerAPI& api, const char* handler_name, LifetimeToken token,
                   std::function<void(const HistoryJobEvent&)> on_event);

  private:
    /// The client the notification is registered on.
    const IMoonrakerClient* client_ = nullptr;
};

} // namespace helix
