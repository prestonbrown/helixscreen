// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file plugin_api.h
 * @brief Plugin API - the interface exposed to plugins
 *
 * This header defines the API that plugins receive during initialization.
 * Plugins use this API to:
 * - Access core services (Moonraker, PrinterState, Config)
 * - Subscribe to events
 * - Register services for plugin-to-plugin communication
 * - Register reactive subjects for UI binding
 * - Log messages
 *
 * @pattern Facade - provides simplified access to complex subsystems
 * @threading Most methods must be called from main thread only
 *
 * @see plugin_manager.h for plugin lifecycle
 * @see plugin_events.h for event system
 * @see plugin_registry.h for service registration
 */

#pragma once

#include "injection_point_manager.h"
#include "plugin_events.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations - plugins don't need full definitions
class IMoonrakerAPI;
namespace helix {
class IMoonrakerClient;
}
namespace helix {
class PrinterState;
}
namespace helix {
class Config;
}

// LVGL types - use typedef form to match LVGL's declaration
typedef struct _lv_subject_t lv_subject_t;

namespace helix::plugin {

using json = nlohmann::json;

// ============================================================================
// Moonraker Subscription Types
// ============================================================================

/**
 * @brief Callback for Moonraker status updates
 *
 * Called when subscribed Moonraker objects change.
 * The JSON contains only the changed fields.
 */
using MoonrakerCallback = std::function<void(const json& status_update)>;

/**
 * @brief Handle for Moonraker subscription
 */
using MoonrakerSubscriptionId = uint64_t;

/// Invalid Moonraker subscription ID
constexpr MoonrakerSubscriptionId INVALID_MOONRAKER_SUBSCRIPTION = 0;

// ============================================================================
// Plugin API
// ============================================================================

/**
 * @brief Plugin API - the interface exposed to plugins
 *
 * Plugins receive a pointer to this struct during initialization.
 * The PluginAPI instance is owned by PluginManager and remains valid
 * for the plugin's lifetime.
 *
 * Thread safety:
 * - Core service pointers are set once at init and never change
 * - Event/service registration is thread-safe
 * - Moonraker subscriptions must be called from main thread
 * - Logging is thread-safe
 */
class PluginAPI {
  public:
    /**
     * @brief Construct PluginAPI with core service references
     *
     * @param api IMoonrakerAPI instance (may be nullptr if not connected)
     * @param client MoonrakerClient instance (may be nullptr if not connected)
     * @param state PrinterState reference
     * @param config Config instance
     * @param plugin_id ID of the plugin this API belongs to
     */
    PluginAPI(IMoonrakerAPI* api, IMoonrakerClient* client, PrinterState& state, Config* config,
              const std::string& plugin_id);

    ~PluginAPI();

    // Non-copyable (tied to specific plugin instance)
    PluginAPI(const PluginAPI&) = delete;
    PluginAPI& operator=(const PluginAPI&) = delete;

    // ========================================================================
    // Core Service Access
    // ========================================================================

    /**
     * @brief Get IMoonrakerAPI for high-level printer operations
     *
     * May return nullptr if Moonraker is not connected. Always check before use.
     */
    IMoonrakerAPI* moonraker_api() const {
        return moonraker_api_;
    }

    /**
     * @brief Get MoonrakerClient for low-level WebSocket operations
     *
     * May return nullptr if Moonraker is not connected. Always check before use.
     */
    IMoonrakerClient* moonraker_client() const {
        return moonraker_client_;
    }

    /**
     * @brief Get PrinterState for reactive printer state access
     *
     * Always valid - PrinterState is a singleton.
     */
    PrinterState& printer_state() const {
        return printer_state_;
    }

    /**
     * @brief Get Config for reading/writing configuration
     *
     * May return nullptr if config is not initialized.
     */
    Config* config() const {
        return config_;
    }

    /**
     * @brief Get this plugin's ID
     */
    const std::string& plugin_id() const {
        return plugin_id_;
    }

    // ========================================================================
    // Event System
    // ========================================================================

    /**
     * @brief Subscribe to an application event
     *
     * Events are fire-and-forget notifications. Callbacks are invoked on the
     * main thread. See plugin_events.h for available event names.
     *
     * @param event_name Event to subscribe to (events::* constant)
     * @param callback Callback to invoke when event occurs
     * @return Subscription ID for later unsubscription
     */
    EventSubscriptionId on_event(const std::string& event_name, EventCallback callback);

