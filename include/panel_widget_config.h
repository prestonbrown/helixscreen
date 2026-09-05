// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_breakpoint.h"

#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {

class Config;

struct PanelWidgetEntry {
    std::string id;
    bool enabled;
    nlohmann::json config; // Optional per-widget config (empty object = no config)
    // Grid placement coordinates (-1 = auto-place)
    int col = -1;
    int row = -1;
    int colspan = 1;
    int rowspan = 1;

    bool operator==(const PanelWidgetEntry& other) const {
        return id == other.id && enabled == other.enabled && config == other.config &&
               col == other.col && row == other.row && colspan == other.colspan &&
               rowspan == other.rowspan;
    }

    /// Returns true if this entry has explicit grid coordinates
    bool has_grid_position() const {
        return col >= 0 && row >= 0;
    }

    /// Turn this entry off AND surrender its grid cell.
    ///
    /// These belong together. save() persists col/row for every entry whether
    /// it is enabled or not, so a disabled entry that keeps coordinates leaves
    /// a claim on a cell nothing is drawing in - and the anchored pass, which
    /// looks entries up by id, could then hand that cell to a widget the
    /// manager synthesizes (the temporary firmware_restart tile), knocking a
    /// user-anchored widget off its saved rectangle (#1414). Three call sites
    /// wrote this rule by hand; one of them forgot the coordinates.
    void disable_and_unplace() {
        enabled = false;
        col = -1;
        row = -1;
    }
};

/// A single page of widgets in a multi-page home screen
struct PageConfig {
    std::string id;
    std::vector<PanelWidgetEntry> widgets;
};

/// Soft cap on maximum number of pages
static constexpr size_t MAX_PAGES = 8;

class PanelWidgetConfig {
  public:
    PanelWidgetConfig(const std::string& panel_id, Config& config);

    /// Load widget order from config, merging with registry defaults.
    /// No-op if already loaded; call mark_dirty() first to force a reload.
    void load();

    /// Mark the cached pages_ vector as stale so the next load() reloads from disk.
    /// Used by the settings overlay and widget catalog when they mutate Config
    /// directly rather than going through this object's setters.
    void mark_dirty() {
        loaded_ = false;
    }

    /// Save current order to config
    void save();

    // ========================================================================
    // Backward-compatible accessors (delegate to page 0)
    // ========================================================================

    const std::vector<PanelWidgetEntry>& entries() const {
        return pages_[0].widgets;
    }

    std::vector<PanelWidgetEntry>& mutable_entries() {
        return pages_[0].widgets;
    }

    // ========================================================================
    // Multi-page accessors
    // ========================================================================

    /// Number of pages
    size_t page_count() const {
        return pages_.size();
    }

    /// Index of the main (default) page
    size_t main_page_index() const {
        return main_page_index_;
    }

    /// Get widget entries for a specific page (const)
    const std::vector<PanelWidgetEntry>& page_entries(size_t page_index) const {
        return pages_[page_index].widgets;
    }

    /// Get mutable widget entries for a specific page
    std::vector<PanelWidgetEntry>& page_entries_mut(size_t page_index) {
        return pages_[page_index].widgets;
    }

    /// Get the page ID for a specific page index
    const std::string& page_id(size_t page_index) const {
        return pages_[page_index].id;
    }

    /// Get all pages (const)
    const std::vector<PageConfig>& pages() const {
        return pages_;
    }

    /// Add a new empty page. Returns the index of the new page, or -1 if at cap.
    int add_page(const std::string& name = "");

    /// Remove a page by index. Cannot remove the last page.
    /// If the removed page is the main page, main_page_index resets to 0.
    /// Returns true if removed.
    bool remove_page(size_t page_index);

    /// Generate a unique page ID
    std::string generate_page_id();

    /// Move widget between positions. No-op if indices are equal or out of bounds.
    /// Operates on page 0 for backward compatibility.
    void reorder(size_t from_index, size_t to_index);

    /// No-op if index out of bounds.
    /// Operates on page 0 for backward compatibility.
    void set_enabled(size_t index, bool enabled);

    /// Enable or disable a widget by ID. Returns true if found.
    bool set_enabled_by_id(const std::string& id, bool enabled);

    void reset_to_defaults();

    /// Generate the next unique instance ID for a multi-instance base ID.
    /// Scans ALL pages for base_id:N patterns and returns base_id:(max_N+1).
    std::string mint_instance_id(const std::string& base_id);

