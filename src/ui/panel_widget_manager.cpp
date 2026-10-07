// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_manager.h"

#include "ui_ams_mini_status.h"
#include "ui_notification.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "filament_sensor_manager.h"
#include "grid_edit_mode.h"
#include "grid_layout.h"
#include "humidity_sensor_manager.h"
#include "layout_manager.h"
#include "layout_port.h"
#include "led/led_controller.h"
#include "observer_factory.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"
#include "printer_cache_registry.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/tile_sizing.h"
#include "system/crash_handler.h"
#include "system/telemetry_manager.h"
#include "temperature_sensor_manager.h"
#include "theme_manager.h"
#include "width_sensor_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <queue>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace helix {

namespace {

/// A saved placement clamped into the grid the panel actually has.
struct ClampedCell {
    int col, row, colspan, rowspan;
};

/// Fit a saved span and origin to `cols` x `rows`.
///
/// A span saved against a different grid cannot exist on this one, so the span
/// is clamped first and the origin is then pushed back far enough for it to fit
/// (#1216). Placement and the merged-card pass must agree on this: the card pass
/// reasons about every enabled config entry, not just the placed ones, and an
/// unclamped span there marks cells occupied that the widget does not cover —
/// which then subtracts those cells from a neighbour's card.
///
/// `col_step` / `row_step` are the track boundaries the widget may occupy. A
/// saved layout can carry an origin or span off those boundaries — written by a
/// build where the widget still declared half-cell support, or edited by hand —
/// and the load path used to honour it verbatim, so the straddle survived every
/// restart (#1126). Snapping DOWN keeps the widget's top-left where the user put
/// it to within half a cell; if the snapped position now collides, the caller's
/// existing fallback re-seats it through auto-placement.
ClampedCell clamp_to_grid(int col, int row, int colspan, int rowspan, int cols, int rows,
                          int col_step = GridLayout::TRACKS_PER_CELL,
                          int row_step = GridLayout::TRACKS_PER_CELL) {
    col_step = std::max(1, col_step);
    row_step = std::max(1, row_step);

    // Round the span UP and the origin DOWN, so the widget never ends up
    // covering less than it was saved with.
    auto ceil_to = [](int v, int step) { return ((v + step - 1) / step) * step; };
    auto floor_to = [](int v, int step) { return v >= 0 ? (v / step) * step : v; };

    ClampedCell c{floor_to(col, col_step), floor_to(row, row_step),
                  std::clamp(ceil_to(colspan, col_step), std::min(col_step, cols), cols),
                  std::clamp(ceil_to(rowspan, row_step), std::min(row_step, rows), rows)};
    if (c.row + c.rowspan > rows) {
        c.row = std::max(0, floor_to(rows - c.rowspan, row_step));
    }
    if (c.col + c.colspan > cols) {
        c.col = std::max(0, floor_to(cols - c.colspan, col_step));
    }
    return c;
}

/// Convert a panel's saved pre-v22 cell coordinates to tracks, once.
///
/// The v22 migration tags the layout instead of converting it, because config
/// load settles no screen size and settings.json records none. Both grids are
/// known here: the new one was just measured, and the old one is deterministic
/// in the panel extent (legacy_grid_cols) with its row count readable off the
/// layout itself (legacy_grid_rows). Every page shares this grid, so all of
/// them port together and the tag drops in one save.
///
/// A widget the port cannot seat is left at -1 with its registry span, which
/// drops it into the same auto-placement pass a never-positioned widget takes.
/// Failing per widget rather than per layout is the whole reason to try: the
/// worst case degrades to one widget losing its spot, not the dashboard.
void port_legacy_page_layouts(PanelWidgetConfig& widget_config, const std::string& panel_id,
                              const GridDimensions& dims) {
    auto& lm = LayoutManager::instance();
    const int old_cols = legacy_grid_cols(lm.width(), lm.height());
    if (old_cols <= 0) {
        spdlog::warn("[PanelWidgetManager] '{}': cannot reconstruct the pre-v22 grid from a "
                     "{}x{} panel; leaving the layout to auto-placement",
                     panel_id, lm.width(), lm.height());
        widget_config.clear_legacy_units();
        return;
    }

    int seated = 0;
    int positioned = 0;
    for (size_t page = 0; page < widget_config.page_count(); ++page) {
        auto& entries = widget_config.page_entries_mut(page);
        std::vector<LegacyPlacement> saved;
        saved.reserve(entries.size());
        for (const auto& e : entries) {
            saved.push_back({e.id, e.col, e.row, e.colspan, e.rowspan});
            if (e.has_grid_position()) {
                ++positioned;
            }
        }

        const int old_rows = legacy_grid_rows(saved, widget_config.legacy_rows());
        const auto ported = port_legacy_layout(saved, old_cols, old_rows, dims.cols, dims.rows);

        for (size_t i = 0; i < entries.size() && i < ported.size(); ++i) {
            if (ported[i].seated) {
                entries[i].col = ported[i].col;
                entries[i].row = ported[i].row;
                entries[i].colspan = ported[i].colspan;
                entries[i].rowspan = ported[i].rowspan;
                ++seated;
                continue;
            }
            // The saved span counts cells; read as tracks it would be half the
            // widget. The registry default is the same answer parse_widget_array
            // gives an entry that omits its span.
            const auto* def = find_widget_def(entries[i].id);
            entries[i].col = -1;
            entries[i].row = -1;
            entries[i].colspan = def ? def->colspan : GridLayout::TRACKS_PER_CELL;
            entries[i].rowspan = def ? def->rowspan : GridLayout::TRACKS_PER_CELL;
        }
    }

    spdlog::info("[PanelWidgetManager] '{}': ported {} of {} placed widget(s) from pre-v22 "
                 "cells onto a {}x{} track grid ({} panel); the rest are auto-placed",
                 panel_id, seated, positioned, dims.cols, dims.rows,
                 std::to_string(lm.width()) + "x" + std::to_string(lm.height()));
    widget_config.clear_legacy_units();
}

} // namespace

std::vector<CardRect> card_background_rects(const std::vector<CardFootprint>& footprints) {
    // Tracks covered by widgets that share the fused card, and tracks blocked
    // by widgets that bring a background of their own.
    struct TrackHash {
        size_t operator()(const std::pair<int, int>& p) const {
            return std::hash<int>()(p.first) ^ (std::hash<int>()(p.second) << 16);
        }
    };
    std::unordered_set<std::pair<int, int>, TrackHash> merged_tracks;
    std::unordered_set<std::pair<int, int>, TrackHash> occupied_by_own_card;
    // Footprints of the merging widgets, so no card piece ends inside one.
    std::vector<const CardFootprint*> merged_footprints;
    for (const auto& f : footprints) {
        auto& covered = f.merges ? merged_tracks : occupied_by_own_card;
        if (f.merges) {
            merged_footprints.push_back(&f);
        }
        for (int r = f.row; r < f.row + f.rowspan; r++) {
            for (int c = f.col; c < f.col + f.colspan; c++) {
                covered.insert({c, r});
            }
        }
    }

    std::vector<CardRect> cards;
    // BFS flood-fill to find connected components (4-directional adjacency)
    std::unordered_set<std::pair<int, int>, TrackHash> visited;
    for (const auto& track : merged_tracks) {
        if (visited.count(track)) {
            continue;
        }

        // BFS from this track to collect the connected component
        std::queue<std::pair<int, int>> q;
        q.push(track);
        visited.insert(track);

        std::vector<std::pair<int, int>> component_tracks;
        while (!q.empty()) {
            auto [c, r] = q.front();
            q.pop();
            component_tracks.push_back({c, r});

            const std::pair<int, int> neighbors[] = {
                {c - 1, r}, {c + 1, r}, {c, r - 1}, {c, r + 1}};
            for (const auto& n : neighbors) {
                if (merged_tracks.count(n) && !visited.count(n)) {
                    visited.insert(n);
                    q.push(n);
                }
            }
        }

        // Build the card coverage: bounding box of the component, filling
        // gaps between merging widgets, but excluding tracks occupied by a
        // widget that paints its own background and intrudes on the region.
        int min_col = component_tracks[0].first;
        int max_col = min_col;
        int min_row = component_tracks[0].second;
        int max_row_card = min_row;
        for (const auto& [c, r] : component_tracks) {
            min_col = std::min(min_col, c);
            max_col = std::max(max_col, c);
            min_row = std::min(min_row, r);
            max_row_card = std::max(max_row_card, r);
        }

        // Cover the component's bounding box, minus any track a non-merging
        // widget sits in, then split what is left into maximal rectangles.
        //
        // Both halves of that matter. Running the bounding box straight
        // over a non-merging widget makes the card butt against that
        // widget's own card with no gutter, and two abutting Card surfaces
        // read as one continuous slab — a readout block and the graph below
        // it stop looking like separate things. Carving the tracks out is
        // what keeps the gutter.
        //
        // Every emitted piece is a rectangle.
        std::unordered_set<std::pair<int, int>, TrackHash> remaining;
        for (int r = min_row; r <= max_row_card; r++) {
            for (int c = min_col; c <= max_col; c++) {
                if (!occupied_by_own_card.count({c, r})) {
                    remaining.insert({c, r});
                }
            }
        }

        while (!remaining.empty()) {
            // Top-left of what is left: min row, then min col.
            auto top_left = *std::min_element(
                remaining.begin(), remaining.end(), [](const auto& a, const auto& b) {
                    return a.second < b.second || (a.second == b.second && a.first < b.first);
                });

            int start_col = top_left.first;
            int start_row = top_left.second;

            int max_end_col = start_col;
            while (remaining.count({max_end_col + 1, start_row})) {
                max_end_col++;
            }

            // A piece that ends partway through a widget draws that widget
            // across two cards with a seam through it, so every widget the
            // piece touches must lie wholly inside it.
            auto cuts_a_widget = [&](int c0, int r0, int c1, int r1) {
                for (const CardFootprint* w : merged_footprints) {
                    const int w_c1 = w->col + w->colspan - 1;
                    const int w_r1 = w->row + w->rowspan - 1;
                    const bool touches = w->col <= c1 && c0 <= w_c1 && w->row <= r1 && r0 <= w_r1;
                    const bool inside = c0 <= w->col && w_c1 <= c1 && r0 <= w->row && w_r1 <= r1;
                    if (touches && !inside) {
                        return true;
                    }
                }
                return false;
            };

            // Widest run first, then as tall as it extends as a full
            // rectangle, backing off until no widget is cut. The widget (or
            // bare gap track) at the top-left always qualifies on its own,
            // because pieces never take part of a widget, so this ends.
            int end_col = start_col;
            int end_row = start_row;
            for (int c1 = max_end_col; c1 >= start_col; c1--) {
                int r1 = start_row;
                for (;;) {
                    bool can_extend = true;
                    for (int c = start_col; c <= c1; c++) {
                        if (!remaining.count({c, r1 + 1})) {
                            can_extend = false;
                            break;
                        }
                    }
                    if (!can_extend) {
                        break;
                    }
                    r1++;
                }
                while (r1 >= start_row && cuts_a_widget(start_col, start_row, c1, r1)) {
                    r1--;
                }
                if (r1 >= start_row) {
                    end_col = c1;
                    end_row = r1;
                    break;
                }
            }

            for (int r = start_row; r <= end_row; r++) {
                for (int c = start_col; c <= end_col; c++) {
                    remaining.erase({c, r});
                }
            }

            cards.push_back(
                {start_col, start_row, end_col - start_col + 1, end_row - start_row + 1});
        }
    }
    return cards;
}