    /**
     * @brief Unsubscribe from an event
     *
     * @param id Subscription ID from on_event()
     * @return true if subscription was found and removed
     */
    bool off_event(EventSubscriptionId id);

    // ========================================================================
    // Moonraker Subscription (Managed)
    // ========================================================================

    /**
     * @brief Subscribe to Moonraker object updates
     *
     * Unlike direct MoonrakerClient subscriptions, this method handles
     * connection timing automatically:
     * - If connected: subscribes immediately
     * - If not connected: queues subscription for when connection is established
     *
     * Subscriptions are automatically cleaned up when the plugin unloads.
     *
     * @param objects Klipper objects to subscribe to (e.g., {"extruder", "heater_bed"})
     * @param callback Callback for status updates
     * @return Subscription ID for later unsubscription
     */
    MoonrakerSubscriptionId subscribe_moonraker(const std::vector<std::string>& objects,
                                                MoonrakerCallback callback);

    /**
     * @brief Unsubscribe from Moonraker updates
     *
     * @param id Subscription ID from subscribe_moonraker()
     * @return true if subscription was found and removed
     */
    bool unsubscribe_moonraker(MoonrakerSubscriptionId id);

    // ========================================================================
    // Subject Registration (for Reactive UI)
    // ========================================================================

    /**
     * @brief Register an LVGL subject for reactive UI binding
     *
     * Registered subjects can be referenced in XML layouts using bind_text
     * or other reactive bindings. Subject name should be prefixed with
     * plugin ID to avoid collisions (e.g., "led_effects.current_mode").
     *
     * @param name Subject name (convention: "plugin_id.subject_name")
     * @param subject Pointer to lv_subject_t (caller owns memory)
     */
    void register_subject(const std::string& name, lv_subject_t* subject);

    /**
     * @brief Unregister a previously registered subject
     *
     * @param name Subject name
     * @return true if subject was found and removed
     */
    bool unregister_subject(const std::string& name);

    // ========================================================================
    // Service Registration (Plugin-to-Plugin)
    // ========================================================================

    /**
     * @brief Register a service for other plugins to use
     *
     * Services are identified by name and can be retrieved by any plugin.
     * Convention: use "plugin_id.service_name" format.
     *
     * @param name Service identifier
     * @param service Pointer to service instance (caller owns memory)
     */
    void register_service(const std::string& name, void* service);

    /**
     * @brief Unregister a service
     *
     * @param name Service identifier
     * @return true if service was found and removed
     */
    bool unregister_service(const std::string& name);

    /**
     * @brief Get a service registered by another plugin
     *
     * @param name Service identifier
     * @return Service pointer, or nullptr if not found
     */
    void* get_service(const std::string& name) const;

    /**
     * @brief Get a service with type casting
     *
     * @tparam T Service type
     * @param name Service identifier
     * @return Typed service pointer, or nullptr if not found
     */
    template <typename T> T* get_service(const std::string& name) const {
        return static_cast<T*>(get_service(name));
    }

    // ========================================================================
    // Logging
    // ========================================================================

    /**
     * @brief Log an info message
     *
     * Messages are prefixed with plugin ID automatically.
     * Thread-safe.
     *
     * @param message Log message
     */
    void log_info(const std::string& message) const;

    /**
     * @brief Log a warning message
     *
     * @param message Log message
     */
    void log_warn(const std::string& message) const;

    /**
     * @brief Log an error message
     *
     * @param message Log message
     */
    void log_error(const std::string& message) const;

    /**
     * @brief Log a debug message
     *
     * Only visible with -vv or higher verbosity.
     *
     * @param message Log message
     */
    void log_debug(const std::string& message) const;

    // ========================================================================
    // UI Injection
    // ========================================================================

    /**
     * @brief Inject a widget into a named injection point
     *
     * Creates an instance of the XML component and adds it to the injection
     * point container. The widget is tracked and will be automatically removed
     * when the plugin unloads.
     *
     * @param point_id Injection point identifier (e.g., "panel_widget_area")
     * @param xml_component Name of registered XML component to instantiate
     * @param callbacks Optional lifecycle callbacks for widget creation/destruction
     * @return true if injection succeeded, false if point not found or creation failed
     *
     * @note The injection point must be registered by a panel before injection can occur.
     *       If the panel hasn't loaded yet, injection will fail.
     */
    bool inject_widget(const std::string& point_id, const std::string& xml_component,
                       const WidgetCallbacks& callbacks = {});