    /// Remove an entry entirely from ANY page (first match).
    void delete_entry(const std::string& id);

    /// Generate the default layout for a measured grid.
    ///
    /// @p grid_cols / @p grid_rows are the track counts the layout will be
    /// placed on, or 0 when they are not known yet — which is every caller that
    /// runs before the widget container has been measured, config load
    /// included.
    ///
    /// Knowing the grid changes two things. A placement may be keyed by the
    /// grid it was authored for ("xxlarge@6x14") and that entry beats the bare
    /// tier key, because a breakpoint names a panel while the UI scale gives
    /// that same panel a different track count per scale. And an anchor that
    /// does not fit is dropped to auto-placement instead of being handed to
    /// clamp_to_grid, which would shove its origin back until the span fitted
    /// and seat it on top of a neighbour looking deliberate.
    static std::vector<PanelWidgetEntry> build_default_grid(int grid_cols = 0, int grid_rows = 0);

    /// True when @p panel_node places widgets but names no system for the
    /// numbers it places them at.
    ///
    /// A saved layout is coordinates, and a coordinate means nothing on its
    /// own. From config version 24 on, the node says which system it counts in:
    /// `grid` names the track grid, `layout_units` marks pre-v22 cells. A node
    /// holding `pages` with a placed widget and neither key is one that only a
    /// build older than its own version stamp can produce — such a build
    /// re-serializes the panel as `pages`/`main_page_index`/`next_page_id` and
    /// nothing else, so every key naming the system is gone while the numbers
    /// stay. Reading them as tracks seats each widget at a coordinate it never
    /// meant, so load() rebuilds the panel from defaults instead.
    ///
    /// Static and pure so the three ways a layout stays legitimate — a named
    /// grid, a cells tag, a version below 24 — are testable without a Config.
    static bool has_uninterpretable_coordinates(const nlohmann::json& panel_node,
                                                int config_version);

    /// True while this layout is the default set with its anchors not yet
    /// applied, because no grid had been measured when it was built.
    ///
    /// build_defaults() runs at config load, where the widget container does
    /// not exist and therefore neither does its content box - and the content
    /// box, not the panel extent, is what the track count divides (1042x2141
    /// against 1080x2400 on a scaled phone, 6x14 tracks against 8x16). So the
    /// anchors cannot be chosen there. PanelWidgetManager applies them at the
    /// first measured populate and clears the tag.
    ///
    /// A positive tag, written only by the defaults path, so a layout that
    /// predates this or that a user has since arranged reads as "already
    /// placed" and is never overwritten.
    bool has_pending_anchors() const {
        return pending_anchors_;
    }

    /// Grid signature the saved coordinates are expressed in ("6x14"), or ""
    /// when the layout predates per-grid storage and has not been stamped yet.
    const std::string& grid_signature() const {
        return grid_signature_;
    }

    /// Make @p cols x @p rows the active grid.
    ///
    /// A saved layout is coordinates in TRACKS, and a track means nothing
    /// without the grid it counts against. The grid is no longer fixed per
    /// device: the UI scale multiplies the cell edge, so one panel yields a
    /// different track count per scale, and restoring a config onto other
    /// hardware moves it too. Rewriting one stored layout each time the grid
    /// changed destroyed the arrangement - the write-back in populate_widgets()
    /// persists computed positions, so the degraded copy became the only copy
    /// and switching back had nothing to restore.
    ///
    /// So each grid keeps its own arrangement. The active one stays exactly
    /// where it always was, which leaves every existing reader untouched; the
    /// rest are parked beside it. Switching parks the outgoing layout, then
    /// restores this grid's if it has one, or seeds it by remapping the
    /// outgoing one - the arrangement the user was just looking at is the
    /// closest thing to their intent that exists, and port_legacy_layout()
    /// makes it fit by construction.
    ///
    /// A layout with no recorded grid is stamped and otherwise left alone:
    /// which grid it was arranged on is unrecoverable, and reseating it on a
    /// guess would discard a real arrangement. No-op when already active.
    void switch_to_grid(int cols, int rows);

    /// Apply the default anchors for a now-known grid, then persist and clear
    /// the tag. No-op unless has_pending_anchors().
    void apply_pending_anchors(int grid_cols, int grid_rows);

