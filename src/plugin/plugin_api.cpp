// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin_api.h"

#include "ui_update_queue.h"

#include "i_moonraker_client.h"
#include "lvgl.h"
#include "plugin_registry.h"
#include "printer_state.h"
#include "spdlog/spdlog.h"

namespace helix::plugin {

// ============================================================================
// PluginAPI Implementation
// ============================================================================

PluginAPI::PluginAPI(IMoonrakerAPI* api, IMoonrakerClient* client, PrinterState& state,
                     Config* config, const std::string& plugin_id)
    : moonraker_api_(api), moonraker_client_(client), printer_state_(state), config_(config),
      plugin_id_(plugin_id), alive_flag_(std::make_shared<bool>(true)) {
    spdlog::debug("[plugin:{}] API instance created", plugin_id_);
}

PluginAPI::~PluginAPI() {
    cleanup();
    spdlog::debug("[plugin:{}] API instance destroyed", plugin_id_);
}

// ============================================================================
// Event System
// ============================================================================

EventSubscriptionId PluginAPI::on_event(const std::string& event_name, EventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);

    EventSubscriptionId id = EventDispatcher::instance().subscribe(event_name, std::move(callback));
    event_subscriptions_.push_back(id);

    spdlog::debug("[plugin:{}] Subscribed to event: {}", plugin_id_, event_name);
    return id;
}

bool PluginAPI::off_event(EventSubscriptionId id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Remove from our tracking list
    auto it = std::find(event_subscriptions_.begin(), event_subscriptions_.end(), id);
    if (it != event_subscriptions_.end()) {
        event_subscriptions_.erase(it);
    }

    return EventDispatcher::instance().unsubscribe(id);
}

// ============================================================================
// Moonraker Subscription
// ============================================================================

MoonrakerSubscriptionId PluginAPI::subscribe_moonraker(const std::vector<std::string>& objects,
                                                       MoonrakerCallback callback) {
    MoonrakerSubscriptionId id;
    IMoonrakerClient* client_to_register = nullptr;
    std::weak_ptr<bool> weak_alive;
    bool should_defer = false;
    std::string plugin_id_copy;

    // Collect data while holding the mutex
    {
        std::lock_guard<std::mutex> lock(mutex_);

        id = next_moonraker_sub_id_++;
        plugin_id_copy = plugin_id_;

        // Check if Moonraker is connected
        if (moonraker_client_ != nullptr) {
            // Prepare for immediate subscription (will register outside lock)
            client_to_register = moonraker_client_;
            weak_alive = alive_flag_;
            active_moonraker_subscriptions_.push_back(id);
        } else {
            // Queue for later when Moonraker connects
            should_defer = true;
            deferred_subscriptions_.push_back({id, objects, callback});
        }
    }

    // Now invoke external code outside the lock to prevent deadlock
    if (client_to_register != nullptr) {
        // Subscribe immediately
        // Use weak_ptr to detect if plugin has been unloaded (prevents use-after-free)
        uint64_t client_sub_id = client_to_register->register_notify_update(
            [callback, objects, weak_alive](const json& update) {
                // Check if plugin is still alive before processing
                auto alive = weak_alive.lock();
                if (!alive || !*alive) {
                    return; // Plugin has been unloaded, skip callback
                }

                // Filter update to only include objects we subscribed to
                // The update is in format: { "object_name": { ... }, ... }
                json filtered;
                for (const auto& obj : objects) {
                    if (update.contains(obj)) {
                        filtered[obj] = update[obj];
                    }
                }
                if (!filtered.empty()) {
                    // Marshal to main thread for LVGL safety
                    // Plugins don't need to worry about threading
                    json filtered_copy = filtered; // Copy for capture
                    helix::ui::queue_update(
                        [callback, filtered_copy]() { callback(filtered_copy); });
                }
            });

        // Store the mapping from our ID to MoonrakerClient's ID for proper cleanup
        {
            std::lock_guard<std::mutex> lock(mutex_);
            moonraker_id_map_[id] = client_sub_id;
        }

        spdlog::debug("[plugin:{}] Moonraker subscription active (id={}, client_id={})",
                      plugin_id_copy, id, client_sub_id);
    } else if (should_defer) {
        spdlog::debug("[plugin:{}] Moonraker subscription deferred (id={})", plugin_id_copy, id);
    }

    return id;
}