    /**
     * @brief Register an XML component from the plugin's directory
     *
     * Registers an XML component file so it can be used with inject_widget().
     * The file is loaded from the plugin's directory.
     *
     * @param filename XML file name (e.g., "my_widget.xml")
     * @return true if registration succeeded
     *
     * @note The plugin directory is passed to helix_plugin_init() and should
     *       be stored by the plugin for use with this method.
     */
    bool register_xml_component(const std::string& plugin_dir, const std::string& filename);

    /**
     * @brief Check if an injection point is available
     *
     * @param point_id Injection point identifier
     * @return true if the point is registered and ready for injection
     */
    bool has_injection_point(const std::string& point_id) const;

    // ========================================================================
    // Internal (called by PluginManager)
    // ========================================================================

    /**
     * @brief Update Moonraker pointers after connection
     *
     * Called by PluginManager when Moonraker connects. Applies any
     * deferred subscriptions.
     *
     * @param api New IMoonrakerAPI pointer
     * @param client New MoonrakerClient pointer
     */
    void set_moonraker(IMoonrakerAPI* api, IMoonrakerClient* client);

    /**
     * @brief Apply deferred Moonraker subscriptions
     *
     * Called by PluginManager after Moonraker connects to apply any
     * subscriptions that were queued while disconnected.
     */
    void apply_deferred_subscriptions();

    /**
     * @brief Cleanup all subscriptions and registrations
     *
     * Called by PluginManager during plugin unload.
     */
    void cleanup();

  private:
    // Core services
    IMoonrakerAPI* moonraker_api_;
    IMoonrakerClient* moonraker_client_;
    PrinterState& printer_state_;
    Config* config_;
    std::string plugin_id_;

    // Event subscriptions (for cleanup on unload)
    std::vector<EventSubscriptionId> event_subscriptions_;

    // Moonraker subscriptions
    struct DeferredSubscription {
        MoonrakerSubscriptionId id;
        std::vector<std::string> objects;
        MoonrakerCallback callback;
    };
    std::vector<DeferredSubscription> deferred_subscriptions_;
    std::vector<MoonrakerSubscriptionId> active_moonraker_subscriptions_;
    MoonrakerSubscriptionId next_moonraker_sub_id_ = 1;

    // Mapping from our plugin subscription IDs to MoonrakerClient's subscription IDs
    // This allows proper cleanup when a plugin unloads
    std::unordered_map<MoonrakerSubscriptionId, uint64_t> moonraker_id_map_;

    // Registered subjects (for cleanup)
    std::vector<std::string> registered_subjects_;

    // Registered services (for cleanup)
    std::vector<std::string> registered_services_;

    // Alive flag for use-after-free prevention in Moonraker callbacks
    // When plugin unloads, this becomes false and callbacks skip execution
    std::shared_ptr<bool> alive_flag_;

    mutable std::mutex mutex_;
};

// ============================================================================
// Plugin Entry Point Contract
// ============================================================================

/**
 * @brief Plugin initialization function signature
 *
 * Every plugin must export a function matching this signature with the name
 * "helix_plugin_init". Called during plugin loading.
 *
 * @param api PluginAPI instance for this plugin
 * @param plugin_dir Path to plugin's directory (for loading assets)
 * @return true if initialization succeeded, false to abort loading
 */
using PluginInitFunc = bool (*)(PluginAPI* api, const char* plugin_dir);

/**
 * @brief Plugin deinitialization function signature
 *
 * Every plugin must export a function matching this signature with the name
 * "helix_plugin_deinit". Called during plugin unloading.
 */
using PluginDeinitFunc = void (*)();

/**
 * @brief Plugin API version function signature
 *
 * Optional export for version compatibility checking.
 * Returns a version string like "1.0".
 */
using PluginApiVersionFunc = const char* (*)();

/// Current plugin API version
constexpr const char* PLUGIN_API_VERSION = "1.0";

} // namespace helix::plugin
