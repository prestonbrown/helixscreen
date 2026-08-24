// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file plugin_manager.h
 * @brief Plugin lifecycle management
 *
 * Manages discovery, loading, and unloading of plugins. Handles:
 * - Scanning plugins directory for manifest.json files
 * - Parsing manifests and building dependency graph
 * - Topological sort for load order
 * - Dynamic loading via dlopen()/dlsym()
 * - Graceful error handling and reporting
 *
 * @pattern Manager/Orchestrator
 * @threading Main thread only (LVGL constraints)
 *
 * @see plugin_api.h for plugin interface
 */

#pragma once

#include "plugin_api.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace helix::plugin {

// ============================================================================
// Plugin Metadata Types
// ============================================================================

/**
 * @brief Plugin UI configuration from manifest
 */
struct PluginUIConfig {
    bool settings_page = false;                ///< Plugin has settings page
    bool navbar_panel = false;                 ///< Plugin wants navbar slot (rare)
    std::vector<std::string> injection_points; ///< UI injection points used
};

/**
 * @brief Plugin manifest data
 *
 * Parsed from manifest.json in plugin directory.
 */
struct PluginManifest {
    std::string id;                        ///< Unique plugin identifier
    std::string name;                      ///< Human-readable name
    std::string version;                   ///< Semantic version (e.g., "1.0.0")
    std::string helix_version;             ///< Required HelixScreen version (e.g., ">=2.0.0")
    std::string author;                    ///< Plugin author
    std::string description;               ///< Plugin description
    std::vector<std::string> dependencies; ///< Other plugin IDs required
    std::string entry_point;               ///< Entry function name (default: "helix_plugin_init")
    PluginUIConfig ui;                     ///< UI configuration
};

/**
 * @brief Plugin runtime information
 */
struct PluginInfo {
    PluginManifest manifest;  ///< Parsed manifest
    std::string directory;    ///< Absolute path to plugin directory
    std::string library_path; ///< Path to .so/.dylib file
    bool enabled = false;     ///< Currently enabled in config
    bool loaded = false;      ///< Successfully loaded
};

/**
 * @brief Plugin load error information
 */
struct PluginError {
    std::string plugin_id; ///< Plugin that failed
    std::string message;   ///< Human-readable error message

    enum class Type {
        MANIFEST_PARSE_ERROR,   ///< Failed to parse manifest.json
        MANIFEST_MISSING_FIELD, ///< Required field missing in manifest
        MISSING_DEPENDENCY,     ///< Required dependency not available
        DEPENDENCY_CYCLE,       ///< Circular dependency detected
        LIBRARY_NOT_FOUND,      ///< .so/.dylib file not found
        LOAD_FAILED,            ///< dlopen() failed
        SYMBOL_NOT_FOUND,       ///< Entry point not found
        INIT_FAILED,            ///< Plugin init returned false
        VERSION_MISMATCH,       ///< API version incompatible
    } type;
};

// ============================================================================
// Plugin Manager
// ============================================================================

/**
 * @brief Plugin lifecycle manager
 *
 * Manages discovery, loading, and unloading of plugins. Ensures plugins
 * are loaded in dependency order and provides graceful error handling.
 *
 * Typical usage:
 * @code
 * PluginManager mgr;
 * mgr.set_core_services(api, client, state, config);
 * mgr.discover_plugins("/path/to/plugins");
 * mgr.load_all();
 *
 * // After Moonraker connects:
 * mgr.on_moonraker_connected();
 *
 * // On shutdown:
 * mgr.unload_all();
 * @endcode
 *
 * Thread safety: All methods must be called from main thread.
 */
class PluginManager {
  public:
    PluginManager();
    ~PluginManager();

    // Non-copyable (owns plugin state)
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Set core service references for plugin API
     *
     * Must be called before load_all().
     *
     * @param api IMoonrakerAPI instance (may be nullptr initially)
     * @param client MoonrakerClient instance (may be nullptr initially)
     * @param state PrinterState reference
     * @param config Config instance
     */
    void set_core_services(IMoonrakerAPI* api, IMoonrakerClient* client, PrinterState& state,
                           Config* config);

    /**
     * @brief Set list of enabled plugin IDs
     *
     * Plugins not in this list will be discovered but not loaded.
     * Plugins must be explicitly enabled - if empty, no plugins load.
     *
     * @param enabled_ids Plugin IDs to enable
     */
    void set_enabled_plugins(const std::vector<std::string>& enabled_ids);

    // ========================================================================
    // Plugin Lifecycle
    // ========================================================================

    /**
     * @brief Discover plugins in directory
     *
     * Scans the specified directory for subdirectories containing manifest.json.
     * Parses manifests and populates the discovered plugins list.
     *
     * @param plugins_dir Path to plugins directory
     * @return true if discovery completed (even if some plugins failed to parse)
     */
    bool discover_plugins(const std::string& plugins_dir);

    /**
     * @brief Load all enabled plugins
     *
     * Loads plugins in dependency order. Plugins with missing dependencies
     * are skipped and added to the errors list.
     *
     * @return true if all enabled plugins loaded successfully
     */
    bool load_all();

    /**
     * @brief Load a specific plugin by ID
     *
     * @param plugin_id Plugin to load
     * @return true if plugin loaded successfully
     */
    bool load_plugin(const std::string& plugin_id);

    /**
     * @brief Unload all loaded plugins
     *
     * Unloads in reverse dependency order. Safe to call multiple times.
     */
    void unload_all();