PanelWidgetManager::PanelWidgetManager() = default;

PanelWidgetManager::~PanelWidgetManager() {
    s_destroyed_ = true;
}

PanelWidgetManager& PanelWidgetManager::instance() {
    static PanelWidgetManager instance;
    return instance;
}

void PanelWidgetManager::clear_shared_resources() {
    shared_resources_.clear();
}

void PanelWidgetManager::init_widget_subjects() {
    // Register all widget factories explicitly (avoids SIOF from file-scope statics)
    init_widget_registrations();

    // Every call, not once: a printer switch deinits these subjects through
    // StaticSubjectRegistry and withdraws their XML names, and the rebuilt panels
    // bind those names again. Each hook is a no-op while its subjects are live.
    for (const auto& def : get_all_widget_defs()) {
        if (def.init_subjects) {
            spdlog::debug("[PanelWidgetManager] Initializing subjects for widget '{}'", def.id);
            def.init_subjects();
        }
    }

    if (widget_subjects_initialized_) {
        return;
    }
    widget_subjects_initialized_ = true;

    // Self-register per-printer cache invalidation. panel_configs_ / active_configs_
    // both derive from /printers/<active>/panel_widgets/<panel>, so an
    // active-printer change must drop them (#804). This manager is a process-lifetime
    // singleton, so there is no matching unregister().
    PrinterCacheRegistry::instance().register_invalidator(
        "PanelWidgetManager", []() { PanelWidgetManager::instance().clear_all_panel_configs(); });

    spdlog::debug("[PanelWidgetManager] Widget subjects initialized");
}

void PanelWidgetManager::register_rebuild_callback(const std::string& panel_id,
                                                   RebuildCallback cb) {
    rebuild_callbacks_[panel_id] = std::move(cb);
}

void PanelWidgetManager::unregister_rebuild_callback(const std::string& panel_id) {
    if (s_destroyed_) {
        return;
    }
    instance().rebuild_callbacks_.erase(panel_id);
}

void PanelWidgetManager::notify_config_changed(const std::string& panel_id) {
    // Invalidate the cached PanelWidgetConfig so the next access reloads from disk.
    // Callers that mutate panel_widgets/<panel_id> directly via Config (rather
    // than via PanelWidgetConfig setters + save) must route through here so the
    // cache can't serve stale data (#804 defensive).
    get_widget_config(panel_id).mark_dirty();

    auto it = rebuild_callbacks_.find(panel_id);
    if (it != rebuild_callbacks_.end()) {
        it->second();
    }
}

namespace {
// Widget builds that stall the UI thread, split into XML creation and the rest
// (placement, gating, attach). Silent below the threshold, so only a slow
// device or a pathological widget ever logs.
void log_if_slow_build(const char* what, std::chrono::steady_clock::time_point t0,
                       std::chrono::steady_clock::time_point t_split) {
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    constexpr long SLOW_BUILD_MS = 100;
    const auto now = std::chrono::steady_clock::now();
    const long total = static_cast<long>(duration_cast<milliseconds>(now - t0).count());
    if (total < SLOW_BUILD_MS)
        return;
    spdlog::info("[PanelWidgetManager] slow build '{}': {}ms (xml {}ms, rest {}ms)", what, total,
                 static_cast<long>(duration_cast<milliseconds>(t_split - t0).count()),
                 static_cast<long>(duration_cast<milliseconds>(now - t_split).count()));
}

// Resolved widget slot: holds the widget ID, resolved XML component name,
// per-widget config, and optionally a pre-created PanelWidget instance.
struct WidgetSlot {
    std::string widget_id;
    std::string component_name;
    nlohmann::json config;
    std::unique_ptr<PanelWidget> instance; // nullptr for pure-XML widgets
    bool hardware_gated = false;           // Gate subject is 0
    const char* gate_hint = nullptr;       // Human-readable hint
};

// The gate subject named by @p def reads 0: the widget is placed, dimmed and
// badged, with no PanelWidget attached. Gates are checked here instead of via
// XML bind_flag_if_eq to avoid orphaned dividers.
const char* gate_hint_if_gated(const PanelWidgetDef* def, bool& gated) {
    gated = false;
    if (def && def->hardware_gate_subject) {
        lv_subject_t* gate = lv_xml_get_subject(nullptr, def->hardware_gate_subject);
        if (gate && lv_subject_get_int(gate) == 0) {
            gated = true;
            return def->hardware_gate_hint;
        }
    }
    return nullptr;
}

std::string visible_id(const std::string& widget_id, bool gated) {
    return gated ? widget_id + GATED_ID_SUFFIX : widget_id;
}

// Resolve one enabled config entry: its gate state, and its PanelWidget (taken
// from @p reuse or made by the factory) configured for @p panel_id. A malformed
// per-widget config or a throwing factory skips only this widget (nullopt).
std::optional<WidgetSlot> resolve_slot(const std::string& panel_id, const PanelWidgetEntry& entry,
                                       WidgetReuseMap& reuse) {
    const auto* def = find_widget_def(entry.id);
    WidgetSlot slot;
    slot.widget_id = entry.id;
    slot.config = entry.config;
    slot.gate_hint = gate_hint_if_gated(def, slot.hardware_gated);
#if defined(__cpp_exceptions)
    try {
#else
    {
#endif
        auto reuse_it = reuse.find(entry.id);
        if (reuse_it != reuse.end()) {
            slot.instance = std::move(reuse_it->second);
            reuse.erase(reuse_it);
            spdlog::debug("[PanelWidgetManager] Reusing widget instance '{}'", entry.id);
        } else if (def && def->factory) {
            slot.instance = def->factory(entry.id);
        }

        if (slot.instance) {
            slot.instance->set_panel_id(panel_id);
            slot.instance->set_config(entry.config);
            slot.component_name = slot.instance->get_component_name();
        } else {
            slot.component_name = "panel_widget_" + entry.id;
        }
#if defined(__cpp_exceptions)
    } catch (const std::exception& e) {
        spdlog::error("[PanelWidgetManager] Widget '{}' configuration failed: {}", entry.id,
                      e.what());
        return std::nullopt;
    }
#else
    }
#endif
    return slot;
}

/// Create the card-styled background object for @p card in @p container,
/// placed in its grid cell.
lv_obj_t* create_card_background(lv_obj_t* container, const CardRect& card,
                                 const CellMetrics& metrics) {
    lv_obj_t* card_bg = lv_obj_create(container);
    lv_obj_remove_style(card_bg, nullptr, LV_PART_MAIN);
    lv_obj_add_style(card_bg, ThemeManager::instance().get_style(StyleRole::Card), LV_PART_MAIN);
    lv_obj_set_style_pad_all(card_bg, 0, 0);
    lv_obj_remove_flag(card_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card_bg, LV_OBJ_FLAG_SCROLLABLE);
    // Set initial size from the track dimensions so the card renders at
    // approximately the right shape on the first frame, before the grid layout
    // resolves. Grid STRETCH overrides once layout runs.
    if (metrics.cell_w > 0 && metrics.cell_h > 0) {
        lv_obj_set_size(
            card_bg,
            static_cast<int>(grid_track_extent(metrics.cell_w, metrics.gutter, card.colspan)),
            static_cast<int>(grid_track_extent(metrics.cell_h, metrics.gutter, card.rowspan)));
    }
    lv_obj_set_grid_cell(card_bg, LV_GRID_ALIGN_STRETCH, card.col, card.colspan,
                         LV_GRID_ALIGN_STRETCH, card.row, card.rowspan);
    spdlog::debug("[PanelWidgetManager] Card background at ({},{} {}x{} tracks)", card.col,
                  card.row, card.colspan, card.rowspan);
    return card_bg;
}

struct TileCell {
    int col, row, colspan, rowspan;
};

// Tell a tile the cell it holds: its attached instance (if any) through
// notify_size_changed(), which records the grant first so a widget that
// rebuilds its contents later lays them out against the same cell, and the AMS
// mini status (a pure XML widget) its width.
void announce_tile_size(lv_obj_t* tile, const std::string& widget_id, PanelWidget* instance,
                        const TileCell& cell, const CellMetrics& metrics) {
    const int width =
        static_cast<int>(grid_track_extent(metrics.cell_w, metrics.gutter, cell.colspan));
    if (instance) {
        instance->notify_size_changed(
            cell.colspan, cell.rowspan, width,
            static_cast<int>(grid_track_extent(metrics.cell_h, metrics.gutter, cell.rowspan)));
    }
    if (widget_id == "ams") {
        lv_obj_t* ams_child = lv_obj_get_child(tile, 0);
        if (ams_child && ui_ams_mini_status_is_valid(ams_child)) {
            ui_ams_mini_status_set_width(ams_child, width);
        }
    }
}

// Create one tile in its grid cell: the XML component, its name and tile flag,
// the gated treatment or the attached PanelWidget (moved into @p result), and
// the size notification. Returns the tile root, or nullptr if creation failed.
lv_obj_t* create_tile(lv_obj_t* container, WidgetSlot& slot, const TileCell& cell,
                      const CellMetrics& metrics,
                      std::vector<std::unique_ptr<PanelWidget>>& result) {
    const auto t_create = std::chrono::steady_clock::now();

    // Create XML component
    auto* widget =
        static_cast<lv_obj_t*>(lv_xml_create(container, slot.component_name.c_str(),
                                             slot.instance ? slot.instance->xml_attrs() : nullptr));
    if (!widget) {
        spdlog::warn("[PanelWidgetManager] Failed to create widget: {} (component: {})",
                     slot.widget_id, slot.component_name);
        return nullptr;
    }

    const auto t_attach = std::chrono::steady_clock::now();

    // Place in grid cell
    lv_obj_set_grid_cell(widget, LV_GRID_ALIGN_STRETCH, cell.col, cell.colspan,
                         LV_GRID_ALIGN_STRETCH, cell.row, cell.rowspan);

    // Tag widget with its config ID so GridEditMode can identify it
    lv_obj_set_name(widget, slot.widget_id.c_str());

    // Mark the tile root so tree walks that only make sense at page
    // level stop here. See PANEL_WIDGET_TILE_FLAG in panel_widget.h.
    lv_obj_add_flag(widget, PANEL_WIDGET_TILE_FLAG);

    spdlog::debug("[PanelWidgetManager] Placed widget '{}' at ({},{} {}x{})", slot.widget_id,
                  cell.col, cell.row, cell.colspan, cell.rowspan);

    // Apply gated visual treatment — widget is placed but hardware not detected.
    // Stack the widget's own type icon underneath a slash-circle badge, both
    // centered, so the user can tell *which* widget is disabled (filament,
    // AMS, etc.) and that it's currently inactive. Both icons are FLOATING
    // so they sit on top of any existing widget content.
    if (slot.hardware_gated) {
        lv_obj_set_style_opa(widget, LV_OPA_40, 0);
        lv_obj_add_state(widget, LV_STATE_DISABLED);
        // LV_STATE_DISABLED alone is not enough. A tile's tap handler is
        // usually declared as an <event_cb> on its XML component root, so
        // it is bound when the tile is PLACED - independent of gating -
        // and a gated tile could still open a panel describing hardware
        // that is not there. Clearing CLICKABLE takes it out of the
        // indev hit test entirely, so the press walks up to the parent
        // instead. Safe without a restore path: an un-gate rebuilds the
        // tile from scratch (see the gate observers' rebuild).
        lv_obj_remove_flag(widget, LV_OBJ_FLAG_CLICKABLE);

        const auto* gated_def = find_widget_def(slot.widget_id);
        const char* type_icon = (gated_def && gated_def->icon) ? gated_def->icon : "cancel";

        // One step down from the badge (lg 48px vs xl 64px). Both glyphs
        // are round, so at equal size they coincide almost exactly and
        // the pair reads as one muddy shape rather than "this widget,
        // unavailable" - the badge has to ring the type icon, not sit on
        // top of it.
        const char* type_icon_attrs[] = {
            "src",    type_icon,   "size",  "lg",           "variant", "muted", "align",
            "center", "clickable", "false", "event_bubble", "true",    nullptr};
        if (auto* type_overlay =
                static_cast<lv_obj_t*>(lv_xml_create(widget, "icon", type_icon_attrs))) {
            lv_obj_add_flag(type_overlay, LV_OBJ_FLAG_FLOATING);
            lv_obj_set_style_opa(type_overlay, LV_OPA_COVER, 0);
        }

        const char* badge_attrs[] = {"src",          "cancel", "size",   "xl",        "variant",
                                     "muted",        "align",  "center", "clickable", "false",
                                     "event_bubble", "true",   nullptr};
        if (auto* badge = static_cast<lv_obj_t*>(lv_xml_create(widget, "icon", badge_attrs))) {
            lv_obj_add_flag(badge, LV_OBJ_FLAG_FLOATING);
            lv_obj_set_style_opa(badge, LV_OPA_COVER, 0);
        }

        spdlog::debug("[PanelWidgetManager] Widget '{}' gated: {}", slot.widget_id,
                      slot.gate_hint ? slot.gate_hint : "hardware not detected");
    }

    // Attach the pre-created PanelWidget instance if present and NOT gated
    PanelWidget* attached = nullptr;
    if (slot.instance && !slot.hardware_gated) {
        if (auto* sizing = slot.instance->tile_sizing()) {
            sizing->set_content_root(widget);
        }
        slot.instance->attach_tile(widget, lv_scr_act());
        attached = slot.instance.get();
        result.push_back(std::move(slot.instance));
    }
    announce_tile_size(widget, slot.widget_id, attached, cell, metrics);
    log_if_slow_build(slot.widget_id.c_str(), t_create, t_attach);
    return widget;
}
} // namespace