bool PluginAPI::unsubscribe_moonraker(MoonrakerSubscriptionId id) {
    uint64_t client_sub_id = 0;
    IMoonrakerClient* client = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Check deferred subscriptions
        auto deferred_it =
            std::find_if(deferred_subscriptions_.begin(), deferred_subscriptions_.end(),
                         [id](const DeferredSubscription& sub) { return sub.id == id; });

        if (deferred_it != deferred_subscriptions_.end()) {
            deferred_subscriptions_.erase(deferred_it);
            spdlog::debug("[plugin:{}] Deferred Moonraker subscription removed (id={})", plugin_id_,
                          id);
            return true;
        }

        // Check active subscriptions
        auto active_it = std::find(active_moonraker_subscriptions_.begin(),
                                   active_moonraker_subscriptions_.end(), id);

        if (active_it != active_moonraker_subscriptions_.end()) {
            active_moonraker_subscriptions_.erase(active_it);

            // Look up the MoonrakerClient subscription ID for proper cleanup
            auto map_it = moonraker_id_map_.find(id);
            if (map_it != moonraker_id_map_.end()) {
                client_sub_id = map_it->second;
                moonraker_id_map_.erase(map_it);
                client = moonraker_client_;
            }
        } else {
            return false;
        }
    }

    // Call MoonrakerClient unsubscribe outside the lock
    if (client != nullptr && client_sub_id != 0) {
        client->unsubscribe_notify_update(client_sub_id);
        spdlog::debug("[plugin:{}] Moonraker subscription unsubscribed (id={}, client_id={})",
                      plugin_id_, id, client_sub_id);
    } else {
        spdlog::debug("[plugin:{}] Moonraker subscription removed (id={}, no client mapping)",
                      plugin_id_, id);
    }

    return true;
}

// ============================================================================
// Subject Registration
// ============================================================================

void PluginAPI::register_subject(const std::string& name, lv_subject_t* subject) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!subject) {
        spdlog::error("[plugin:{}] Cannot register null subject '{}'", plugin_id_, name);
        return;
    }

    // Register with LVGL XML system for bind_text/bind_flag support
    lv_xml_register_subject(nullptr, name.c_str(), subject);
    registered_subjects_.push_back(name);

    spdlog::debug("[plugin:{}] Subject registered: {}", plugin_id_, name);
}

bool PluginAPI::unregister_subject(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find(registered_subjects_.begin(), registered_subjects_.end(), name);
    if (it != registered_subjects_.end()) {
        registered_subjects_.erase(it);
        // TODO: Unregister from LVGL XML system
        spdlog::debug("[plugin:{}] Subject unregistered: {}", plugin_id_, name);
        return true;
    }

    return false;
}

// ============================================================================
// Service Registration
// ============================================================================

void PluginAPI::register_service(const std::string& name, void* service) {
    std::lock_guard<std::mutex> lock(mutex_);

    PluginRegistry::instance().register_service(name, service);
    registered_services_.push_back(name);

    spdlog::debug("[plugin:{}] Service registered: {}", plugin_id_, name);
}

bool PluginAPI::unregister_service(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find(registered_services_.begin(), registered_services_.end(), name);
    if (it != registered_services_.end()) {
        registered_services_.erase(it);
        PluginRegistry::instance().unregister_service(name);
        spdlog::debug("[plugin:{}] Service unregistered: {}", plugin_id_, name);
        return true;
    }

    return false;
}

void* PluginAPI::get_service(const std::string& name) const {
    return PluginRegistry::instance().get_service(name);
}

// ============================================================================
// Logging
// ============================================================================

void PluginAPI::log_info(const std::string& message) const {
    spdlog::info("[plugin:{}] {}", plugin_id_, message);
}

void PluginAPI::log_warn(const std::string& message) const {
    spdlog::warn("[plugin:{}] {}", plugin_id_, message);
}

void PluginAPI::log_error(const std::string& message) const {
    spdlog::error("[plugin:{}] {}", plugin_id_, message);
}

void PluginAPI::log_debug(const std::string& message) const {
    spdlog::debug("[plugin:{}] {}", plugin_id_, message);
}

// ============================================================================
// UI Injection
// ============================================================================

bool PluginAPI::inject_widget(const std::string& point_id, const std::string& xml_component,
                              const WidgetCallbacks& callbacks) {
    // Delegate to InjectionPointManager with our plugin ID
    bool success = InjectionPointManager::instance().inject_widget(plugin_id_, point_id,
                                                                   xml_component, callbacks);

    if (success) {
        spdlog::info("[plugin:{}] Injected widget '{}' into '{}'", plugin_id_, xml_component,
                     point_id);
    } else {
        spdlog::error("[plugin:{}] Failed to inject widget '{}' into '{}'", plugin_id_,
                      xml_component, point_id);
    }

    return success;
}

bool PluginAPI::register_xml_component(const std::string& plugin_dir, const std::string& filename) {
    // Build full path to the XML file
    // LVGL uses virtual filesystem with 'A:' prefix for POSIX driver
    std::string full_path = "A:";
    full_path += plugin_dir;
    if (!full_path.empty() && full_path.back() != '/') {
        full_path += '/';
    }
    full_path += filename;

    // Derive component name from filename (strip .xml extension)
    std::string component_name = filename;
    size_t ext_pos = component_name.rfind(".xml");
    if (ext_pos != std::string::npos) {
        component_name = component_name.substr(0, ext_pos);
    }

    // Register with LVGL XML system
    // Note: lv_xml_register_component_from_file expects the file path
    bool success = lv_xml_register_component_from_file(full_path.c_str());

    if (success) {
        spdlog::info("[plugin:{}] Registered XML component '{}' from '{}'", plugin_id_,
                     component_name, full_path);
    } else {
        spdlog::error("[plugin:{}] Failed to register XML component from '{}'", plugin_id_,
                      full_path);
    }

    return success;
}