    /**
     * @brief Unload a specific plugin
     *
     * @param plugin_id Plugin to unload
     * @return true if plugin was loaded and unloaded successfully
     */
    bool unload_plugin(const std::string& plugin_id);

    /**
     * @brief Disable a plugin in config
     *
     * Removes the plugin from the enabled list and persists to config.
     * For failed plugins that never loaded, this is just a config update.
     * No restart required.
     *
     * @param plugin_id Plugin ID to disable
     * @return true if plugin was found and disabled
     */
    bool disable_plugin(const std::string& plugin_id);

    // ========================================================================
    // Moonraker Connection Events
    // ========================================================================

    /**
     * @brief Notify manager that Moonraker is connected
     *
     * Updates plugin API with new Moonraker references and applies
     * any deferred subscriptions.
     */
    void on_moonraker_connected();

    /**
     * @brief Notify manager that Moonraker disconnected
     */
    void on_moonraker_disconnected();

    /**
     * @brief Update Moonraker service references
     *
     * Call after Moonraker reconnects to update all plugin APIs.
     *
     * @param api New IMoonrakerAPI pointer
     * @param client New MoonrakerClient pointer
     */
    void update_moonraker_services(IMoonrakerAPI* api, IMoonrakerClient* client);

    // ========================================================================
    // Status Queries
    // ========================================================================

    /**
     * @brief Get list of all discovered plugins
     */
    std::vector<PluginInfo> get_discovered_plugins() const;

    /**
     * @brief Get list of successfully loaded plugins
     */
    std::vector<PluginInfo> get_loaded_plugins() const;

    /**
     * @brief Get list of load errors
     */
    std::vector<PluginError> get_load_errors() const;

    /**
     * @brief Check if a plugin is loaded
     *
     * @param plugin_id Plugin to check
     * @return true if plugin is currently loaded
     */
    bool is_loaded(const std::string& plugin_id) const;

    /**
     * @brief Get plugin info by ID
     *
     * @param plugin_id Plugin to look up
     * @return Pointer to PluginInfo, or nullptr if not found
     */
    const PluginInfo* get_plugin(const std::string& plugin_id) const;

  private:
    // ========================================================================
    // Internal Types
    // ========================================================================

    /**
     * @brief Loaded plugin state
     *
     * Ownership: The `handle` pointer is a dlopen() handle that MUST be closed
     * via dlclose() when the plugin is unloaded. This is handled by unload_plugin()
     * and unload_all(). Do not copy LoadedPlugin or let handles leak.
     */
    struct LoadedPlugin {
        PluginInfo info;
        void* handle = nullptr; ///< dlopen() handle - owned, closed by unload_plugin()
        PluginInitFunc init_func = nullptr;
        PluginDeinitFunc deinit_func = nullptr;
        std::unique_ptr<PluginAPI> api; ///< Plugin's API instance
    };

    // ========================================================================
    // Internal Methods
    // ========================================================================

    /**
     * @brief Parse a manifest.json file
     *
     * @param manifest_path Path to manifest.json
     * @param[out] manifest Parsed manifest data
     * @param[out] error_msg Error message if parsing fails
     * @return true if parsing succeeded
     */
    bool parse_manifest(const std::string& manifest_path, PluginManifest& manifest,
                        std::string& error_msg);

    /**
     * @brief Validate manifest has required fields
     *
     * @param manifest Manifest to validate
     * @param[out] error_msg Error message if validation fails
     * @return true if manifest is valid
     */
    bool validate_manifest(const PluginManifest& manifest, std::string& error_msg);

    /**
     * @brief Build dependency-sorted load order
     *
     * Performs topological sort to determine safe load order.
     *
     * @param[out] load_order Plugin IDs in load order
     * @return true if sort succeeded (no cycles)
     */
    bool build_load_order(std::vector<std::string>& load_order);

    /**
     * @brief Load a single plugin (internal)
     *
     * @param plugin_id Plugin to load
     * @return true if loading succeeded
     */
    bool load_plugin_internal(const std::string& plugin_id);

    /**
     * @brief Find plugin library file
     *
     * Searches for .so (Linux) or .dylib (macOS) in plugin directory.
     *
     * @param plugin_dir Plugin directory
     * @param plugin_id Plugin ID (for naming convention)
     * @return Path to library, or empty string if not found
     */
    std::string find_library(const std::string& plugin_dir, const std::string& plugin_id);

    /**
     * @brief Add error to errors list
     */
    void add_error(const std::string& plugin_id, PluginError::Type type, const std::string& msg);

    // ========================================================================
    // State
    // ========================================================================

    // Core services (set via set_core_services())
    IMoonrakerAPI* moonraker_api_ = nullptr;
    IMoonrakerClient* moonraker_client_ = nullptr;
    PrinterState* printer_state_ = nullptr;
    Config* config_ = nullptr;

    // Discovered plugins (keyed by plugin ID)
    std::unordered_map<std::string, PluginInfo> discovered_;

    // Loaded plugins (keyed by plugin ID)
    std::unordered_map<std::string, LoadedPlugin> loaded_;

    // Enabled plugin IDs (empty = all enabled)
    std::vector<std::string> enabled_ids_;

    // Load errors
    std::vector<PluginError> errors_;

    // Plugin load order (dependency-sorted)
    std::vector<std::string> load_order_;

    // Plugins directory
    std::string plugins_dir_;
};

} // namespace helix::plugin
