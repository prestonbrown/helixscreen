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
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;
struct _lv_event_t;
typedef struct _lv_event_t lv_event_t;

namespace helix {

/// The death signal of the owner that registers hardware-gate subject @p name, or null when
/// no owner is mapped. A token from any other owner would read as defended while defending
/// nothing, and an owner-token guard no longer falls back to the invalidation epoch.
SubjectLifetime gate_subject_lifetime(const char* name);

class PanelWidget;

/// Map of widget ID → reusable PanelWidget instance, passed into populate_widgets
/// so that expensive C++ state (e.g. camera streams) survives LVGL tree rebuilds.
using WidgetReuseMap = std::unordered_map<std::string, std::unique_ptr<PanelWidget>>;

/// Appended to a widget id in compute_visible_widget_ids() while its hardware
/// gate reads 0, so a gate flip changes the page's id list.
inline constexpr char GATED_ID_SUFFIX[] = "~gated";

/// A placed widget's footprint on the grid, in tracks, as the card background
/// pass reads it.
struct CardFootprint {
    int col, row, colspan, rowspan;
    /// The widget shares the fused card (PanelWidgetDef::merges_into_card)
    /// rather than painting a background of its own.
    bool merges;
};

/// One card background rectangle, in tracks.
struct CardRect {
    int col, row, colspan, rowspan;
    bool operator==(const CardRect& o) const {
        return col == o.col && row == o.row && colspan == o.colspan && rowspan == o.rowspan;
    }
};

/// The card backgrounds behind @p footprints. Merging widgets that touch form
/// one component (4-way adjacency over tracks); each component's bounding box,
/// minus the tracks a non-merging widget sits in, is split into maximal
/// rectangles, none of which ends partway through a merging widget. Pure, so
/// a full populate and an in-place relayout derive the same cards.
std::vector<CardRect> card_background_rects(const std::vector<CardFootprint>& footprints);

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

    /// Remove only T's slot, leaving every other registered type untouched -
    /// the scoped counterpart to register_shared_resource<T>(), for a caller
    /// that owns exactly one type's registration and must not disturb the
    /// others (e.g. an RAII guard unregistering itself while a sibling
    /// guard for a different T is still in scope).
    template <typename T> void unregister_shared_resource() {
        shared_resources_.erase(type_tag<T>());
    }

    void clear_shared_resources();

    // -- Per-panel rebuild callbacks --
    using RebuildCallback = std::function<void()>;
    void register_rebuild_callback(const std::string& panel_id, RebuildCallback cb);
    /// Teardown entry point, safe to call after the manager is destroyed: a
    /// process-lifetime panel can outlive this function-local static at exit.
    static void unregister_rebuild_callback(const std::string& panel_id);
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

    /// One tile whose hardware gate flipped between two visible-id snapshots.
    struct GateFlip {
        size_t index;   ///< Position in both snapshots
        bool now_gated; ///< Direction of the flip
    };

    /// The gate flips that turn @p before into @p after, or nullopt when
    /// anything else differs (an id, the order, the length). Placement never
    /// depends on gate state, so a flip-only change leaves every tile's cell as
    /// it is.
    static std::optional<std::vector<GateFlip>>
    gate_flips_only(const std::vector<std::string>& before, const std::vector<std::string>& after);

    /// Re-create only the flipped tiles of a populated page, each in the grid
    /// cell it already holds; every other tile and instance is untouched. A tile
    /// that gates loses its instance from @p widgets; one that un-gates gains a
    /// fresh instance there. Returns the newly attached instances for the caller
    /// to activate, or nullopt (having changed nothing) when a flipped tile is
    /// not on the page or the container is not a live grid.
    std::optional<std::vector<PanelWidget*>>
    swap_gated_tiles(const std::string& panel_id, lv_obj_t* container, int page_index,
                     const std::vector<std::string>& visible_ids,
                     const std::vector<GateFlip>& flips,
                     std::vector<std::unique_ptr<PanelWidget>>& widgets);

