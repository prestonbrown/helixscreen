// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "helix_type_tag.h"
#include "panel_widget_config.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;
struct _lv_event_t;
typedef struct _lv_event_t lv_event_t;

namespace helix {

class PanelWidget;

/// Map of widget ID → reusable PanelWidget instance, passed into populate_widgets
/// so that expensive C++ state (e.g. camera streams) survives LVGL tree rebuilds.
using WidgetReuseMap = std::unordered_map<std::string, std::unique_ptr<PanelWidget>>;

/// Central manager for panel widget lifecycle, shared resources, and config change
/// notifications. Widgets and panels interact through this singleton rather than
/// reaching into each other directly.
class PanelWidgetManager {
  public:
    static PanelWidgetManager& instance();

    // -- Shared resources --
    // Type-erased storage keyed by helix::type_tag<T>() rather than typeid, so this
    // header compiles under -fno-rtti (ESP32 firmware). The stored void* is always
    // the T* the caller named at registration: the tag is the key, so a slot can only
    // ever be written by register_shared_resource<T> and read by shared_resource<T>
    // for the same T. That makes the static_cast<T*> on retrieval exact - it is the
    // reverse of the static_pointer_cast<void> below, not a cross-hierarchy guess.
    // Registration under a base/interface type is still the caller's choice (see
    // subject_initializer.cpp registering IMoonrakerAPI); the pointer is converted to
    // the base at the call site, before erasure, so both ends agree on T.
    template <typename T> void register_shared_resource(std::shared_ptr<T> resource) {
        shared_resources_[type_tag<T>()] = std::static_pointer_cast<void>(std::move(resource));
    }

    /// Register a non-owning raw pointer as a shared resource.
    /// The caller is responsible for ensuring the pointed-to object outlives usage.
    template <typename T> void register_shared_resource(T* raw) {
        // Wrap in a no-op-deleter shared_ptr so retrieval path stays uniform.
        shared_resources_[type_tag<T>()] =
            std::shared_ptr<void>(static_cast<void*>(raw), [](void*) {});
    }

    template <typename T> T* shared_resource() const {
        auto it = shared_resources_.find(type_tag<T>());
        if (it == shared_resources_.end())
            return nullptr;
        return static_cast<T*>(it->second.get());
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
                                                               lv_obj_t* container,
                                                               int page_index = 0,
                                                               WidgetReuseMap reuse = {});

    /// Compute which widget IDs would be visible for a panel without creating
    /// any LVGL objects. Used to short-circuit rebuilds when the list is unchanged.
    std::vector<std::string> compute_visible_widget_ids(const std::string& panel_id,
                                                        int page_index = 0);

    // -- Gate observers --

    /// Observe every hardware gate subject so that widgets appear/disappear
    /// when capabilities change. Calls rebuild_cb on change.
    void setup_gate_observers(const std::string& panel_id, RebuildCallback rebuild_cb);

    /// Release gate observers for a panel (call during deinit/shutdown).
    void clear_gate_observers(const std::string& panel_id);

    /// Clear cached widget config for a panel, forcing a full rebuild on the
    /// next populate_widgets() call. Use when the panel is destroyed or when
    /// the user explicitly edits the widget layout. Grid descriptors are owned
    /// per container, so they are not touched here.
    void clear_panel_config(const std::string& panel_id);

    /// Invalidate EVERY cached panel config and clear all per-page derived
    /// widget-list caches. Call when the active printer changes
    /// (Application::switch_printer) — per-printer layouts live at
    /// /printers/<active>/panel_widgets/<panel>, so a switch repoints
    /// Config::df() and every cached PanelWidgetConfig must reload from the
    /// now-current path. Marks each cached config dirty (next load() re-reads
    /// disk) and empties active_configs_. Grid descriptors are owned per
    /// container and outlive this call: containers built for the previous
    /// printer keep reading valid memory until they are deleted.
    /// Main-thread only — no synchronization on the cache maps.
    void clear_all_panel_configs();

    /// Get the PanelWidgetConfig for a panel (creates if needed).
    class PanelWidgetConfig& get_widget_config(const std::string& panel_id);

  private:
    friend struct PanelWidgetManagerTestAccess;

    PanelWidgetManager();
    ~PanelWidgetManager();

