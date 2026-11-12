// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "lua_bindings.h"
#include "lua_runtime.h"
#include "plugin_backend.h"
#include "plugin_consts.h"
#include "plugin_manifest.h"
#include "plugin_menu.h"
#include "plugin_overlay_host.h"
#include "plugin_permissions.h"
#include "plugin_presenter.h"
#include "plugin_surfaces.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace helix::plugin {

class PluginSettingsOverlay;

/// What all plugins together may allocate: min(MemTotal / 16, 64 MB).
size_t plugin_memory_budget(uint64_t mem_total_bytes);

enum class PluginStatus {
    Disabled,
    Loaded,
    NeedsApproval,
    Invalid,
    Incompatible,
    OverBudget,
    Faulted
};

const char* plugin_status_name(PluginStatus s);

struct PluginInfo {
    std::string dir_name;
    std::optional<Manifest> manifest; ///< unset when the manifest failed to parse
    PluginStatus status = PluginStatus::Disabled;
    std::string reason; ///< why, for every status but Loaded and Disabled
};

/// Owns every plugin: discovery, enable state, memory budget, load, unload and faults.
/// Main thread only.
class PluginHost {
  public:
    struct Deps {
        PluginBackend backend;
        std::function<json()> read_block;             ///< the current /plugins object
        std::function<void(const json&)> write_block; ///< replaces /plugins and saves
        std::string settings_path;                    ///< for storage file paths
        std::string helix_version;
        size_t memory_budget = 0;
        /// CPU and wall time each plugin entry may spend. Memory comes from each
        /// plugin's manifest.
        std::chrono::milliseconds entry_time_budget = LuaRuntime::Limits{}.time_budget;
        std::chrono::milliseconds entry_wall_ceiling = LuaRuntime::Limits{}.wall_ceiling;
    };

    explicit PluginHost(Deps deps);
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    /// Unloads everything, rescans `dir`, and loads every enabled plugin.
    void load_from(const std::string& dir);
    void unload_all();

    /// Reloads, adds or removes only the named plugins: each id with a manifest
    /// on disk is unloaded (when loaded), re-read and reconsidered; each id
    /// whose manifest is gone is unloaded and dropped. Ids not named keep
    /// their runtime and generation.
    void rescan(const std::vector<std::string>& ids);

    /// The host the app runs, or nullptr.
    static PluginHost* live();

    const std::vector<PluginInfo>& plugins() const {
        return plugins_;
    }

    /// Grants the manifest's current permissions and loads the plugin.
    bool enable(const std::string& id);
    void disable(const std::string& id);
    /// The permissions recorded for `id` in the enabled block (empty when not
    /// enabled) - the same read `consider` compares the manifest against.
    PermissionSet granted(const std::string& id) const;
    LuaRuntime* runtime(const std::string& id);

    /// The overlays every loaded plugin has on the navigation stack.
    const PluginOverlayHost& overlays() const {
        return overlays_;
    }

    /// Re-resolves every loaded plugin's breakpoint constants for @p display.
    /// Runs on its own after every theme layout refresh.
    void refresh_layout_constants(lv_display_t* display);

    /// The queue every plugin dialog, overlay and confirm passes through.
    PluginPresenter& presenter() {
        return presenter_;
    }

    /// Opens the plugin's settings: its settings_overlay component when the manifest
    /// names one, else the generated screen. False when the plugin is not loaded, or
    /// has neither settings nor an overlay.
    bool open_settings(const std::string& id);
    /// set_plugin_setting for the loaded plugin `id`: the same rule the Lua side
    /// writes through. False when it is not loaded or the value does not fit.
    bool set_setting(const std::string& id, const std::string& key, const json& value);
    /// The generated settings screen currently showing `id`, or null.
    PluginSettingsOverlay* settings_screen(const std::string& id);
    /// True when an open settings screen owns the row binding at `ud`.
    bool owns_row_binding(const void* ud);