    /// Re-seat a populated page's tiles at their entries' cells in place, after
    /// edit mode moved, swapped or resized the widgets named in @p changed_ids
    /// on it, and replace only the card backgrounds that changed. Every tile
    /// keeps its objects; the one named @p resized_id (empty for none) is told
    /// its new span through notify_size_changed(), as a populate tells it.
    /// Returns false (having changed nothing) when the page needs a full
    /// populate instead: a tile with no placed entry, a tile outside
    /// @p changed_ids laid out anywhere but its entry's cell (placement moved
    /// it, or this grid reduced or grew its span), a resized widget that
    /// cannot draw at its new size, or a container that is not a live grid.
    bool relayout_tiles(const std::string& panel_id, lv_obj_t* container, int page_index,
                        const std::vector<std::string>& changed_ids, const std::string& resized_id,
                        std::vector<std::unique_ptr<PanelWidget>>& widgets);

    /// relayout_tiles() with every tile counted as moved: re-seats a populated page at
    /// its entries' cells when the page holds the same widgets with the same config and
    /// only placement differs, telling each tile whose span changed. Same refusals,
    /// minus the one about tiles outside the edit.
    bool reseat_tiles(const std::string& panel_id, lv_obj_t* container, int page_index,
                      std::vector<std::unique_ptr<PanelWidget>>& widgets);

    /// Counts PanelWidget::save_widget_config() calls: a widget that saved its config
    /// applied it in place, so a record of the config a page was built from is stale.
    uint64_t widget_config_saves() const {
        return widget_config_saves_;
    }
    void note_widget_config_saved() {
        ++widget_config_saves_;
    }

    // -- Gate observers --

    /// Observe every hardware gate subject so that widgets appear/disappear
    /// when capabilities change. Calls rebuild_cb on change. @p panel_id only
    /// keys the registration: any surface listing gated widgets (the home panel,
    /// the widget catalog) holds its own under a distinct key.
    void setup_gate_observers(const std::string& panel_id, RebuildCallback rebuild_cb);

    /// Release gate observers for a panel (call during deinit/shutdown). Safe to
    /// call after the manager is destroyed: a process-lifetime panel can outlive
    /// this function-local static at exit, and then there is nothing to release.
    static void clear_gate_observers(const std::string& panel_id);

    /// Clear cached widget config for a panel, forcing a full rebuild on the
    /// next populate_widgets() call. Use when the panel is destroyed or when
    /// the user explicitly edits the widget layout. Grid descriptors are owned
    /// per container, so they are not touched here.
    void clear_panel_config(const std::string& panel_id);

    /// Invalidate EVERY cached panel config and clear all per-page derived
    /// widget-list caches. Call when the active printer changes
    /// (PrinterSession::switch_printer) — per-printer layouts live at
    /// /printers/<active>/panel_widgets/<panel>, so a switch repoints
    /// Config::df() and every cached PanelWidgetConfig must reload from the
    /// now-current path. Marks each cached config dirty (next load() re-reads
    /// disk) and empties active_configs_. Grid descriptors are owned per
    /// container and outlive this call: containers built for the previous
    /// printer keep reading valid memory until they are deleted.
    /// Main-thread only — no synchronization on the cache maps.
    void clear_all_panel_configs();

    /// The set of definitions changed: reload every panel's layout and rebuild
    /// every panel and catalog that lists widgets. Main thread. Each rebuild
    /// is queued through that panel's gate-rebuild async slot, never run
    /// inline, because a definitions change can arrive inside an UpdateQueue
    /// drain (a plugin fault) where a synchronous rebuild corrupts LVGL's
    /// event list.
    void notify_widget_defs_changed();

    /// Get the PanelWidgetConfig for a panel (creates if needed).
    class PanelWidgetConfig& get_widget_config(const std::string& panel_id);

  private:
    uint64_t widget_config_saves_ = 0;
    bool relayout_tiles_impl(const std::string& panel_id, lv_obj_t* container, int page_index,
                             const std::vector<std::string>& changed_ids,
                             const std::string& resized_id,
                             std::vector<std::unique_ptr<PanelWidget>>& widgets, bool reseat_all);
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
