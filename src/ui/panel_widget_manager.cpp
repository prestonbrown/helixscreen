// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_manager.h"

#include "ui_ams_mini_status.h"
#include "ui_notification.h"
#include "ui_utils.h"

#include "config.h"
#include "grid_edit_mode.h"
#include "grid_layout.h"
#include "layout_manager.h"
#include "layout_port.h"
#include "observer_factory.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"
#include "printer_cache_registry.h"
#include "system/crash_handler.h"
#include "system/telemetry_manager.h"
#include "theme_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <queue>
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

PanelWidgetManager& PanelWidgetManager::instance() {
    static PanelWidgetManager instance;
    return instance;
}

void PanelWidgetManager::clear_shared_resources() {
    shared_resources_.clear();
}

void PanelWidgetManager::init_widget_subjects() {
    if (widget_subjects_initialized_) {
        return;
    }

    // Register all widget factories explicitly (avoids SIOF from file-scope statics)
    init_widget_registrations();

    for (const auto& def : get_all_widget_defs()) {
        if (def.init_subjects) {
            spdlog::debug("[PanelWidgetManager] Initializing subjects for widget '{}'", def.id);
            def.init_subjects();
        }
    }

    widget_subjects_initialized_ = true;

    // Self-register per-printer cache invalidation. panel_configs_ / active_configs_ /
    // grid_descriptors_ all derive from /printers/<active>/panel_widgets/<panel>, so an
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
    rebuild_callbacks_.erase(panel_id);
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

    // Collect enabled + hardware-available widgets
    std::vector<WidgetSlot> enabled_widgets;
    for (const auto& entry : widget_config.page_entries(page_index)) {
        if (!entry.enabled) {
            continue;
        }

        // Check hardware gate — flag widgets whose hardware isn't present.
        // Gates are defined in PanelWidgetDef::hardware_gate_subject and checked
        // here instead of XML bind_flag_if_eq to avoid orphaned dividers.
        const auto* def = find_widget_def(entry.id);
        bool gated = false;
        const char* hint = nullptr;
        if (def && def->hardware_gate_subject) {
            lv_subject_t* gate = lv_xml_get_subject(nullptr, def->hardware_gate_subject);
            if (gate && lv_subject_get_int(gate) == 0) {
                gated = true;
                hint = def->hardware_gate_hint;
            }
        }

        WidgetSlot slot;
        slot.widget_id = entry.id;
        slot.config = entry.config;

        // Build + configure the widget defensively: a malformed per-widget config
        // (or a throwing factory/set_config) must skip only THIS widget, not abort
        // the whole dashboard rebuild. Guard per-iteration so one bad entry never
        // takes the page down with it.
        try {
            // Acquire instance: reuse existing or create via factory
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
        } catch (const std::exception& e) {
            spdlog::error("[PanelWidgetManager] Widget '{}' configuration failed: {}", entry.id,
                          e.what());
            continue;
        }

        slot.hardware_gated = gated;
        slot.gate_hint = hint;

        enabled_widgets.push_back(std::move(slot));
    }

    // Check if widget list is unchanged — skip teardown+rebuild if nothing changed.
    // Gate status is part of the key: a widget transitioning from gated→ungated
    // must trigger a rebuild so its cancel-icon overlay + OPA_40 are removed and
    // the PanelWidget instance gets attached. Matches compute_visible_widget_ids().
    {
        std::vector<std::string> new_ids;
        new_ids.reserve(enabled_widgets.size());
        for (const auto& slot : enabled_widgets) {
            new_ids.push_back(slot.hardware_gated ? slot.widget_id + "~gated" : slot.widget_id);
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
        klippy_layout &&
        lv_subject_get_int(klippy_layout) != static_cast<int>(KlippyState::READY);
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
        if (entry_it != entries.end() && entry_it->enabled && entry_it->has_grid_position()) {
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

            // Pin print_status to bottom row on first layout (no user edit yet).
            // Skip pinning if the grid edit mode is active — user is positioning manually.
            // We detect user-positioned widgets by checking if the row would differ;
            // during initial layout (auto-placed), the row will be -1 and get_grid_position
            // won't match, so this only fires for the default layout.
            // TODO: replace with explicit "user_positioned" flag in config

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
            spdlog::debug("[PanelWidgetManager] Widget '{}' placed at reduced span {}x{} "
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
    // Installing a fresh generation for this key is the first moment a
    // previously-retired one is unreferenced (the container's style re-points
    // to the new arrays at the end of this populate) — drop it now.
    retired_grid_descriptors_.erase(make_cache_key(panel_id, page_index));
    auto& dsc = grid_descriptors_[make_cache_key(panel_id, page_index)];
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
    // Same content box the track counts came from, so the two cannot disagree.
    CellMetrics metrics = grid_cell_metrics(content_w, content_h, cols, grid_rows, gutter);
    int cell_w = static_cast<int>(metrics.cell_w);
    int cell_h = static_cast<int>(metrics.cell_h);
    spdlog::debug("[PanelWidgetManager] Track geometry: {:.2f}x{:.2f}px, gutter {}px",
                  metrics.cell_w, metrics.cell_h, gutter);

    // Create merged card backgrounds behind adjacent widgets that ask for one.
    // BFS flood-fill finds connected components of merging widgets, then a
    // single card object spans each component's bounding rectangle.
    //
    // All analysis happens in TRACK coordinates - the same units the grid is
    // addressed in. It used to work in cells and convert back, which forced
    // every widget off a cell boundary out of the merge entirely: a widget that
    // supports half-cell resolution can be dragged onto an odd track, and
    // dividing that position into cells truncates it, so the card would have
    // landed half a cell from the widget it belongs to. Excluding those left
    // them with no background at all. Tracks divide exactly, so there is
    // nothing to exclude.
    //
    // Use ALL enabled config entries (not just currently-placed ones) so that
    // cards for hardware-gated widgets appear from the first frame, preventing
    // the grid from visually jumping when hardware gates fire.
    {
        // Tracks covered by widgets that share the fused card, and tracks
        // blocked by widgets that bring a background of their own.
        struct TrackHash {
            size_t operator()(const std::pair<int, int>& p) const {
                return std::hash<int>()(p.first) ^ (std::hash<int>()(p.second) << 16);
            }
        };
        std::unordered_set<std::pair<int, int>, TrackHash> merged_tracks;
        std::unordered_set<std::pair<int, int>, TrackHash> occupied_by_own_card;

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

        for (const auto& entry : widget_config.page_entries(page_index)) {
            if (!entry.enabled || !entry.has_grid_position()) {
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
            auto& covered = (def && def->merges_into_card) ? merged_tracks : occupied_by_own_card;
            for (int r = fitted.row; r < fitted.row + fitted.rowspan; r++) {
                for (int c = fitted.col; c < fitted.col + fitted.colspan; c++) {
                    covered.insert({c, r});
                }
            }
        }

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
            // Every emitted piece is still a rectangle. The ragged look this
            // used to have came from carving a hole in the MIDDLE of a run:
            // ams sat one cell into the readout row while painting its own
            // card, which split that row into three pieces of differing
            // heights. It merges now, so a carve-out only ever trims an edge.
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

                int end_col = start_col;
                while (remaining.count({end_col + 1, start_row})) {
                    end_col++;
                }

                // Extend down only while every column in the run is present, so
                // the result is always a full rectangle.
                int end_row = start_row;
                for (;;) {
                    bool can_extend = true;
                    for (int c = start_col; c <= end_col; c++) {
                        if (!remaining.count({c, end_row + 1})) {
                            can_extend = false;
                            break;
                        }
                    }
                    if (!can_extend) {
                        break;
                    }
                    end_row++;
                }

                for (int r = start_row; r <= end_row; r++) {
                    for (int c = start_col; c <= end_col; c++) {
                        remaining.erase({c, r});
                    }
                }

                // Already in track coordinates, which is what the grid takes.
                int track_col = start_col;
                int track_row = start_row;
                int track_colspan = end_col - start_col + 1;
                int track_rowspan = end_row - start_row + 1;

                // Create a plain lv_obj with Card styling as the background
                lv_obj_t* card_bg = lv_obj_create(container);
                lv_obj_remove_style(card_bg, nullptr, LV_PART_MAIN);
                lv_obj_add_style(card_bg, ThemeManager::instance().get_style(StyleRole::Card),
                                 LV_PART_MAIN);
                lv_obj_set_style_pad_all(card_bg, 0, 0);
                lv_obj_remove_flag(card_bg, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_remove_flag(card_bg, LV_OBJ_FLAG_SCROLLABLE);
                // Set initial size from cached track dimensions so the card renders
                // at approximately the right shape on the first frame, before the
                // grid layout resolves. Grid STRETCH overrides once layout runs.
                if (cell_w > 0 && cell_h > 0) {
                    lv_obj_set_size(
                        card_bg,
                        static_cast<int>(grid_track_extent(metrics.cell_w, gutter, track_colspan)),
                        static_cast<int>(grid_track_extent(metrics.cell_h, gutter, track_rowspan)));
                }
                lv_obj_set_grid_cell(card_bg, LV_GRID_ALIGN_STRETCH, track_col, track_colspan,
                                     LV_GRID_ALIGN_STRETCH, track_row, track_rowspan);

                spdlog::debug("[PanelWidgetManager] Card background at ({},{} {}x{} tracks)",
                              track_col, track_row, track_colspan, track_rowspan);
            }
        }
    }

    // Second pass: create XML components and place in grid cells
    std::vector<std::unique_ptr<PanelWidget>> result;

    for (const auto& p : placed) {
        try {
            auto& slot = enabled_widgets[p.slot_index];

            // Create XML component
            auto* widget = static_cast<lv_obj_t*>(
                lv_xml_create(container, slot.component_name.c_str(), nullptr));
            if (!widget) {
                spdlog::warn("[PanelWidgetManager] Failed to create widget: {} (component: {})",
                             slot.widget_id, slot.component_name);
                continue;
            }

            // Place in grid cell
            lv_obj_set_grid_cell(widget, LV_GRID_ALIGN_STRETCH, p.col, p.colspan,
                                 LV_GRID_ALIGN_STRETCH, p.row, p.rowspan);

            // Tag widget with its config ID so GridEditMode can identify it
            lv_obj_set_name(widget, slot.widget_id.c_str());

            // Mark the tile root so tree walks that only make sense at page
            // level stop here. See PANEL_WIDGET_TILE_FLAG in panel_widget.h.
            lv_obj_add_flag(widget, PANEL_WIDGET_TILE_FLAG);

            spdlog::debug("[PanelWidgetManager] Placed widget '{}' at ({},{} {}x{})",
                          slot.widget_id, p.col, p.row, p.colspan, p.rowspan);

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

                const char* badge_attrs[] = {
                    "src",    "cancel",    "size",  "xl",           "variant", "muted", "align",
                    "center", "clickable", "false", "event_bubble", "true",    nullptr};
                if (auto* badge =
                        static_cast<lv_obj_t*>(lv_xml_create(widget, "icon", badge_attrs))) {
                    lv_obj_add_flag(badge, LV_OBJ_FLAG_FLOATING);
                    lv_obj_set_style_opa(badge, LV_OPA_COVER, 0);
                }

                spdlog::debug("[PanelWidgetManager] Widget '{}' gated: {}", slot.widget_id,
                              slot.gate_hint ? slot.gate_hint : "hardware not detected");
            }

            // Attach the pre-created PanelWidget instance if present and NOT gated
            if (slot.instance && !slot.hardware_gated) {
                slot.instance->attach(widget, lv_scr_act());

                // Notify widget of its grid allocation and approximate pixel size
                slot.instance->on_size_changed(
                    p.colspan, p.rowspan,
                    static_cast<int>(grid_track_extent(metrics.cell_w, metrics.gutter, p.colspan)),
                    static_cast<int>(grid_track_extent(metrics.cell_h, metrics.gutter, p.rowspan)));

                result.push_back(std::move(slot.instance));
            }

            // Propagate width to AMS mini status (pure XML widget, no PanelWidget)
            if (slot.widget_id == "ams") {
                lv_obj_t* ams_child = lv_obj_get_child(widget, 0);
                if (ams_child && ui_ams_mini_status_is_valid(ams_child)) {
                    ui_ams_mini_status_set_width(
                        ams_child, static_cast<int>(grid_track_extent(metrics.cell_w,
                                                                      metrics.gutter, p.colspan)));
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("[PanelWidgetManager] Widget '{}' creation failed: {}",
                          enabled_widgets[p.slot_index].widget_id, e.what());
        }
    }

    spdlog::debug("[PanelWidgetManager] Populated {} widgets ({} with factories) via grid for '{}'",
                  placed.size(), result.size(), panel_id);

    // All children (card backgrounds + widgets) now exist and carry their
    // per-cell grid placement. Install the grid descriptor + activate the grid
    // layout last so the very first grid_update runs over a complete, valid grid
    // in a single clean pass — never over a half-built one (#983).
    // lv_obj_set_grid_dsc_array() is what turns the container into a live grid;
    // lv_obj_set_layout() is belt-and-suspenders. The explicit update_layout
    // forces that pass now and re-flows widgets whose attach() read a pre-grid
    // size. `dsc` is stored in grid_descriptors_ (a member), so its backing
    // arrays remain valid for the lifetime of the active grid.
    lv_obj_set_grid_dsc_array(container, dsc.col_dsc.data(), dsc.row_dsc.data());
    lv_obj_set_layout(container, LV_LAYOUT_GRID);
    lv_obj_update_layout(container);

    populating_ = false;
    return result;
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
        const auto* def = find_widget_def(entry.id);
        bool gated = false;
        if (def && def->hardware_gate_subject) {
            lv_subject_t* gate = lv_xml_get_subject(nullptr, def->hardware_gate_subject);
            if (gate && lv_subject_get_int(gate) == 0) {
                gated = true;
            }
        }
        ids.push_back(gated ? entry.id + "~gated" : entry.id);
    }

    return ids;
}

void PanelWidgetManager::setup_gate_observers(const std::string& panel_id,
                                              RebuildCallback rebuild_cb) {
    using helix::ui::observe_int_sync;

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
        // Capture panel_id by value into the lambda so the async rebuild
        // can find the right rebuild_pending_ entry even if `this` outlives
        // a particular panel registration.
        observers.push_back(observe_int_sync<PanelWidgetManager>(
            subject, this, [name, panel_id](PanelWidgetManager* self, int value) {
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
                // ([L083] family). lv_async_call escapes the UpdateQueue
                // batch per CLAUDE.md "safe escape routes".
                lv_async_call(&PanelWidgetManager::gate_rebuild_trampoline, &s);
            }));
        spdlog::trace("[PanelWidgetManager] Observing gate subject '{}' for panel '{}'", name,
                      panel_id);
    }

    spdlog::debug("[PanelWidgetManager] Set up {} gate observers for panel '{}'", observers.size(),
                  panel_id);
}

void PanelWidgetManager::clear_gate_observers(const std::string& panel_id) {
    auto it = gate_observers_.find(panel_id);
    if (it != gate_observers_.end()) {
        spdlog::debug("[PanelWidgetManager] Clearing {} gate observers for panel '{}'",
                      it->second.size(), panel_id);
        gate_observers_.erase(it);
    }
    // Cancel any in-flight async rebuild *before* destroying the slot it
    // points at. Without this, a rebuild queued via lv_async_call could fire
    // after the slot's storage is freed → UAF on ud / on the captured
    // rebuild_cb (which closes over the registering panel's `this`).
    auto sit = gate_rebuild_slots_.find(panel_id);
    if (sit != gate_rebuild_slots_.end()) {
        lv_async_call_cancel(&PanelWidgetManager::gate_rebuild_trampoline, &sit->second);
        gate_rebuild_slots_.erase(sit);
    }
    gate_rebuild_callbacks_.erase(panel_id);
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
    // Also erase grid descriptors for all pages of this panel. The vectors are
    // RETIRED, not freed: the containers laid out with them may still exist and
    // their grid style holds the raw dsc pointers (LVGL does not copy them).
    // Freeing here was the 2026-08-17 nightly's heap-use-after-free —
    // GridEditMode::current_metrics -> grid_count_tracks read the freed arrays.
    retire_grid_descriptors_matching(prefix);
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
    // Drop the per-page derived caches wholesale — they key on "panel:page" and
    // describe the old printer's resolved widget list / grid geometry. Descriptors
    // are retired (see clear_panel_config) so grids laid out for the previous
    // printer keep reading valid memory until their containers are destroyed.
    active_configs_.clear();
    retire_grid_descriptors_matching({});
}

void PanelWidgetManager::retire_grid_descriptors_matching(const std::string& prefix) {
    // Move descriptor arrays whose cache key starts with `prefix` (empty prefix:
    // all of them) out of grid_descriptors_ and into the retirement map. LVGL's
    // grid style holds the raw dsc pointers without copying, and the clear paths
    // have no container handle to unstyle, so an array must outlive every
    // container still laid out with it. populate_page() drops the retired entry
    // for a key at the same moment it installs a fresh one — the re-point of the
    // container's style is what makes the old array unreferenced.
    for (auto it = grid_descriptors_.begin(); it != grid_descriptors_.end();) {
        if (prefix.empty() || it->first.compare(0, prefix.size(), prefix) == 0) {
            retired_grid_descriptors_[it->first] = std::move(it->second);
            it = grid_descriptors_.erase(it);
        } else {
            ++it;
        }
    }
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
    // The tile tree can outlive this widget: the manager drops non-reused
    // instances after a rebuild, and app shutdown destroys panels before
    // lv_deinit(). Uninstall the delete hook while the object is still valid,
    // or the tree's eventual teardown would call on_root_deleted_event() on
    // freed memory. A null delete_hook_root_ means the tree already died (the
    // hook fired) or detach() removed it — nothing left to uninstall.
    uninstall_delete_hook();
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