    /// plugin_menu::any_declared over the loaded, unfaulted plugins.
    bool menu_declared(MenuKind kind, std::string_view plugin_id = {}) const;
    /// plugin_menu::resolve: runs each item's check, applies plugin_menu::row_cap.
    std::vector<plugin_menu::Entry> resolve_menu(MenuKind kind, const json& context,
                                                 std::string_view only_plugin = {});
    /// plugin_menu::activate: runs the item's handler when `e.gen` is still loaded.
    void activate_menu(const plugin_menu::Entry& e, const json& context);

    /// The body of the global `plugin_event` XML callback.
    void dispatch_event(std::string_view user_data);
    /// Finds the row whose root carries a known binding (walking up from the event
    /// target) and forwards to the screen that owns it.
    void handle_setting_row_event(lv_event_t* e, bool action);

  private:
    struct Loaded {
        json settings;
        std::unique_ptr<PluginContext> ctx;
        std::unique_ptr<LuaRuntime> rt; ///< destroyed before ctx
        /// The ui handle this plugin's bindings reach; binds `this` host, so it dies
        /// with the record, before rt's state can call it again.
        PluginUi ui;
        /// Each component this plugin registered, with the scope it created: an app that later
        /// registers the same name replaces the scope, and unload must then leave it alone.
        std::vector<std::pair<std::string, const void*>> components;
        /// Its <consts>, kept to re-resolve the breakpoint ladder on a resize.
        PluginConsts consts;
        /// The widget definitions this plugin registered, to unregister at unload.
        std::vector<std::string> widget_ids;
        /// The image icons this plugin registered, to withdraw at unload.
        std::vector<std::string> icons;
        size_t memory_bytes = 0;
        /// Bumped per load: a screen built against an older generation of the same
        /// plugin id must not route its rows to this instance.
        uint64_t gen = 0;
    };

    PluginInfo* find(const std::string& id);
    void consider(PluginInfo& info);
    bool load(PluginInfo& info);
    void unload(const std::string& id);
    void on_fault(const std::string& id, uint64_t gen, const std::string& reason);
    void save_settings(const std::string& id);
    json enabled_entry(const std::string& id) const;
    size_t memory_in_use() const;
    /// Pops every generated settings screen showing `id` through navigation; each
    /// screen's close callback erases it.
    void close_settings_screens(const std::string& id);

    Deps deps_;
    std::string dir_;
    std::vector<PluginInfo> plugins_;
    std::map<std::string, Loaded> loaded_;
    AsyncLifetimeGuard guard_;
    /// Set while load_from / unload_all loop over plugins: widget definitions still change,
    /// but each change only marks the set dirty; one notify runs after the loop.
    bool bulk_ = false;
    /// Widget definitions changed since the last notify_widget_defs_changed(). Survives a
    /// bulk unload_all so load_from's single end-of-scan notify covers plugins that went.
    bool widget_defs_dirty_ = false;
    int layout_hook_ = 0;
    PluginOverlayHost overlays_;
    PluginPresenter presenter_;
    /// Destroyed first: it closes dialogs that still point back at it.
    PluginSurfaces surfaces_{overlays_, presenter_};
    /// Generated settings screens currently open. More than one only while an
    /// earlier one is still sliding out.
    std::vector<std::unique_ptr<PluginSettingsOverlay>> settings_screens_;
    uint64_t next_load_gen_ = 1;
};

/// The candidates the host can load after a rescan: present in `infos`, with a
/// readable manifest, in a state the user could enable. A synced folder with
/// no manifest, or one the host rejected, does not count as a new plugin.
std::vector<std::string> loadable_plugin_ids(const std::vector<std::string>& candidates,
                                             const std::vector<PluginInfo>& infos);

/// Registers the `plugin_event` XML callback once per process. It forwards to the live host.
void register_plugin_event_callback();

} // namespace helix::plugin
