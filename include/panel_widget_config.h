// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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

    /// Generate default grid layout, placing enabled widgets sequentially in 1x1 cells.
    static std::vector<PanelWidgetEntry> build_default_grid();

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

  private:
    std::string panel_id_;
    Config& config_;
    std::vector<PageConfig> pages_;
    size_t main_page_index_ = 0;
    int next_page_id_ = 1;
    bool loaded_ = false;

    static std::vector<PanelWidgetEntry> build_defaults();

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

} // namespace helix