std::vector<std::unique_ptr<PanelWidget>>
PanelWidgetManager::populate_widgets(const std::string& panel_id, lv_obj_t* container,
                                     int page_index, WidgetReuseMap reuse) {
    if (!container) {
        spdlog::debug("[PanelWidgetManager] populate_widgets: null container for '{}'", panel_id);
        return {};
    }

    if (populating_) {
        spdlog::debug(
            "[PanelWidgetManager] populate_widgets: already in progress for '{}', skipping",
            panel_id);
        return {};
    }
    populating_ = true;

    auto& widget_config = get_widget_config(panel_id);

    // Collect enabled + hardware-available widgets
    std::vector<WidgetSlot> enabled_widgets;
    for (const auto& entry : widget_config.page_entries(page_index)) {
        if (!entry.enabled) {
            continue;
        }
        if (auto slot = resolve_slot(panel_id, entry, reuse)) {
            enabled_widgets.push_back(std::move(*slot));
        }
    }

    // Check if widget list is unchanged — skip teardown+rebuild if nothing changed.
    // Gate status is part of the key: a widget transitioning from gated→ungated
    // must trigger a rebuild so its cancel-icon overlay + OPA_40 are removed and
    // the PanelWidget instance gets attached. Matches compute_visible_widget_ids().
    {
        std::vector<std::string> new_ids;
        new_ids.reserve(enabled_widgets.size());
        for (const auto& slot : enabled_widgets) {
            new_ids.push_back(visible_id(slot.widget_id, slot.hardware_gated));
        }

        auto cache_key = make_cache_key(panel_id, page_index);
        auto it = active_configs_.find(cache_key);
        bool container_has_children = lv_obj_get_child_count(container) > 0;
        if (it != active_configs_.end() && it->second.widget_ids == new_ids &&
            container_has_children) {
            spdlog::debug("[PanelWidgetManager] Widget list unchanged for '{}', skipping rebuild",
                          cache_key);
            populating_ = false;
            return {};
        }

        // Store new config for future comparison
        active_configs_[cache_key] = ActiveWidgetConfig{std::move(new_ids)};
    }

    // Clear existing children (for repopulation). Use safe_clean_children so the
    // deletions run on LVGL's async list — multiple sync cleans in one
    // UpdateQueue batch (gate observers fanning out during CFS/AMS discovery)
    // corrupt LVGL's event linked list (#776, #834).
    helix::ui::safe_clean_children(container);

    // Deactivate grid layout for the duration of the rebuild. On a *rebuild* the
    // container is reused and is still in LV_LAYOUT_GRID from the previous pass,
    // its grid style holding a pointer into the old `dsc.col_dsc` buffer. The
    // move-assignment at `dsc.col_dsc = make_col_dsc(...)` below frees that buffer,
    // leaving the container's descriptor pointer dangling. Any child whose
    // attach() synchronously forces a layout (e.g. PrintStatusWidget ->
    // resize_and_publish -> lv_obj_update_layout) would then cascade grid_update
    // -> count_tracks over the freed descriptor and walk off the heap end ->
    // SIGSEGV (#983, bundle VDJ3J9UV). Turning the grid off here closes that
    // window; it is re-activated with the fresh descriptor at the end of the
    // build. The "activate grid last" guard alone is insufficient because it only
    // covers the first build, where the container is not yet a grid.
    lv_obj_set_layout(container, LV_LAYOUT_NONE);

    if (enabled_widgets.empty()) {
        populating_ = false;
        return {};
    }

    // --- Grid layout: compute placements first, then build minimal grid ---

    // Get current breakpoint for column count
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint breakpoint = bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj))
                                      : UiBreakpoint::Medium; // Default to MEDIUM

    // Measure what the tracks will actually be laid out inside. The track count
    // is derived from the CONTENT box, so this measurement decides the shape of
    // the whole dashboard, not just the pixel size it reports downstream.
    //
    // The layout pass is mandatory: on a first build the container has just been
    // created and its coordinates are still unresolved, so the content box reads
    // zero. Safe here because the grid was switched off at LV_LAYOUT_NONE above
    // and the children have not been created yet — there is no half-built grid
    // for a layout pass to cascade over (#983).
    lv_obj_update_layout(container);
    int content_w = lv_obj_get_content_width(container);
    int content_h = lv_obj_get_content_height(container);
    const bool content_box_measured = content_w > 0 && content_h > 0;
    if (content_w <= 0 || content_h <= 0) {
        // Nothing legitimate produces this; a container that measures empty
        // after an explicit layout pass is detached or zero-sized. Fall back to
        // the panel extent, which oversizes the grid slightly but keeps the
        // dashboard usable, rather than collapsing to the MIN_TRACKS floor and
        // silently disabling every widget too large for a 4x4 grid.
        auto& lm = LayoutManager::instance();
        spdlog::error("[PanelWidgetManager] '{}' content box measured {}x{} after layout; "
                      "falling back to the {}x{} panel extent",
                      panel_id, content_w, content_h, lm.width(), lm.height());
        content_w = lm.width();
        content_h = lm.height();
    }
    const GridDimensions grid_dims = GridLayout::get_dimensions(breakpoint, content_w, content_h);

    // Port a pre-v22 layout before anything reads its coordinates as tracks.
    // This is the first point in the run where both grids are knowable, which
    // is why the v22 migration deferred the conversion rather than doing it.
    //
    // Only against a real measurement. The port is one-shot and destructive —
    // it rewrites the coordinates and drops the tag — so running it on the
    // panel-extent fallback above would bake a layout derived from a grid this
    // panel never has. Skipping leaves the tag set for the next populate, and
    // until one measures cleanly the saved positions are simply not used, which
    // is the same dashboard the unconditional reset would have given.
    if (widget_config.has_legacy_units()) {
        if (content_box_measured) {
            port_legacy_page_layouts(widget_config, panel_id, grid_dims);
        } else {
            spdlog::warn("[PanelWidgetManager] '{}': deferring the pre-v22 layout port until the "
                         "content box measures",
                         panel_id);
        }
    }

    // Then apply the default anchors, for the same reason and at the same point:
    // build_defaults() runs at config load with no container to measure, and the
    // panel extent it could have guessed from is not what the track count
    // divides — the content box is, and the two disagree enough to pick a
    // different grid (1042x2141 against 1080x2400 is 6x14 tracks against 8x16 on
    // a scaled phone).
    //
    // The two tags never coexist: a pre-v22 layout is not a freshly-defaulted
    // one. Ordering them anyway keeps the invariant that nothing reads a
    // coordinate before the pass that owns its units has run.
    //
    // Neither runs while a connected printer reports Klipper not READY: that is
    // not a view of the user's layout, and both of these persist — one stamps
    // the grid, the other can reseat every coordinate. Freezing either from
    // such an arrangement is the same mistake the write-back below refuses to
    // make; deferring costs nothing, because the next READY populate resolves
    // it cleanly. Both halves of the condition are load-bearing — see the
    // write-back for why klippy_state alone is not enough.
    lv_subject_t* conn_layout = lv_xml_get_subject(nullptr, "printer_connection_state");
    lv_subject_t* klippy_layout = lv_xml_get_subject(nullptr, "klippy_state");
    const bool layout_state_transient =
        conn_layout &&
        lv_subject_get_int(conn_layout) == static_cast<int>(ConnectionState::CONNECTED) &&
        klippy_layout && lv_subject_get_int(klippy_layout) != static_cast<int>(KlippyState::READY);
    if (layout_state_transient) {
        spdlog::debug("[PanelWidgetManager] '{}': deferring layout resolution — Klipper is not "
                      "READY, so this arrangement is transient",
                      panel_id);
    } else if (widget_config.has_pending_anchors()) {
        if (content_box_measured) {
            widget_config.apply_pending_anchors(grid_dims.cols, grid_dims.rows);
        } else {
            spdlog::warn("[PanelWidgetManager] '{}': deferring the default anchors until the "
                         "content box measures",
                         panel_id);
        }
    } else if (content_box_measured) {
        // Make this grid the active one. A saved layout is coordinates in
        // tracks, and the grid those tracks count against is no longer fixed
        // per device — the UI scale changes it on the same panel. Without this
        // the write-back below persists whatever THIS grid could seat over the
        // arrangement the user made on another one, and switching back finds
        // nothing to restore.
        //
        // Cheap on the common path: the signature already matches on every
        // rebuild after the first, so this returns without touching storage.
        widget_config.switch_to_grid(grid_dims.cols, grid_dims.rows);
    }

    // Build grid placement tracker to compute positions
    GridLayout grid(breakpoint, grid_dims);

    // Track geometry for the placement pass. The layout is activated much
    // further down, so this is the only way to know a span's pixel extent while
    // spans are still being decided; it comes from the same content box the
    // track counts did, so the two cannot disagree.
    const CellMetrics place_metrics = grid_cell_metrics(content_w, content_h, grid_dims.cols,
                                                        grid_dims.rows, GridLayout::gutter_px());

    // Hand the measured tracks to every tile BEFORE the placement pass asks
    // fits_at(). TileSizing's whole-cell floor must be measured against the
    // tracks this content box actually built: quantising to the nearest whole
    // cell can deliver a track smaller than the tier's GRID_CELL target
    // (31.25px against 34 at 480x272), and a tile left on the nominal target
    // rejects its authored span, grows, and lands on a later anchor.
    for (auto& slot : enabled_widgets) {
        if (slot.instance) {
            if (TileSizing* sizing = slot.instance->tile_sizing()) {
                sizing->set_cell_metrics(place_metrics);
            }
        }
    }

    // Correlate widget entries with config entries to get grid positions
    const auto& entries = widget_config.page_entries(page_index);

    // First pass: place widgets with explicit grid positions (anchors + user-positioned)
    struct PlacedSlot {
        size_t slot_index; // Index into enabled_widgets
        int col, row, colspan, rowspan;
        // The span it would be honest to persist — the authored (auto-placed) or
        // saved (anchored) span, as opposed to whatever this particular grid
        // could seat. A reduction is a property of the current screen, not of the
        // user's layout: writing it back would strand the widget at the portrait
        // width after rotating to landscape (#1216).
        int want_colspan = 1;
        int want_rowspan = 1;
    };
    std::vector<PlacedSlot> placed;
    std::vector<size_t> auto_place_indices; // Widgets needing dynamic placement

    for (size_t i = 0; i < enabled_widgets.size(); ++i) {
        auto& slot = enabled_widgets[i];

        auto entry_it =
            std::find_if(entries.begin(), entries.end(),
                         [&](const PanelWidgetEntry& e) { return e.id == slot.widget_id; });

        // enabled_widgets is built from enabled entries only, so this is a guard
        // on that invariant rather than a live filter. A disabled entry keeps
        // whatever col/row it last held, and anchoring on that cell would let it
        // outrank a user-anchored widget whose saved rectangle overlaps it.
        // Every GridEditMode occupancy loop filters on enabled for the same
        // reason.
        if (entry_it != entries.end() && entry_it->is_placed()) {
            // Clamp the SPAN to the grid before clamping the position. A span
            // saved on a 6-column landscape grid cannot exist on a 2-column
            // portrait one; leaving it unclamped made can_place() fail, dropped
            // the widget into auto-place, and ultimately disabled it (#1216).
            const auto [anchor_col_step, anchor_row_step] =
                GridEditMode::snap_step_for(slot.widget_id);
            const auto fitted =
                clamp_to_grid(entry_it->col, entry_it->row, entry_it->colspan, entry_it->rowspan,
                              grid.cols(), grid.rows(), anchor_col_step, anchor_row_step);
            int col = fitted.col;
            int row = fitted.row;
            int colspan = fitted.colspan;
            int rowspan = fitted.rowspan;

            // Reading fits_at here is safe only because of the ordering this
            // pass runs in, all within one tick: the previous tree is detached
            // and scheduled for deferred deletion but NOT yet freed, so an
            // instance still holding a root from the last pass points at live
            // memory, and attach() below replaces it before the free happens.
            // Making the tree teardown synchronous, or deferring this grow to a
            // later tick, would leave that root dangling.
            //
            // A span saved against a different panel, language or theme can be
            // smaller than this widget can draw. Grow the PLACEMENT request
            // only: want_colspan below stays the saved span, so the #1216 guard
            // keeps a size this panel forced out of serialize_pages(). Growing
            // before place() means a grown span that collides is refused here
            // and falls through to auto-place, as any other span would.
            if (slot.instance) {
                const auto* grow_def = find_widget_def(slot.widget_id);
                const auto [grown_c, grown_r] = grow_span_to_fit(
                    [&](int w, int h) { return slot.instance->fits_at(w, h); }, colspan, rowspan,
                    grow_def ? grow_def->effective_max_colspan() : colspan,
                    grow_def ? grow_def->effective_max_rowspan() : rowspan, anchor_col_step,
                    anchor_row_step, place_metrics);
                colspan = grown_c;
                rowspan = grown_r;
            }

            if (grid.place({slot.widget_id, col, row, colspan, rowspan})) {
                placed.push_back(
                    {i, col, row, colspan, rowspan, entry_it->colspan, entry_it->rowspan});
            } else {
                spdlog::warn("[PanelWidgetManager] Cannot place widget '{}' at ({},{} {}x{})",
                             slot.widget_id, col, row, colspan, rowspan);
                auto_place_indices.push_back(i); // Fall back to auto-place
            }
        } else {
            auto_place_indices.push_back(i);
        }
    }

    // Second pass: auto-place widgets without explicit positions.
    // Place multi-cell widgets first (they need contiguous space), then pack
    // 1×1 widgets into remaining cells bottom-right first.
    std::vector<size_t> multi_cell_indices;
    std::vector<size_t> single_cell_indices;
    for (size_t idx : auto_place_indices) {
        const auto* def = find_widget_def(enabled_widgets[idx].widget_id);
        int cs = def ? def->colspan : 1;
        int rs = def ? def->rowspan : 1;
        if (cs > 1 || rs > 1) {
            multi_cell_indices.push_back(idx);
        } else {
            single_cell_indices.push_back(idx);
        }
    }

    // Disable a widget that does not fit the grid AT ALL — it is wider or taller
    // than the whole grid even at its declared minimum, so no arrangement of the
    // other widgets could ever seat it. Sending it back to the catalog as an
    // available widget is the only outcome it has. Tell the user WHICH condition
    // failed: "grid full" is a lie here (#1216).
    //
    // GridFull is the other branch and is deliberately NOT routed here — see
    // evict_for_full_grid below.
    auto disable_unplaceable = [&](const std::string& widget_id,
                                   GridLayout::PlacementFailure reason) {
        auto& mut_entries = widget_config.page_entries_mut(page_index);
        auto cfg_it = std::find_if(mut_entries.begin(), mut_entries.end(),
                                   [&](const PanelWidgetEntry& e) { return e.id == widget_id; });
        if (cfg_it != mut_entries.end()) {
            cfg_it->disable_and_unplace();
        }
        const char* why = GridLayout::failure_text(reason);
        spdlog::info("[PanelWidgetManager] Disabled widget '{}' — {}", widget_id, why);
        const auto* def = find_widget_def(widget_id);
        const char* name = def ? def->display_name : widget_id.c_str();
        ui_notification_warning(fmt::format("'{}' removed — {}", name, why).c_str());
    };

    // Drop a widget that fits the grid fine but has no free cell left. Unlike
    // TooLargeForGrid this is a property of THIS screen's occupancy, not of the
    // widget: remove any other widget, close a hardware gate, or lay the same
    // config out on a taller grid and it seats without complaint.
    //
    // So it must never write enabled=false. The layout is stored once per printer
    // (/printers/<id>/panel_widgets/<panel>) with no breakpoint key, so a disable
    // forced by one screen's occupancy takes the widget away at EVERY size — the
    // same mistake the span write-back already refuses to make (#1216). Worse, it
    // was not even deterministic: the disable only reached disk if some unrelated
    // save() happened to follow, so whether the user permanently lost the widget
    // depended on what they did next.
    //
    // What IS recorded is that the widget has no position. That is the truth (it
    // is configured, it just has nowhere to go), it lets the widget re-place
    // itself the moment a cell frees, and it doubles as the memo that stops the
    // nagging: a widget with no saved position was never on the user's screen, so
    // announcing a removal would be false. Bundle XGVDYEB5 — 6x4 grid, ten
    // widgets filling all 24 cells — toasted "'Fan Speeds' removed — grid full"
    // on every single launch because the in-memory disable never reached disk.
    bool evicted_position = false;
    auto evict_for_full_grid = [&](const std::string& widget_id) {
        auto& mut_entries = widget_config.page_entries_mut(page_index);
        auto cfg_it = std::find_if(mut_entries.begin(), mut_entries.end(),
                                   [&](const PanelWidgetEntry& e) { return e.id == widget_id; });
        const bool was_on_screen = cfg_it != mut_entries.end() && cfg_it->has_grid_position();
        const char* why = GridLayout::failure_text(GridLayout::PlacementFailure::GridFull);

        if (!was_on_screen) {
            spdlog::debug("[PanelWidgetManager] Widget '{}' has no cell — {} (stays enabled)",
                          widget_id, why);
            return;
        }

        cfg_it->col = -1;
        cfg_it->row = -1;
        evicted_position = true;
        spdlog::info("[PanelWidgetManager] Evicted widget '{}' — {} (stays enabled; returns when "
                     "a cell frees)",
                     widget_id, why);
        const auto* def = find_widget_def(widget_id);
        const char* name = def ? def->display_name : widget_id.c_str();
        ui_notification_warning(fmt::format("'{}' removed — {}", name, why).c_str());
    };

    // ---- Auto-placement -----------------------------------------------------
    //
    // Two span policies, tried in order.
    //
    //  1. AUTHORED — every widget asks for the span its registry definition
    //     declares. When they all fit, this is the layout the dashboard was
    //     designed around, so a roomy grid ends up exactly where it always did.
    //  2. MINIMUM-FIRST — used only when (1) cannot seat everyone. Every widget
    //     is placed at its declared MINIMUM, which maximises how many widgets
    //     survive, and the cells left over are handed back out by growing each
    //     widget toward its authored span (GridLayout::grow_to_targets).
    //
    // The old policy asked for the largest span that fit and stepped down from
    // there. On a 3-column portrait grid that let `tips` take a reduced 3x2 — 6
    // of 18 cells — and fan_stack, ams and notifications were then all disabled
    // with "grid full": three widgets lost where main lost one (#1216).
    const auto anchored_placements = grid.placements();
    const size_t anchored_slots = placed.size();

    struct AutoFailure {
        std::string widget_id;
        GridLayout::PlacementFailure reason;
    };

    auto run_auto_pass = [&](bool minimum_first) {
        std::vector<AutoFailure> failures;

        // Rewind to the anchored widgets. Anchors hold an explicit position the
        // user (or the shipped default layout) chose, so they are never re-spanned
        // by either policy.
        grid.clear();
        for (const auto& p : anchored_placements) {
            grid.place(p);
        }
        placed.resize(anchored_slots);

        std::vector<GridLayout::GrowthTarget> growth;

        // Multi-cell widgets first — they need contiguous space.
        for (size_t slot_idx : multi_cell_indices) {
            auto& slot = enabled_widgets[slot_idx];
            const auto* def = find_widget_def(slot.widget_id);
            const int want_cols = def ? std::max(1, def->colspan) : 1;
            const int want_rows = def ? std::max(1, def->rowspan) : 1;
            // Growth stops at the authored span, and never past the declared
            // maximum: a definition whose max sits below its default is a bug in
            // the table, not a licence to overflow.
            const int grow_cols = def ? std::min(want_cols, def->effective_max_colspan()) : 1;
            const int grow_rows = def ? std::min(want_rows, def->effective_max_rowspan()) : 1;
            const int min_cols = def ? std::min(def->effective_min_colspan(), want_cols) : 1;
            const int min_rows = def ? std::min(def->effective_min_rowspan(), want_rows) : 1;
            // These are registry spans, not a fits_at answer: auto-place seats
            // at the authored default, which every definition holds at or above
            // its own minimum. A widget whose fits_at declines its authored
            // default would be seated unmeasured here, where the anchored path
            // above would have grown it. No definition is shaped that way; one
            // that were would need this path to consult grow_span_to_fit too.
            // The boundaries this widget may sit on — a whole cell unless it
            // declares half-cell support. Same source edit mode snaps drags and
            // resizes to, so the two paths cannot disagree (#1126).
            const auto [col_step, row_step] = GridEditMode::snap_step_for(slot.widget_id);

            auto fit =
                minimum_first
                    ? grid.find_available_bottom_min(min_cols, min_rows, col_step, row_step)
                    : grid.find_available_bottom_min(want_cols, want_rows, col_step, row_step);

            if (fit.placed() &&
                grid.place({slot.widget_id, fit.col, fit.row, fit.colspan, fit.rowspan})) {
                placed.push_back(
                    {slot_idx, fit.col, fit.row, fit.colspan, fit.rowspan, want_cols, want_rows});
                if (minimum_first) {
                    growth.push_back({slot.widget_id, grow_cols, grow_rows, col_step, row_step});
                }
            } else {
                // A found-but-unplaceable position can only mean the free run
                // vanished under us; report that, not a stale "None".
                failures.push_back({slot.widget_id, fit.placed()
                                                        ? GridLayout::PlacementFailure::GridFull
                                                        : fit.failure});
            }
        }

        // Pack one-cell widgets into the remaining free cells, bottom-right
        // first. Spans here are in TRACKS, so one cell is TRACKS_PER_CELL of
        // them on each axis — a literal 1 would hand out a quarter of a cell.
        // Only an id with no registry definition reaches this block: every real
        // definition spans at least one whole cell, so the classification above
        // routes them all through the multi-cell path.
        {
            constexpr int kCell = GridLayout::TRACKS_PER_CELL;
            std::vector<std::pair<int, int>> free_cells;
            for (int r = grid.rows() - kCell; r >= 0; r -= kCell) {
                for (int c = grid.cols() - kCell; c >= 0; c -= kCell) {
                    if (grid.can_place(c, r, kCell, kCell)) {
                        free_cells.push_back({c, r});
                    }
                }
            }

            // Map: last widget -> bottom-right cell, first -> top-left of the block
            size_t n_single = single_cell_indices.size();
            size_t n_cells = free_cells.size();
            for (size_t i = 0; i < n_single; ++i) {
                size_t slot_idx = single_cell_indices[i];
                auto& slot = enabled_widgets[slot_idx];

                size_t cell_idx = n_single - 1 - i;
                if (cell_idx < n_cells) {
                    auto [col, row] = free_cells[cell_idx];
                    if (grid.place({slot.widget_id, col, row, kCell, kCell})) {
                        placed.push_back({slot_idx, col, row, kCell, kCell, kCell, kCell});
                        continue;
                    }
                }

                // Fallback
                auto pos = grid.find_available_bottom(kCell, kCell);
                if (pos && grid.place({slot.widget_id, pos->first, pos->second, kCell, kCell})) {
                    placed.push_back(
                        {slot_idx, pos->first, pos->second, kCell, kCell, kCell, kCell});
                } else {
                    // A one-cell widget fits any grid by definition — MIN_TRACKS
                    // is a whole number of cells — so the only way to get here
                    // is that every cell is taken.
                    failures.push_back({slot.widget_id, GridLayout::PlacementFailure::GridFull});
                }
            }
        }

        // Hand the leftover cells back out, then re-read the grid: growth moves
        // origins as well as spans, and `placed` is what actually builds the UI.
        if (minimum_first && !growth.empty() && grid.grow_to_targets(growth) > 0) {
            for (auto& p : placed) {
                if (const auto* gp = grid.find_placement(enabled_widgets[p.slot_index].widget_id)) {
                    p.col = gp->col;
                    p.row = gp->row;
                    p.colspan = gp->colspan;
                    p.rowspan = gp->rowspan;
                }
            }
        }
        return failures;
    };

    auto failures = run_auto_pass(/*minimum_first=*/false);
    if (!failures.empty()) {
        spdlog::debug("[PanelWidgetManager] {} widget(s) do not fit at their authored span on a "
                      "{}x{} grid — retrying minimum-first",
                      failures.size(), grid.cols(), grid.rows());
        failures = run_auto_pass(/*minimum_first=*/true);
    }

    for (const auto& f : failures) {
        if (f.reason == GridLayout::PlacementFailure::GridFull) {
            evict_for_full_grid(f.widget_id);
        } else {
            disable_unplaceable(f.widget_id, f.reason);
        }
    }

    for (const auto& p : placed) {
        if (p.colspan != p.want_colspan || p.rowspan != p.want_rowspan) {
            spdlog::debug("[PanelWidgetManager] Widget '{}' placed at span {}x{} "
                          "(wanted {}x{}, grid is {}x{})",
                          enabled_widgets[p.slot_index].widget_id, p.colspan, p.rowspan,
                          p.want_colspan, p.want_rowspan, grid.cols(), grid.rows());
        }
    }

    // Write computed positions back to config entries and persist to disk, so
    // auto-placed positions survive the next load() call (get_widget_config
    // reloads from the JSON store after mark_dirty). Only widgets that are
    // enabled in config get a position written.
    //
    // Never persist a layout computed while a CONNECTED printer reports Klipper
    // not READY. A populate in that state is not a view of the user's intended
    // arrangement, and freezing it to disk is what makes tiles revert to a
    // previous layout after a power cycle or a FIRMWARE_RESTART. The widgets for
    // THIS frame are placed from `placed` regardless, so skipping the write-back
    // only defers auto-placed positions to the next READY populate, which
    // re-derives and persists them.
    //
    // The connection half is load-bearing: klippy_state reads SHUTDOWN before
    // Moonraker has reported anything, so gating on it alone would also refuse
    // the first write-back of every launch, and an auto-placed layout would
    // never reach disk at all.
    lv_subject_t* conn_now = lv_xml_get_subject(nullptr, "printer_connection_state");
    lv_subject_t* klippy_now = lv_xml_get_subject(nullptr, "klippy_state");
    const bool connected =
        conn_now && lv_subject_get_int(conn_now) == static_cast<int>(ConnectionState::CONNECTED);
    const bool klippy_not_ready =
        klippy_now && lv_subject_get_int(klippy_now) != static_cast<int>(KlippyState::READY);
    if (!(connected && klippy_not_ready)) {
        auto& mut_entries = widget_config.page_entries_mut(page_index);
        bool any_written = false;
        for (const auto& p : placed) {
            auto& slot = enabled_widgets[p.slot_index];
            auto entry_it =
                std::find_if(mut_entries.begin(), mut_entries.end(),
                             [&](const PanelWidgetEntry& e) { return e.id == slot.widget_id; });
            if (entry_it != mut_entries.end() && entry_it->enabled) {
                if (entry_it->col != p.col || entry_it->row != p.row) {
                    any_written = true;
                }
                entry_it->col = p.col;
                entry_it->row = p.row;
                // Never persist a span that was cut down just to fit THIS grid.
                // The reduction is a property of the current screen, not of the
                // user's layout; writing it back would strand the widget at the
                // portrait width after rotating to landscape (#1216).
                if (p.colspan == p.want_colspan && p.rowspan == p.want_rowspan) {
                    entry_it->colspan = p.colspan;
                    entry_it->rowspan = p.rowspan;
                }
            }
        }
        // `evicted_position` is the other reason to save: a widget that lost its
        // cell this pass changed nothing about the widgets that WERE placed, so
        // any_written stays false and the eviction would never reach disk — which
        // is exactly how the "grid full" toast came back on every launch.
        if (any_written || evicted_position) {
            widget_config.save();
        }
    }

    // Row track count comes from the grid, exactly like the column count. The
    // grid's tracks are square by construction, so a page that uses only the
    // top half of them leaves the bottom half empty rather than stretching
    // every widget to fill the height.
    const int cols = grid_dims.cols;
    const int grid_rows = grid_dims.rows;

    spdlog::debug(
        "[PanelWidgetManager] Grid layout: {}cols x {}rows (bp={}, content {}x{}) for '{}'", cols,
        grid_rows, to_int(breakpoint), content_w, content_h, panel_id);

    // Generate grid descriptors sized to actual content
    // Columns: use breakpoint column count (fills available width)
    // Rows: use max of current and cached row count for stable sizing
    // Built as a local and handed to the container's slot at install time
    // (near the end of this populate) — that handoff owns the arrays' lifetime.
    GridDescriptors dsc;
    dsc.col_dsc = GridLayout::make_col_dsc(cols);
    dsc.row_dsc = GridLayout::make_row_dsc(grid_rows);

    // Configure grid padding now, but DEFER activating LV_LAYOUT_GRID and
    // installing the grid descriptor array until all children have been created
    // and attached (see the grid activation near the end of this function).
    // lv_obj_set_grid_dsc_array() internally calls lv_obj_set_style_layout(...,
    // LV_LAYOUT_GRID), so it cannot run here without turning the container into a
    // live grid. Children are placed with lv_obj_set_grid_cell() — style
    // properties that simply take effect once the layout becomes grid — so
    // per-cell placement can be set up below without the layout being active.
    // Activating grid before the children exist lets any widget whose attach()
    // synchronously triggers lv_obj_update_layout (e.g. PrintStatusWidget ->
    // lv_image_set_src -> update_align, see print_status_widget.cpp:331) cascade
    // a grid_update over a half-built grid, which crashes (#983).
    const int gutter = GridLayout::gutter_px();
    lv_obj_set_style_pad_column(container, gutter, 0);
    lv_obj_set_style_pad_row(container, gutter, 0);

    // Compute cell pixel dimensions for size callbacks and card backgrounds.
    // Same derivation the placement pass used, not a second one: the promise a
    // widget is sized against and the fits_at() answers that seated it must
    // come from one CellMetrics.
    const CellMetrics& metrics = place_metrics;
    spdlog::debug("[PanelWidgetManager] Track geometry: {:.2f}x{:.2f}px, gutter {}px",
                  metrics.cell_w, metrics.cell_h, gutter);

    // Card backgrounds behind adjacent widgets that ask for one, from ALL
    // enabled config entries (not just currently-placed ones) so that cards for
    // hardware-gated widgets appear from the first frame, preventing the grid
    // from visually jumping when hardware gates fire.
    {
        // Where each widget ACTUALLY landed, which is not always where its entry
        // asks for. Auto-placement and span reduction both move a widget without
        // touching the saved entry, so the authored position is a request, not a
        // result. Keyed by id because slot_index indexes enabled_widgets, and
        // this loop walks config entries.
        std::unordered_map<std::string, const PlacedSlot*> placed_by_id;
        for (const auto& p : placed) {
            if (p.slot_index < enabled_widgets.size()) {
                placed_by_id[enabled_widgets[p.slot_index].widget_id] = &p;
            }
        }

        std::vector<CardFootprint> footprints;
        for (const auto& entry : widget_config.page_entries(page_index)) {
            if (!entry.is_placed()) {
                continue;
            }
            // Prefer where the widget actually landed; fall back to the authored
            // placement only when it was not placed at all.
            //
            // The authored entry is a request, not a result: auto-placement and
            // span reduction both move a widget without touching its saved
            // entry. Trusting the entry marks cells the widget does not cover,
            // and those cells are then subtracted from a neighbour's card,
            // costing that neighbour its background. That is what cost two
            // widgets their background beside an unanchored `tips` at 480x800.
            // The fewer tracks a grid has, the more often placement has to move
            // something, so this is routine on cramped and high-DPI-scaled
            // layouts rather than rare.
            //
            // The fallback matters: this walks ALL enabled entries, not just
            // placed ones, so a hardware-gated widget still reserves its card on
            // the first frame instead of making the grid jump once its hardware
            // appears. An entry with no placed geometry is still only a request,
            // so it goes through the same snap steps.
            const auto* placed_slot = [&]() -> const PlacedSlot* {
                auto it = placed_by_id.find(entry.id);
                return it != placed_by_id.end() ? it->second : nullptr;
            }();
            const auto [card_col_step, card_row_step] = GridEditMode::snap_step_for(entry.id);
            const auto fitted =
                placed_slot ? clamp_to_grid(placed_slot->col, placed_slot->row,
                                            placed_slot->colspan, placed_slot->rowspan, grid.cols(),
                                            grid.rows(), card_col_step, card_row_step)
                            : clamp_to_grid(entry.col, entry.row, entry.colspan, entry.rowspan,
                                            grid.cols(), grid.rows(), card_col_step, card_row_step);

            // Whether a widget wants the shared card is a property of the
            // widget, not of its size — `merges_into_card` in the registry. It
            // used to be inferred from "is this exactly one cell", which
            // conflated wanting a card with being small and was wrong both
            // ways: a 2x1 fan_stack paints nothing of its own and got no
            // background, while a one-cell ams brings its own and got two.
            //
            // Geometry no longer has a say. In tracks a widget's footprint is
            // exact whatever its size or alignment, so the flag alone decides:
            // a widget that opts out blocks merges through the tracks it
            // touches and is on its own for a background.
            const auto* def = find_widget_def(entry.id);
            footprints.push_back({fitted.col, fitted.row, fitted.colspan, fitted.rowspan,
                                  def && def->merges_into_card});
        }

        for (const CardRect& card : card_background_rects(footprints)) {
            create_card_background(container, card, metrics);
        }
    }

    // Second pass: create XML components and place in grid cells
    std::vector<std::unique_ptr<PanelWidget>> result;

    for (const auto& p : placed) {
#if defined(__cpp_exceptions)
        try {
#else
        {
#endif
            create_tile(container, enabled_widgets[p.slot_index],
                        TileCell{p.col, p.row, p.colspan, p.rowspan}, metrics, result);
#if defined(__cpp_exceptions)
        } catch (const std::exception& e) {
            spdlog::error("[PanelWidgetManager] Widget '{}' creation failed: {}",
                          enabled_widgets[p.slot_index].widget_id, e.what());
        }
#else
        }
#endif
    }

    spdlog::debug("[PanelWidgetManager] Populated {} widgets ({} with factories) via grid for '{}'",
                  placed.size(), result.size(), panel_id);

    // All children (card backgrounds + widgets) now exist and carry their
    // per-cell grid placement. Install the grid descriptor + activate the grid
    // layout last so the very first grid_update runs over a complete, valid grid
    // in a single clean pass — never over a half-built one (#983).
    // install_grid_descriptors stores the arrays in the container's
    // grid_descriptors_ slot (re-pointing the style and only then freeing the
    // previous generation), so they remain valid for the lifetime of the grid.
    // lv_obj_set_grid_dsc_array() is what turns the container into a live grid;
    // lv_obj_set_layout() is belt-and-suspenders. The explicit update_layout
    // forces that pass now and re-flows widgets whose attach() read a pre-grid
    // size.
    install_grid_descriptors(container, std::move(dsc));
    lv_obj_set_layout(container, LV_LAYOUT_GRID);
    const auto t_layout = std::chrono::steady_clock::now();
    lv_obj_update_layout(container);
    log_if_slow_build("(grid layout)", t_layout, t_layout);

    populating_ = false;
    return result;
}

