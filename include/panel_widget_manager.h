// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_coalesced_timer.h"
#include "ui_observer_guard.h"

#include <any>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace helix {

class PanelWidget;

/// Central manager for panel widget lifecycle, shared resources, and config change
/// notifications. Widgets and panels interact through this singleton rather than
/// reaching into each other directly.
class PanelWidgetManager {
  public:
    static PanelWidgetManager& instance();

    // -- Shared resources --
    // Type-erased storage. Widgets request shared objects by type.
    template <typename T> void register_shared_resource(std::shared_ptr<T> resource) {
        shared_resources_[std::type_index(typeid(T))] = std::move(resource);
    }

    /// Register a non-owning raw pointer as a shared resource.
    /// The caller is responsible for ensuring the pointed-to object outlives usage.
    template <typename T> void register_shared_resource(T* raw) {
        // Wrap in a no-op-deleter shared_ptr so retrieval path stays uniform.
        shared_resources_[std::type_index(typeid(T))] = std::shared_ptr<T>(raw, [](T*) {});
    }

    template <typename T> T* shared_resource() const {
        auto it = shared_resources_.find(std::type_index(typeid(T)));
        if (it == shared_resources_.end())
            return nullptr;
        auto ptr = std::any_cast<std::shared_ptr<T>>(&it->second);
        return ptr ? ptr->get() : nullptr;
    }

    void clear_shared_resources();

    // -- Per-panel rebuild callbacks --
    using RebuildCallback = std::function<void()>;
    void register_rebuild_callback(const std::string& panel_id, RebuildCallback cb);
    void unregister_rebuild_callback(const std::string& panel_id);
    void notify_config_changed(const std::string& panel_id);

    // -- Widget subjects --

    /// Initialize subjects for all registered widgets that have init_subjects hooks.
    /// Must be called before any XML that references widget subjects is created.
    /// Idempotent - safe to call multiple times.
    void init_widget_subjects();

    // -- Widget lifecycle --

    /// Build widgets from PanelWidgetConfig for the given panel, creating XML
    /// components and attaching PanelWidget instances via their factories.
    /// Returns the vector of active (attached) PanelWidget instances.
    std::vector<std::unique_ptr<PanelWidget>> populate_widgets(const std::string& panel_id,
                                                               lv_obj_t* container);

    // -- Gate observers --

    /// Observe hardware gate subjects and klippy_state so that widgets
    /// appear/disappear when capabilities change. Calls rebuild_cb on change.
    void setup_gate_observers(const std::string& panel_id, RebuildCallback rebuild_cb);

    /// Release gate observers for a panel (call during deinit/shutdown).
    void clear_gate_observers(const std::string& panel_id);

  private:
    PanelWidgetManager() = default;

    bool widget_subjects_initialized_ = false;
    std::unordered_map<std::type_index, std::any> shared_resources_;
    std::unordered_map<std::string, RebuildCallback> rebuild_callbacks_;

    /// Per-panel gate observers that trigger widget rebuilds on hardware changes
    std::unordered_map<std::string, std::vector<ObserverGuard>> gate_observers_;

    /// Per-panel coalesced rebuild timers — batches rapid gate observer changes
    /// into a single rebuild per LVGL frame instead of one per subject change
    std::unordered_map<std::string, ui::CoalescedTimer> rebuild_timers_;
};

} // namespace helix