    /// Build a cache key from panel_id and page_index for active_configs_.
    static std::string make_cache_key(const std::string& panel_id, int page_index) {
        return panel_id + ":" + std::to_string(page_index);
    }

    bool widget_subjects_initialized_ = false;
    bool populating_ = false;
    /// Keyed by helix::type_tag<T>(); values are the erased T* (see the accessors above).
    std::unordered_map<std::size_t, std::shared_ptr<void>> shared_resources_;
    std::unordered_map<std::string, RebuildCallback> rebuild_callbacks_;

    /// Per-panel gate observers that trigger widget rebuilds on hardware changes
    std::unordered_map<std::string, std::vector<ObserverGuard>> gate_observers_;

    /// Per-panel async-rebuild slot. Stable storage in the singleton so the
    /// `lv_async_call(trampoline, &slot)` user-data pointer is valid across
    /// the entire panel-registration lifetime — no per-firing `new` (which on
    /// memory-tight AD5X risks std::bad_alloc → terminate → SIGABRT through
    /// the LVGL C frame, [L083]). `clear_gate_observers()` calls
    /// `lv_async_call_cancel(trampoline, &slot)` before erasing so a queued
    /// rebuild can't fire on a destroyed registration.
    ///
    /// Coalescing semantics unchanged: `pending=true` while a rebuild is
    /// queued; the trampoline clears it before invoking rebuild_cb so any
    /// gate firing while the rebuild runs queues a fresh rebuild for the
    /// next tick.
    struct GateRebuildSlot {
        PanelWidgetManager* mgr = nullptr;
        std::string panel_id;
        bool pending = false;
    };
    std::unordered_map<std::string, GateRebuildSlot> gate_rebuild_slots_;
    std::unordered_map<std::string, RebuildCallback> gate_rebuild_callbacks_;

    /// Stable function pointer for `lv_async_call_cancel` — non-capturing
    /// lambda addresses aren't guaranteed stable across cancel/queue calls.
    static void gate_rebuild_trampoline(void* ud);

    /// Grid descriptor arrays, one generation per container. LVGL's grid style
    /// stores the raw dsc pointers WITHOUT copying them, so each generation
    /// must outlive the container it was installed on. Keyed by that container:
    /// the slot dies with the container (on_container_delete erases it), and a
    /// repopulate retires the previous generation only after the container's
    /// style has been re-pointed at the fresh arrays (install_grid_descriptors).
    struct GridDescriptors {
        std::vector<int32_t> col_dsc;
        std::vector<int32_t> row_dsc;
    };
    std::unordered_map<lv_obj_t*, GridDescriptors> grid_descriptors_;

    /// Install `fresh` as `container`'s descriptor generation, re-pointing the
    /// container's grid style at it before the previous generation's buffers
    /// are freed.
    void install_grid_descriptors(lv_obj_t* container, GridDescriptors&& fresh);

    /// LV_EVENT_DELETE callback on every container holding a grid_descriptors_
    /// slot: erases the slot so the map is bounded by live containers.
    static void on_container_delete(lv_event_t* e);

    /// Set by ~PanelWidgetManager. The delete callback fires from
    /// lv_obj_delete()/lv_deinit(), both inside main(), but the manager is a
    /// function-local static whose destructor runs after main returns - an
    /// exit-time delete event must not reach into the destroyed singleton.
    static bool s_destroyed_;

    /// Track current widget configuration per panel to detect no-op rebuilds.
    /// When populate_widgets() is called and the ordered list of widget IDs
    /// hasn't changed, the teardown+rebuild cycle is skipped entirely.
    struct ActiveWidgetConfig {
        std::vector<std::string> widget_ids; // ordered list of active widget IDs
    };
    std::unordered_map<std::string, ActiveWidgetConfig> active_configs_;

    /// Per-panel PanelWidgetConfig instances, cached by panel ID and lazily
    /// created on first access. Main-thread only — no synchronization. A cached
    /// config's load() is a no-op once loaded (#804); invalidation is explicit
    /// via mark_dirty() (notify_config_changed) or clear_all_panel_configs()
    /// (active-printer switch).
    std::unordered_map<std::string, PanelWidgetConfig> panel_configs_;
};

} // namespace helix