std::optional<std::vector<PanelWidgetManager::GateFlip>>
PanelWidgetManager::gate_flips_only(const std::vector<std::string>& before,
                                    const std::vector<std::string>& after) {
    if (before.size() != after.size()) {
        return std::nullopt;
    }
    const std::string_view suffix = GATED_ID_SUFFIX;
    auto split = [suffix](std::string_view id) {
        const bool gated =
            id.size() >= suffix.size() && id.substr(id.size() - suffix.size()) == suffix;
        if (gated) {
            id.remove_suffix(suffix.size());
        }
        return std::make_pair(id, gated);
    };
    std::vector<GateFlip> flips;
    for (size_t i = 0; i < before.size(); ++i) {
        const auto b = split(before[i]);
        const auto a = split(after[i]);
        if (b.first != a.first) {
            return std::nullopt;
        }
        if (b.second != a.second) {
            flips.push_back({i, a.second});
        }
    }
    return flips;
}

std::optional<std::vector<PanelWidget*>>
PanelWidgetManager::swap_gated_tiles(const std::string& panel_id, lv_obj_t* container,
                                     int page_index, const std::vector<std::string>& visible_ids,
                                     const std::vector<GateFlip>& flips,
                                     std::vector<std::unique_ptr<PanelWidget>>& widgets) {
    if (!container || populating_) {
        return std::nullopt;
    }
    const int cols =
        grid_count_tracks(lv_obj_get_style_grid_column_dsc_array(container, LV_PART_MAIN));
    const int rows =
        grid_count_tracks(lv_obj_get_style_grid_row_dsc_array(container, LV_PART_MAIN));
    if (cols <= 0 || rows <= 0) {
        return std::nullopt;
    }

    // Locate every flipped tile before touching any, so a miss changes nothing.
    struct Target {
        const PanelWidgetEntry* entry;
        lv_obj_t* tile;
        TileCell cell;
        uint32_t index;
    };
    std::vector<Target> targets;
    const auto& entries = get_widget_config(panel_id).page_entries(page_index);
    for (const auto& flip : flips) {
        if (flip.index >= visible_ids.size()) {
            return std::nullopt;
        }
        std::string id = visible_ids[flip.index];
        if (flip.now_gated) {
            id.resize(id.size() - std::strlen(GATED_ID_SUFFIX));
        }
        auto entry = std::find_if(entries.begin(), entries.end(),
                                  [&id](const auto& e) { return e.enabled && e.id == id; });
        if (entry == entries.end()) {
            return std::nullopt;
        }
        lv_obj_t* tile = nullptr;
        const uint32_t count = lv_obj_get_child_count(container);
        for (uint32_t i = 0; i < count && !tile; ++i) {
            lv_obj_t* child = lv_obj_get_child(container, static_cast<int32_t>(i));
            const char* name = lv_obj_get_name(child);
            if (lv_obj_has_flag(child, PANEL_WIDGET_TILE_FLAG) && name && id == name) {
                tile = child;
            }
        }
        if (!tile) {
            return std::nullopt;
        }
        targets.push_back({&*entry, tile,
                           TileCell{lv_obj_get_style_grid_cell_column_pos(tile, LV_PART_MAIN),
                                    lv_obj_get_style_grid_cell_row_pos(tile, LV_PART_MAIN),
                                    lv_obj_get_style_grid_cell_column_span(tile, LV_PART_MAIN),
                                    lv_obj_get_style_grid_cell_row_span(tile, LV_PART_MAIN)},
                           static_cast<uint32_t>(lv_obj_get_index(tile))});
    }

    const CellMetrics metrics =
        grid_cell_metrics(lv_obj_get_content_width(container), lv_obj_get_content_height(container),
                          cols, rows, GridLayout::gutter_px());
    populating_ = true;
    std::vector<std::unique_ptr<PanelWidget>> attached;
    for (auto& t : targets) {
        auto inst = std::find_if(widgets.begin(), widgets.end(),
                                 [&t](const auto& w) { return w && t.entry->id == w->id(); });
        if (inst != widgets.end()) {
            (*inst)->detach_tile();
            widgets.erase(inst);
        }
        helix::ui::safe_delete_deferred(t.tile);

        WidgetReuseMap no_reuse;
        auto slot = resolve_slot(panel_id, *t.entry, no_reuse);
        if (!slot) {
            continue;
        }
        if (slot->instance) {
            if (TileSizing* sizing = slot->instance->tile_sizing()) {
                sizing->set_cell_metrics(metrics);
            }
        }
#if defined(__cpp_exceptions)
        try {
#else
        {
#endif
            if (lv_obj_t* tile = create_tile(container, *slot, t.cell, metrics, attached)) {
                lv_obj_move_to_index(tile, static_cast<int32_t>(t.index));
            }
#if defined(__cpp_exceptions)
        } catch (const std::exception& e) {
            spdlog::error("[PanelWidgetManager] Widget '{}' creation failed: {}", t.entry->id,
                          e.what());
        }
#else
        }
#endif
    }
    active_configs_[make_cache_key(panel_id, page_index)] = ActiveWidgetConfig{visible_ids};
    lv_obj_update_layout(container);
    populating_ = false;

    std::vector<PanelWidget*> fresh;
    for (auto& w : attached) {
        fresh.push_back(w.get());
        widgets.push_back(std::move(w));
    }
    spdlog::debug("[PanelWidgetManager] Swapped {} gate-flipped tile(s) in place for '{}:{}'",
                  targets.size(), panel_id, page_index);
    return fresh;
}

