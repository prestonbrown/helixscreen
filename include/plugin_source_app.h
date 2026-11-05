// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_timer_guard.h"

#include "i_moonraker_api.h"
#include "plugin_host.h"
#include "plugin_source.h"

#include <functional>
#include <memory>
#include <string>

namespace helix::plugin {

/// The plugin root inside Moonraker's config root.
constexpr const char* kPluginRootPath = "helixscreen/plugins/";

/// SourceDeps over the app's Moonraker API: `list` walks kPluginRootPath one directory at
/// a time with server.files.get_directory, because server.files.list walks the whole config
/// root with Moonraker's event loop blocked and that root can hold the mod and this app's
/// install. A missing folder lists as empty. `download` is download_file_partial capped at
/// `max_bytes` with the body written to the destination. `api` must outlive the deps' callbacks;
/// the caller guards them.
SourceDeps make_moonraker_source_deps(IMoonrakerAPI* api);

/// True when a notify_filelist_changed message touches config/helixscreen/plugins/: its
/// item, or a move/copy source_item, sits in the config root under kPluginRootPath. Pure.
bool is_plugin_filelist_change(const json& msg);

/// get_helix_cache_dir("plugins") + "/<printer_id>" ("default" when the id is empty),
/// created so a boot-time load_from finds a directory. One cache per printer: a plugin
/// installed on one printer must not appear for another.
std::string plugin_cache_dir_for(const std::string& printer_id);

/// Owns a PluginSource and the host it feeds. sync_now() runs one sync and rescans the
/// changed and removed ids; request_sync() coalesces a burst of requests into one sync
/// `debounce_ms` after the last one. Main thread only.
class PluginSyncDriver {
  public:
    PluginSyncDriver(PluginHost& host, SourceDeps deps, std::string cache_dir,
                     uint32_t debounce_ms = 1500);
    /// Cancels the debounce timer; a sync still in flight is dropped by the source's
    /// lifetime guard, so no reply ever reaches a dead driver.
    ~PluginSyncDriver();
    PluginSyncDriver(const PluginSyncDriver&) = delete;
    PluginSyncDriver& operator=(const PluginSyncDriver&) = delete;

    /// Runs one sync now, superseding any pending debounced request.
    void sync_now();
    /// Restarts the debounce: one sync, `debounce_ms` after the last request.
    void request_sync();

    /// Called after every completed sync with its result, after the host has been
    /// rescanned (tests, and the new-plugin toast).
    std::function<void(const SyncResult&)> on_synced;

  private:
    static void on_debounce(lv_timer_t* timer);

    PluginHost& host_;
    std::unique_ptr<PluginSource> source_;
    lv_timer_t* timer_ =
        nullptr; ///< re-armed one-shot; spent and paused unless a debounced sync is pending
    uint32_t debounce_ms_;
};

} // namespace helix::plugin