    /// Check if config uses grid format (has any entries with col/row fields)
    bool is_grid_format() const;

    /// Search ALL pages for a widget with this ID
    bool is_enabled(const std::string& id) const;

    /// True when this widget is enabled AND holds a grid cell on some page.
    ///
    /// Not the same question as is_enabled(): an entry can be enabled at
    /// (-1,-1) — the setup wizard enables without a position, a hardware gate
    /// can open after the grid filled, and PanelWidgetManager's GridFull
    /// eviction drops the position rather than the entry so the widget can
    /// come back on its own. Such a widget is configured but on no dashboard,
    /// so anything asking "is it already on the grid" (the widget catalog)
    /// must ask this, not is_enabled().
    bool is_placed(const std::string& id) const;

    /// Get per-widget config for a given widget ID (searches all pages)
    nlohmann::json get_widget_config(const std::string& id) const;

    /// Set per-widget config for a given widget ID (searches all pages), then save
    void set_widget_config(const std::string& id, const nlohmann::json& config);

    /// True while these coordinates are still counts of cells in the pre-v22
    /// home grid rather than tracks of the square-cell one (#1126).
    ///
    /// Set by the v22 migration, which cannot convert them itself: it runs at
    /// config load, with no screen size settled and none recorded. The first
    /// grid build has both, so PanelWidgetManager ports the layout there and
    /// clears the tag. Until it does, the coordinates must not be read as
    /// tracks — they name a grid with different dimensions and a different unit.
    bool has_legacy_units() const {
        return legacy_units_;
    }

    /// Row count the pre-v22 grid was known to have reached, 0 when unknown.
    /// The old grid sized its row axis from the widgets in use, so this is the
    /// floor its cache held for widgets whose hardware gate had not yet fired.
    int legacy_rows() const {
        return legacy_rows_;
    }

    /// Drop the legacy-units tag and persist. Call once the port has run, so a
    /// layout already in track units is never ported a second time.
    void clear_legacy_units();

  private:
    std::string panel_id_;
    Config& config_;
    std::vector<PageConfig> pages_;
    size_t main_page_index_ = 0;
    int next_page_id_ = 1;
    bool loaded_ = false;
    bool pending_anchors_ = false;
    std::string grid_signature_;
    /// Arrangements for grids that are not active, keyed by signature. Each
    /// value has the same shape as the active layout's persisted form.
    nlohmann::json parked_grids_ = nlohmann::json::object();
    bool legacy_units_ = false;
    int legacy_rows_ = 0;

    static std::vector<PanelWidgetEntry> build_defaults();

    /// The pages payload as it is persisted. Shared by save() and by
    /// switch_to_grid(), which parks exactly what save() would have written.
    nlohmann::json serialize_pages() const;

    /// Replace pages_ from a payload produced by serialize_pages().
    void restore_pages(const nlohmann::json& payload);

    /// Parse a JSON array of widget entries into a vector, applying migrations.
    /// If append_registry_defaults is true, appends missing registry widgets.
    std::vector<PanelWidgetEntry> parse_widget_array(const nlohmann::json& arr,
                                                     bool append_registry_defaults = true);

    /// Attempt to populate pages_ from a preset-shipped seed file at
    /// assets/config/panel_widgets/<preset>/<panel_id>.json. Returns true if a
    /// seed was found and applied. Used only on fresh installs (no saved config).
    bool try_populate_from_preset_seed();

    /// One-shot migration for installs stuck in the pre-ef580203d state where
    /// the filament widget captured a grid cell before ams_slot_count reported
    /// live, leaving the ams widget enabled but at (-1,-1) with no way to
    /// surface it. If that exact pattern is detected on any page, swap ams
    /// into filament's grid position and disable filament. Self-gating: the
    /// condition no longer holds after the swap. No-op on every other layout.
    /// Returns true if any page was mutated.
    bool migrate_stuck_ams_filament_swap();
};

/// Most specific breakpoint key present in @p by_bp, or nullptr.
///
/// Both the per-anchor `placements` map and a variant's `disabled` map are keyed
/// by breakpoint name and resolve through the same chain theme_manager uses.
/// Exported so the shipped-table tests resolve keys through the same function
/// the loader uses instead of carrying a copy of the fallback chain.
const char* choose_breakpoint_key(const nlohmann::json& by_bp, UiBreakpoint breakpoint);

} // namespace helix