bool PanelWidgetManager::relayout_tiles(const std::string& panel_id, lv_obj_t* container,
                                        int page_index, const std::vector<std::string>& changed_ids,
                                        const std::string& resized_id,
                                        std::vector<std::unique_ptr<PanelWidget>>& widgets) {
    if (!container || populating_) {
        return false;
    }
    const int cols =
        grid_count_tracks(lv_obj_get_style_grid_column_dsc_array(container, LV_PART_MAIN));
    const int rows =
        grid_count_tracks(lv_obj_get_style_grid_row_dsc_array(container, LV_PART_MAIN));
    if (cols <= 0 || rows <= 0) {
        return false;
    }
    const CellMetrics metrics =
        grid_cell_metrics(lv_obj_get_content_width(container), lv_obj_get_content_height(container),
                          cols, rows, GridLayout::gutter_px());
    const auto& entries = get_widget_config(panel_id).page_entries(page_index);

    // An entry's cell as a populate seats it.
    auto entry_cell = [&](const PanelWidgetEntry& e) {
        const auto [col_step, row_step] = GridEditMode::snap_step_for(e.id);
        const auto f =
            clamp_to_grid(e.col, e.row, e.colspan, e.rowspan, cols, rows, col_step, row_step);
        return TileCell{f.col, f.row, f.colspan, f.rowspan};
    };

    // Check every tile before touching any, so a refusal changes nothing.
    struct Seat {
        lv_obj_t* tile;
        const PanelWidgetEntry* entry;
        TileCell cell;
    };
    std::vector<Seat> seats;
    std::vector<lv_obj_t*> cards;
    const Seat* resized = nullptr;
    const uint32_t count = lv_obj_get_child_count(container);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* child = lv_obj_get_child(container, static_cast<int32_t>(i));
        // A hidden child is condemned: safe_delete_deferred() hides what it
        // queues for deletion.
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        const char* name = lv_obj_get_name(child);
        if (!lv_obj_has_flag(child, PANEL_WIDGET_TILE_FLAG)) {
            // Card backgrounds are the unnamed grid children; edit mode's own
            // objects float.
            if (!name && !lv_obj_has_flag(child, LV_OBJ_FLAG_FLOATING)) {
                cards.push_back(child);
            }
            continue;
        }
        auto entry = std::find_if(entries.begin(), entries.end(), [name](const auto& e) {
            return name && e.enabled && e.id == name;
        });
        if (entry == entries.end() || !entry->is_placed()) {
            return false;
        }
        // A tile the edit did not touch must already sit where its entry says.
        // Populate can seat one elsewhere (auto-placement, a span reduced or
        // grown for this grid) and does not always write that back, and moving
        // it to its entry here could land it on another tile.
        const TileCell cell = entry_cell(*entry);
        const bool changed =
            std::find(changed_ids.begin(), changed_ids.end(), entry->id) != changed_ids.end();
        if (!changed &&
            (lv_obj_get_style_grid_cell_column_pos(child, LV_PART_MAIN) != cell.col ||
             lv_obj_get_style_grid_cell_row_pos(child, LV_PART_MAIN) != cell.row ||
             lv_obj_get_style_grid_cell_column_span(child, LV_PART_MAIN) != cell.colspan ||
             lv_obj_get_style_grid_cell_row_span(child, LV_PART_MAIN) != cell.rowspan)) {
            return false;
        }
        seats.push_back({child, &*entry, cell});
    }
    for (const auto& seat : seats) {
        if (!resized_id.empty() && seat.entry->id == resized_id) {
            resized = &seat;
        }
    }
    if (!resized_id.empty() && !resized) {
        return false;
    }
    auto instance = std::find_if(widgets.begin(), widgets.end(),
                                 [&](const auto& w) { return w && resized_id == w->id(); });
    if (resized && instance != widgets.end() &&
        !(*instance)->fits_at(static_cast<int>(grid_track_extent(metrics.cell_w, metrics.gutter,
                                                                 resized->cell.colspan)),
                              static_cast<int>(grid_track_extent(metrics.cell_h, metrics.gutter,
                                                                 resized->cell.rowspan)))) {
        return false;
    }

    populating_ = true;
    for (const auto& seat : seats) {
        lv_obj_set_grid_cell(seat.tile, LV_GRID_ALIGN_STRETCH, seat.cell.col, seat.cell.colspan,
                             LV_GRID_ALIGN_STRETCH, seat.cell.row, seat.cell.rowspan);
    }

    // The resized tile keeps its tree: what a widget builds depends on its
    // config, never its span, so the new size reaches it the way a populate's
    // does, through on_size_changed().
    if (resized) {
        announce_tile_size(resized->tile, resized_id,
                           instance != widgets.end() ? instance->get() : nullptr, resized->cell,
                           metrics);
    }

    // Cards: keep the ones the new arrangement still has, replace the rest.
    std::vector<CardFootprint> footprints;
    for (const auto& e : entries) {
        if (e.is_placed()) {
            const TileCell c = entry_cell(e);
            const auto* def = find_widget_def(e.id);
            footprints.push_back(
                {c.col, c.row, c.colspan, c.rowspan, def && def->merges_into_card});
        }
    }
    std::vector<CardRect> wanted = card_background_rects(footprints);
    for (lv_obj_t* card : cards) {
        const CardRect have{lv_obj_get_style_grid_cell_column_pos(card, LV_PART_MAIN),
                            lv_obj_get_style_grid_cell_row_pos(card, LV_PART_MAIN),
                            lv_obj_get_style_grid_cell_column_span(card, LV_PART_MAIN),
                            lv_obj_get_style_grid_cell_row_span(card, LV_PART_MAIN)};
        auto kept = std::find(wanted.begin(), wanted.end(), have);
        if (kept != wanted.end()) {
            wanted.erase(kept);
        } else {
            helix::ui::safe_delete_deferred(card);
        }
    }
    for (const CardRect& card : wanted) {
        lv_obj_move_to_index(create_card_background(container, card, metrics), 0);
    }

    lv_obj_update_layout(container);
    populating_ = false;

    spdlog::debug("[PanelWidgetManager] Relaid out {} tile(s) in place for '{}:{}'{}", seats.size(),
                  panel_id, page_index, resized ? " (one resized)" : "");
    return true;
}