bool PluginAPI::has_injection_point(const std::string& point_id) const {
    return InjectionPointManager::instance().has_point(point_id);
}

// ============================================================================
// Internal Methods
// ============================================================================

void PluginAPI::set_moonraker(IMoonrakerAPI* api, IMoonrakerClient* client) {
    std::lock_guard<std::mutex> lock(mutex_);
    moonraker_api_ = api;
    moonraker_client_ = client;
    spdlog::debug("[plugin:{}] Moonraker services updated", plugin_id_);
}

void PluginAPI::apply_deferred_subscriptions() {
    // Collect data while holding the mutex
    std::vector<DeferredSubscription> subs_to_apply;
    IMoonrakerClient* client_to_register = nullptr;
    std::weak_ptr<bool> weak_alive;
    std::string plugin_id_copy;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (moonraker_client_ == nullptr) {
            spdlog::warn("[plugin:{}] Cannot apply deferred subscriptions: client is null",
                         plugin_id_);
            return;
        }

        if (deferred_subscriptions_.empty()) {
            return;
        }

        plugin_id_copy = plugin_id_;
        client_to_register = moonraker_client_;
        weak_alive = alive_flag_;

        // Move subscriptions out so we can process them outside the lock
        subs_to_apply = std::move(deferred_subscriptions_);
        deferred_subscriptions_.clear();

        // Pre-register all IDs as active while we still hold the lock
        for (const auto& sub : subs_to_apply) {
            active_moonraker_subscriptions_.push_back(sub.id);
        }
    }

    // Now invoke external code outside the lock to prevent deadlock
    spdlog::info("[plugin:{}] Applying {} deferred Moonraker subscriptions", plugin_id_copy,
                 subs_to_apply.size());

    // Store client IDs after registration (need to reacquire lock)
    std::vector<std::pair<MoonrakerSubscriptionId, uint64_t>> id_mappings;

    for (auto& sub : subs_to_apply) {
        auto callback = sub.callback;
        auto objects = sub.objects;

        // Use weak_ptr to detect if plugin has been unloaded (prevents use-after-free)
        uint64_t client_sub_id = client_to_register->register_notify_update(
            [callback, objects, weak_alive](const json& update) {
                // Check if plugin is still alive before processing
                auto alive = weak_alive.lock();
                if (!alive || !*alive) {
                    return; // Plugin has been unloaded, skip callback
                }

                json filtered;
                for (const auto& obj : objects) {
                    if (update.contains(obj)) {
                        filtered[obj] = update[obj];
                    }
                }
                if (!filtered.empty()) {
                    // Marshal to main thread for LVGL safety
                    json filtered_copy = filtered;
                    helix::ui::queue_update(
                        [callback, filtered_copy]() { callback(filtered_copy); });
                }
            });

        id_mappings.emplace_back(sub.id, client_sub_id);
        spdlog::debug("[plugin:{}] Deferred subscription applied (id={}, client_id={})",
                      plugin_id_copy, sub.id, client_sub_id);
    }

    // Store all ID mappings
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [plugin_id, client_id] : id_mappings) {
            moonraker_id_map_[plugin_id] = client_id;
        }
    }
}

void PluginAPI::cleanup() {
    // Collect data for cleanup outside the lock
    std::vector<uint64_t> client_sub_ids;
    IMoonrakerClient* client = nullptr;
    std::string plugin_id_copy;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        plugin_id_copy = plugin_id_;

        // Mark plugin as no longer alive - prevents Moonraker callbacks from invoking
        // plugin code after unload (use-after-free prevention)
        if (alive_flag_) {
            *alive_flag_ = false;
        }

        // Unsubscribe from all events
        for (auto id : event_subscriptions_) {
            EventDispatcher::instance().unsubscribe(id);
        }
        event_subscriptions_.clear();

        // Collect MoonrakerClient subscription IDs for cleanup
        client = moonraker_client_;
        for (const auto& [plugin_id, client_id] : moonraker_id_map_) {
            client_sub_ids.push_back(client_id);
        }
        moonraker_id_map_.clear();
        deferred_subscriptions_.clear();
        active_moonraker_subscriptions_.clear();

        // Unregister all services
        for (const auto& name : registered_services_) {
            PluginRegistry::instance().unregister_service(name);
        }
        registered_services_.clear();

        // Clear subjects
        // TODO: Unregister from LVGL XML system when implemented
        registered_subjects_.clear();
    }

    // Unsubscribe from MoonrakerClient outside the lock
    if (client != nullptr && !client_sub_ids.empty()) {
        for (uint64_t client_id : client_sub_ids) {
            client->unsubscribe_notify_update(client_id);
        }
        spdlog::debug("[plugin:{}] Unsubscribed {} Moonraker callbacks", plugin_id_copy,
                      client_sub_ids.size());
    }

    spdlog::debug("[plugin:{}] Cleanup complete", plugin_id_copy);
}

} // namespace helix::plugin
