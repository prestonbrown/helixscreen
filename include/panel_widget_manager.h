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

    /// Observe hardware gate subjects and klippy_state so that widgets
    /// appear/disappear when capabilities change. Calls rebuild_cb on change.
    void setup_gate_observers(const std::string& panel_id, RebuildCallback rebuild_cb);

    /// Release gate observers for a panel (call during deinit/shutdown).
    void clear_gate_observers(const std::string& panel_id);

    /// Clear cached widget config for a panel, forcing a full rebuild on the
    /// next populate_widgets() call. Use when the panel is destroyed or when
    /// the user explicitly edits the widget layout.
    void clear_panel_config(const std::string& panel_id);

    /// Invalidate EVERY cached panel config and clear all per-page derived
    /// caches. Call when the active printer changes (Application::switch_printer)
    /// — per-printer layouts live at /printers/<active>/panel_widgets/<panel>,
    /// so a switch repoints Config::df() and every cached PanelWidgetConfig must
    /// reload from the now-current path. Marks each cached config dirty (next
    /// load() re-reads disk) and empties active_configs_ + grid_descriptors_.
    /// Main-thread only — no synchronization on the cache maps.
    void clear_all_panel_configs();

    /// Move grid_descriptors_ entries matching `prefix` (empty = all) into
    /// retired_grid_descriptors_ instead of freeing them — the clear paths have
    /// no container handle to unstyle, and LVGL's grid style still holds the raw
    /// dsc pointers. See retired_grid_descriptors_ for the lifetime contract.
    void retire_grid_descriptors_matching(const std::string& prefix);

    /// Get the PanelWidgetConfig for a panel (creates if needed).
    class PanelWidgetConfig& get_widget_config(const std::string& panel_id);

  private:
    PanelWidgetManager() = default;

    /// Build a cache key from panel_id and page_index for grid_descriptors_ and active_configs_.
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

    /// Per-panel grid descriptor arrays — must persist while the grid layout is active
    /// on the associated container. Keyed by panel_id to support multiple panels.
    struct GridDescriptors {
        std::vector<int32_t> col_dsc;
        std::vector<int32_t> row_dsc;
    };
    std::unordered_map<std::string, GridDescriptors> grid_descriptors_;

    /// Descriptor arrays dropped by clear_panel_config()/clear_all_panel_configs().
    /// LVGL's grid style stores the raw dsc pointers WITHOUT copying them, and the
    /// clear paths have no handle to the container(s) to unstyle, so freeing the
    /// vectors on the spot leaves every still-existing grid reading freed memory
    /// (heap-use-after-free in grid_count_tracks via GridEditMode::current_metrics,
    /// 2026-08-17 nightly). Keyed by the original cache key: a populate_page() for
    /// that key re-points the container's style, which is the first moment the old
    /// array is provably unreferenced, and that is exactly when the entry is
    /// dropped. Bounded by the same panel×page count as grid_descriptors_ itself.
    std::unordered_map<std::string, GridDescriptors> retired_grid_descriptors_;

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
