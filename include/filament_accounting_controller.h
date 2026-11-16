// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Keeps the filament_spools map (filament_accounting.h) current in Moonraker's
// database for the HelixPrint plugin, and switches the Spoolman active spool at
// a tool change itself when the plugin cannot. Tools a filament backend feeds
// are switched by AmsState::sync_current_loaded_from_backend() on the lane
// load; this covers the tools that print from their own extruder with a spool
// assigned to the tool.

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "filament_accounting.h"

#include <optional>
#include <string>
#include <vector>

namespace helix {

struct ToolInfo;
class AmsBackend;

/// Who switches the active spool on this printer now: active_spool_owner()
/// over what this app knows (Spoolman, @p backend_manages_active_spool for
/// the backend in question, firmware that switches it, T<n> macros that do).
/// Every HelixScreen path that sets the active spool asks this, and acts only
/// on ActiveSpoolOwner::Helix. Main thread.
ActiveSpoolOwner active_spool_owner_now(bool spoolman, bool backend_manages_active_spool);

/// Whether HelixScreen itself sets the active spool for a lane of @p backend
/// (nullptr: the printer has no filament system): active_spool_owner_now()
/// answers Helix. Asked with a linked spool in hand, so Spoolman is in use.
/// Every path that sets it from a lane or spool edit asks this.
bool helix_sets_active_spool(const AmsBackend* backend);

/// The Spoolman spool HelixScreen makes active when lane @p slot of @p backend
/// is the loaded one: the lane's linked spool when helix_sets_active_spool(),
/// else 0 (no lane, no linked spool, or something else switches it).
int spool_to_activate(const AmsBackend* backend, int slot);

/// Whether HelixScreen sets the active spool for the user's explicit link of
/// the loaded lane of @p backend, or of the external spool (@p
/// external_spool: no lane of the backend's carries it):
/// app_sets_spool_on_edit() over active_spool_owner_now().
bool helix_sets_active_spool_on_edit(const AmsBackend* backend, bool external_spool = false);

class FilamentAccountingController {
  public:
    FilamentAccountingController();
    ~FilamentAccountingController() = default;

    FilamentAccountingController(const FilamentAccountingController&) = delete;
    FilamentAccountingController& operator=(const FilamentAccountingController&) = delete;

    /// The feed signals the printer's tools and backends publish now.
    static std::vector<FeedSignal> signals_now();
    /// Whether a filament backend switches the active spool itself.
    static bool backends_manage_active_spool();
    /// Whether the printer's T<n> macros switch the active spool.
    static bool macros_set_active_spool();
    /// What tool @p tool_index feeds from: the spool of the lane that feeds it.
    static FeedEntry feed_for_tool(int tool_index);

    /// The active tool is now @p tool_index. Main thread.
    void on_active_tool_changed(int tool_index);

  private:
    void on_plugin_probed();
    void on_connection_changed(int state);
    /// Rebuild the map and write it when it changed. Main thread.
    void refresh_record();
    void write_record(FallbackSenderRole role);
    /// Switch Moonraker's active spool for @p feed after reading the current one.
    void switch_active_spool(const FeedEntry& feed, const std::vector<int>& mapped);

    ObserverGuard probe_observer_;
    ObserverGuard installed_observer_;
    ObserverGuard connection_observer_;
    ObserverGuard backend_count_observer_;
    ObserverGuard data_revision_observer_;
    ObserverGuard tools_version_observer_;
    ObserverGuard active_tool_observer_;

    /// Guards the database and Spoolman replies, which arrive on the
    /// WebSocket thread.
    AsyncLifetimeGuard lifetime_;

    /// The record as last written this connection; nullopt until then.
    std::optional<std::string> written_;
    /// The rendering last weighed for writing this connection, written or not.
    std::optional<std::string> considered_;
    /// Tool changes count only once this connection's discovery has read the
    /// printer: a tool carried over a reconnect is no change.
    bool armed_ = false;
    /// A sender-role read for the map is in flight.
    bool role_pending_ = false;
    int baseline_tool_ = -1;
};

} // namespace helix