std::vector<std::string> PanelWidgetManager::compute_visible_widget_ids(const std::string& panel_id,
                                                                        int page_index) {
    auto& widget_config = get_widget_config(panel_id);
    std::vector<std::string> ids;

    for (const auto& entry : widget_config.page_entries(page_index)) {
        if (!entry.enabled) {
            continue;
        }
        // Include gate status in the ID so rebuild detects gated→ungated transitions
        bool gated = false;
        gate_hint_if_gated(find_widget_def(entry.id), gated);
        ids.push_back(visible_id(entry.id, gated));
    }

    return ids;
}

// Hardware-gate subjects live in several owners, so the death signal is
// resolved per gate name. A def whose subject has no entry here is left
// unobserved with a warning: handing observe_*() a token from the wrong
// owner would read as defended while defending nothing. Map the owner here
// when adding a gated widget.
SubjectLifetime gate_subject_lifetime(const char* name) {
    if (std::strcmp(name, "ams_slot_count") == 0 || std::strcmp(name, "ams_supports_bypass") == 0 ||
        std::strcmp(name, "clog_meter_mode") == 0 || std::strcmp(name, "buffer_present") == 0) {
        return AmsState::instance().get_subjects_lifetime();
    }
    if (std::strcmp(name, "filament_sensor_count") == 0) {
        return FilamentSensorManager::instance().get_subjects_lifetime();
    }
    if (std::strcmp(name, "humidity_sensor_count") == 0) {
        return sensors::HumiditySensorManager::instance().get_subjects_lifetime();
    }
    if (std::strcmp(name, "temp_sensor_count") == 0) {
        return sensors::TemperatureSensorManager::instance().get_subjects_lifetime();
    }
    if (std::strcmp(name, "width_sensor_count") == 0) {
        return sensors::WidthSensorManager::instance().get_subjects_lifetime();
    }
    if (std::strcmp(name, "led_controllable") == 0 || std::strcmp(name, "led_has_devices") == 0) {
        return led::LedController::instance().get_subjects_lifetime();
    }
    if (std::strcmp(name, "platform_host_power_supported") == 0) {
        return get_app_globals_subjects_lifetime();
    }
    if (std::strcmp(name, "power_device_count") == 0 ||
        std::strcmp(name, "printer_has_chamber") == 0) {
        return get_printer_state().get_subjects_lifetime();
    }
    return nullptr;
}

