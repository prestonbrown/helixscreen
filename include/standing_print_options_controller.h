// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Keeps the standing print options record (standing_print_options.h) current
// in Moonraker's database, and sends it at a job start some other client made
// when the HelixPrint plugin's print_hooks feature cannot.

#include "ui_observer_guard.h"
#include "ui_timer_guard.h"

#include "async_lifetime_guard.h"
#include "helix_plugin_features.h"
#include "history_job_events.h"

#include <functional>
#include <optional>
#include <string>

namespace helix {

class IMoonrakerAPI;

class StandingPrintOptionsController {
  public:
    explicit StandingPrintOptionsController(IMoonrakerAPI* api);
    ~StandingPrintOptionsController() = default;

    StandingPrintOptionsController(const StandingPrintOptionsController&) = delete;
    StandingPrintOptionsController& operator=(const StandingPrintOptionsController&) = delete;

    /// The options every backend asks for now, rendered.
    static std::string render_now();

    /// History added a job for @p filename. Main thread.
    void on_job_added(const std::string& filename);

  private:
    /// The plugin answered a status probe: a new connection, or its feature
    /// map changed.
    void on_plugin_probed();
    /// Re-render and write the record when it changed. Main thread.
    void refresh_record();
    void subscribe_history(IMoonrakerAPI& api);
    /// Read helix-screen/plugin_features into the plugin status, for the
    /// fallback switches and the fallback decision.
    void load_opt_ins(IMoonrakerAPI& api);
    /// A screen on the printer's host rewrites its presence record.
    void announce_presence();
    void send_options(const std::string& filename, const std::string& gcode);
    /// This instance's fallback_sender_role(): at once on the printer's host,
    /// else after reading the local screen's presence record.
    void resolve_sender_role(std::function<void(FallbackSenderRole)> then);
    /// Write the record if writes_standing_record() allows it.
    void write_record(FallbackSenderRole role);

    IMoonrakerAPI* api_;
    ObserverGuard probe_observer_;
    ObserverGuard backend_count_observer_;
    ObserverGuard data_revision_observer_;
    ObserverGuard unload_tools_observer_;

    /// Guards the database and notification replies, which arrive on the
    /// WebSocket thread.
    AsyncLifetimeGuard lifetime_;

    /// The record as last written this connection; nullopt until then.
    std::optional<std::string> written_;
    /// The rendering last weighed for writing this connection, written or not.
    std::optional<std::string> considered_;
    /// Rewrites the presence record while this screen is on the printer's host.
    ui::LvglTimerGuard presence_timer_;
    HistoryJobEvents history_events_;
};

} // namespace helix