void PanelWidgetManager::setup_gate_observers(const std::string& panel_id,
                                              RebuildCallback rebuild_cb) {
    using helix::ui::observe;

    gate_observers_.erase(panel_id);
    auto& observers = gate_observers_[panel_id];

    // Walk the registry and observe every distinct hardware_gate_subject —
    // these are the same names compute_visible_widget_ids consults, so this
    // automatically tracks any new gated widget added in the future.
    //
    // Each observer schedules a coalesced rebuild via lv_async_call:
    //   * The first gate firing in a tick sets rebuild_pending_[panel_id]=true
    //     and queues ONE async rebuild. Subsequent firings in the same tick
    //     see the flag and skip — the queued rebuild will see all their values
    //     when it runs.
    //   * The async rebuild clears the flag at the start, so any gate firing
    //     AFTER the rebuild begins (e.g. a late-arriving capability subject)
    //     re-queues another rebuild on the next tick.
    //   * Without coalescing, N back-to-back firings produced N populate_page
    //     calls in the same UpdateQueue tick. Each call ran safe_clean_children,
    //     queuing async deletes for that pass's children. The accumulated
    //     N×children async-delete backlog then corrupted LVGL's event list
    //     during processing, crashing inside unsubscribe_on_delete_cb on
    //     resource-constrained MIPS hardware (AD5X bundles XG9QJ3V9, PFEHDEXF —
    //     L081 family).
    //   * lv_async_call escapes the UpdateQueue batch and runs on LVGL's own
    //     async list, so the deferred rebuild is not in the same batch as
    //     the gate-observer callbacks that scheduled it. This mirrors the
    //     "safe escape routes" pattern documented in CLAUDE.md.
    //   * The 2-second coalesce timer this replaced was a timing guess that
    //     fired before late-arriving capability subjects landed (e.g.
    //     printer_has_led on a busy Voron arrives 3-5s into discovery), then
    //     skipped because the cached list still showed "~gated". This
    //     async-coalesce pattern combines the correctness of direct dispatch
    //     with safety against backlog corruption.
    // Stable per-panel slot: lives in the manager singleton so its address
    // outlives any individual gate-observer registration. The slot is the
    // user-data passed to lv_async_call; clear_gate_observers calls
    // lv_async_call_cancel against it before erasing.
    GateRebuildSlot& slot = gate_rebuild_slots_[panel_id];
    slot.mgr = this;
    slot.panel_id = panel_id;
    slot.pending = false;
    gate_rebuild_callbacks_[panel_id] = std::move(rebuild_cb);
    // Cancel any rebuild that might still be queued from a previous registration
    // for this panel_id (e.g. soft-restart re-registers the home panel).
    lv_async_call_cancel(&PanelWidgetManager::gate_rebuild_trampoline, &slot);

    std::vector<const char*> gate_names;
    for (const auto& def : get_all_widget_defs()) {
        if (!def.hardware_gate_subject)
            continue;
        bool dup = false;
        for (const auto* n : gate_names) {
            if (std::strcmp(n, def.hardware_gate_subject) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup)
            gate_names.push_back(def.hardware_gate_subject);
    }

    for (const char* name : gate_names) {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
        if (!subject) {
            spdlog::trace("[PanelWidgetManager] Gate subject '{}' not registered yet", name);
            continue;
        }
        // ponytail: a hand-maintained name->owner table, replace with a lifetime
        // carried by the subject registry if gate names start churning
        SubjectLifetime gate_lifetime = gate_subject_lifetime(name);
        if (!gate_lifetime) {
            spdlog::warn(
                "[PanelWidgetManager] Gate subject '{}' has no owner lifetime mapped; "
                "gate changes will not rebuild until it is added to gate_subject_lifetime()",
                name);
            continue;
        }
        // Capture panel_id by value into the lambda so the async rebuild
        // can find the right rebuild_pending_ entry even if `this` outlives
        // a particular panel registration.
        observers.push_back(observe<int>(
            subject, this,
            [name, panel_id](PanelWidgetManager* self, int value) {
                spdlog::debug("[PanelWidgetManager] gate '{}' -> {} (rebuild)", name, value);
                crash_handler::breadcrumb::note("gate", name, value);

                // Look up the stable per-panel slot. If the panel was torn
                // down between subscription and firing, skip — the gate
                // observer may have been pending in the UpdateQueue when
                // clear_gate_observers ran.
                auto sit = self->gate_rebuild_slots_.find(panel_id);
                if (sit == self->gate_rebuild_slots_.end()) {
                    return;
                }
                GateRebuildSlot& s = sit->second;
                // Coalesce: if a rebuild is already queued, the queued one
                // will read the latest gate values when it runs.
                if (s.pending) {
                    return;
                }
                s.pending = true;
                // Stable user_data — no allocation in the hot path. Avoids
                // std::bad_alloc → terminate → SIGABRT on memory-tight AD5X
                // ([L083] family), which is why this is raw lv_async_call
                // and not run_next_tick. It escapes the UpdateQueue batch
                // per CLAUDE.md "safe escape routes".
                // LV_ASYNC_OK: allocation-free hot path, cancelled by slot address
                lv_async_call(&PanelWidgetManager::gate_rebuild_trampoline, &s); // LV_ASYNC_OK
            },
            gate_lifetime));
        spdlog::trace("[PanelWidgetManager] Observing gate subject '{}' for panel '{}'", name,
                      panel_id);
    }

    spdlog::debug("[PanelWidgetManager] Set up {} gate observers for panel '{}'", observers.size(),
                  panel_id);
}

void PanelWidgetManager::clear_gate_observers(const std::string& panel_id) {
    if (s_destroyed_) {
        return;
    }
    PanelWidgetManager& mgr = instance();
    auto it = mgr.gate_observers_.find(panel_id);
    if (it != mgr.gate_observers_.end()) {
        spdlog::debug("[PanelWidgetManager] Clearing {} gate observers for panel '{}'",
                      it->second.size(), panel_id);
        mgr.gate_observers_.erase(it);
    }
    // Cancel any in-flight async rebuild *before* destroying the slot it
    // points at. Without this, a rebuild queued via lv_async_call could fire
    // after the slot's storage is freed → UAF on ud / on the captured
    // rebuild_cb (which closes over the registering panel's `this`).
    auto sit = mgr.gate_rebuild_slots_.find(panel_id);
    if (sit != mgr.gate_rebuild_slots_.end()) {
        lv_async_call_cancel(&PanelWidgetManager::gate_rebuild_trampoline, &sit->second);
        mgr.gate_rebuild_slots_.erase(sit);
    }
    mgr.gate_rebuild_callbacks_.erase(panel_id);
}

void PanelWidgetManager::notify_widget_defs_changed() {
    clear_all_panel_configs();
    // Queue each registered gate panel's rebuild through its async slot, the
    // same coalesced route a gate firing takes (see setup_gate_observers):
    // this can run inside an UpdateQueue drain, where a synchronous rebuild
    // would corrupt LVGL's event list.
    for (auto& [panel_id, slot] : gate_rebuild_slots_) {
        (void)panel_id;
        if (slot.pending)
            continue;
        slot.pending = true;
        // LV_ASYNC_OK: same slot trampoline as setup_gate_observers, cancelled by address
        lv_async_call(&PanelWidgetManager::gate_rebuild_trampoline, &slot); // LV_ASYNC_OK
    }
}

void PanelWidgetManager::gate_rebuild_trampoline(void* ud) {
    auto* slot = static_cast<GateRebuildSlot*>(ud);
    if (!slot || !slot->mgr) {
        return;
    }
    PanelWidgetManager& mgr = *slot->mgr;
    std::string panel_id = slot->panel_id;
    // Clear pending BEFORE invoking — a late-arriving gate firing while
    // the rebuild runs queues a fresh rebuild for the next tick.
    slot->pending = false;

    auto cb_it = mgr.gate_rebuild_callbacks_.find(panel_id);
    if (cb_it == mgr.gate_rebuild_callbacks_.end()) {
        // Panel was torn down between queueing and dispatch. clear_gate_observers
        // calls lv_async_call_cancel before erasing the slot, but the cancel
        // can race with an in-progress dispatch — guard explicitly.
        return;
    }
    // Copy the callback before invoking. If rebuild_cb itself triggers a
    // re-registration (clear+setup) for this panel, the underlying map
    // entry can move and invalidate cb_it; the local copy survives.
    RebuildCallback cb = cb_it->second;
    if (cb) {
        cb();
    }
}

void PanelWidgetManager::clear_panel_config(const std::string& panel_id) {
    // Erase all page-keyed entries matching this panel (e.g. "home:0", "home:1", ...)
    std::string prefix = panel_id + ":";
    for (auto it = active_configs_.begin(); it != active_configs_.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            it = active_configs_.erase(it);
        } else {
            ++it;
        }
    }
    // Grid descriptors are keyed by container, not by this panel's pages: the
    // containers laid out with them may still exist and their grid style holds
    // the raw dsc pointers (LVGL does not copy them). They are freed when their
    // container is deleted.
}

void PanelWidgetManager::clear_all_panel_configs() {
    // Active printer changed: every cached PanelWidgetConfig was loaded from the
    // PREVIOUS printer's /printers/<id>/panel_widgets/<panel> path. Mark each
    // dirty so the next load() re-reads from the now-current Config::df() path
    // (the #804 load() guard otherwise serves the stale layout indefinitely).
    for (auto& [panel_id, config] : panel_configs_) {
        (void)panel_id;
        config.mark_dirty();
    }
    // Drop the per-page widget-list cache wholesale — it keys on "panel:page" and
    // describes the old printer's resolved widget list. Grid descriptors stay
    // put: they are keyed by container, so grids laid out for the previous
    // printer keep reading valid memory until their containers are deleted.
    active_configs_.clear();
}

void PanelWidgetManager::install_grid_descriptors(lv_obj_t* container, GridDescriptors&& fresh) {
    auto [slot, inserted] = grid_descriptors_.try_emplace(container);
    if (inserted) {
        // First generation for this container: drop the slot when the container
        // dies, keeping the map bounded by live containers.
        // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative equivalent.
        lv_obj_add_event_cb(container, &PanelWidgetManager::on_container_delete, LV_EVENT_DELETE,
                            nullptr);
    }
    GridDescriptors retired = std::move(slot->second);
    slot->second = std::move(fresh);
    lv_obj_set_grid_dsc_array(container, slot->second.col_dsc.data(), slot->second.row_dsc.data());
    // `retired` frees the previous generation's buffers HERE, after the
    // container's style has been re-pointed above: the style holds the raw
    // pointers, so freeing any earlier would leave every reader of the old
    // arrays (a layout pass, GridEditMode::current_metrics) on freed memory.
}

bool PanelWidgetManager::s_destroyed_ = false;

void PanelWidgetManager::on_container_delete(lv_event_t* e) {
    if (s_destroyed_) {
        return;
    }
    auto* container = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    PanelWidgetManager::instance().grid_descriptors_.erase(container);
}

PanelWidgetConfig& PanelWidgetManager::get_widget_config(const std::string& panel_id) {
    // Per-panel config instances cached by panel ID in panel_configs_.
    // Main-thread only — no synchronization on the map.
    auto it = panel_configs_.find(panel_id);
    if (it == panel_configs_.end()) {
        it = panel_configs_.emplace(panel_id, PanelWidgetConfig(panel_id, *Config::get_instance()))
                 .first;
    }
    // load() is a no-op if already loaded. Callers that bypass the setters must
    // call notify_config_changed() → mark_dirty() (or clear_all_panel_configs()
    // on a printer switch) to trigger a reload. Previously this unconditionally
    // reloaded on every access, churning pages_ many times per panel populate
    // and leaving outer frames exposed to invalidated references (#804).
    it->second.load();
    return it->second;
}

// -- PanelWidget base class --

PanelWidget::~PanelWidget() {
    unbind_root();
    // The tile tree can outlive this widget: the manager drops non-reused
    // instances after a rebuild, and app shutdown destroys panels before
    // lv_deinit(). Uninstall the delete hook while the object is still valid,
    // or the tree's eventual teardown would call on_root_deleted_event() on
    // freed memory. A null delete_hook_root_ means the tree already died (the
    // hook fired) or detach() removed it — nothing left to uninstall.
    uninstall_delete_hook();
}

void PanelWidget::attach_tile(lv_obj_t* root, lv_obj_t* parent_screen) {
    bind_root(root);
    attach(root, parent_screen);
}

void PanelWidget::detach_tile() {
    detach();
    unbind_root();
}

void PanelWidget::bind_root(lv_obj_t* obj) {
    unbind_root();
    if (!obj) {
        return;
    }
    root_ = obj;
    lv_obj_set_user_data(obj, this);
    // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative equivalent.
    lv_obj_add_event_cb(obj, on_bound_root_deleted, LV_EVENT_DELETE, this);
}

void PanelWidget::unbind_root() {
    if (!root_ || !lv_is_initialized()) {
        root_ = nullptr;
        return;
    }
    lv_obj_remove_event_cb_with_user_data(root_, on_bound_root_deleted, this);
    if (lv_obj_get_user_data(root_) == this) {
        lv_obj_set_user_data(root_, nullptr);
    }
    root_ = nullptr;
}

// A raw lv_obj_delete() of the tree gives the widget no detach(); the root
// pointer must not outlive it.
void PanelWidget::on_bound_root_deleted(lv_event_t* e) {
    auto* self = static_cast<PanelWidget*>(lv_event_get_user_data(e));
    if (self && lv_event_get_current_target(e) == self->root_) {
        self->root_ = nullptr;
    }
}

void PanelWidget::record_interaction() {
    TelemetryManager::instance().notify_widget_interaction(id());
}

void PanelWidget::install_delete_hook(lv_obj_t* root) {
    if (!root || delete_hook_root_ == root) {
        return;
    }
    // A recycled instance may still hold the hook on the tree it was detached
    // (or re-attached away) from. Take it off there before hooking the new
    // root, or that tree's late deletion would fire into this widget while its
    // pointers name the successor.
    uninstall_delete_hook();

    // The home panel owns this tree; a raw lv_obj_delete() gives the widget no
    // other notice, and the queued observer handlers would run against the
    // freed child pointers on the next drain.
    // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative equivalent.
    lv_obj_add_event_cb(root, on_root_deleted_event, LV_EVENT_DELETE, this);
    delete_hook_root_ = root;
}

void PanelWidget::uninstall_delete_hook() {
    if (delete_hook_root_ && lv_is_initialized()) {
        lv_obj_remove_event_cb_with_user_data(delete_hook_root_, on_root_deleted_event, this);
    }
    delete_hook_root_ = nullptr;
}

void PanelWidget::on_root_deleted_event(lv_event_t* e) {
    auto* self = static_cast<PanelWidget*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    auto* dying = static_cast<lv_obj_t*>(lv_event_get_current_target(e));

    // Only the tree the hook was installed for matters. install_delete_hook()
    // moves the hook at attach() time, but a tree condemned without a detach
    // (or any future path that swaps roots without going through attach) can
    // still land its late delete event here while the successor's pointers are
    // live — clearing those would blank a live tree. Same staleness skip as
    // PowerPanel's and PrintStatusPanel's hooks.
    if (dying != self->delete_hook_root_) {
        return;
    }

    self->delete_hook_root_ = nullptr;
    self->on_hooked_root_deleted();
}

void PanelWidget::save_widget_config(const nlohmann::json& config) {
    if (panel_id_.empty()) {
        spdlog::warn("[PanelWidget] save_widget_config called with no panel_id set for '{}'", id());
        return;
    }
    auto& wc = PanelWidgetManager::instance().get_widget_config(panel_id_);
    wc.set_widget_config(id(), config);
}

} // namespace helix
