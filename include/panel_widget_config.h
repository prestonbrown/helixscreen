// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_breakpoint.h"

#include "default_layout_resolver.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "hv/json.hpp"

namespace helix {

class Config;

namespace home_layout {
struct ParsedLayout;
}

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

    /// True when this entry is enabled AND holds a grid cell, which is what
    /// puts the widget on a dashboard. An entry enabled at (-1,-1), or disabled
    /// while keeping coordinates, draws nothing.
    bool is_placed() const {
        return enabled && has_grid_position();
    }

    /// Turn this entry off AND surrender its grid cell.
    ///
    /// These belong together. save() persists col/row for every entry whether
    /// it is enabled or not, so a disabled entry that keeps coordinates leaves
    /// a claim on a cell nothing is drawing in - and the anchored pass, which
    /// looks entries up by id, could then hand that cell to a widget the
    /// manager synthesizes (the temporary firmware_restart tile), knocking a
    /// user-anchored widget off its saved rectangle (#1414).
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
    /// Saved entries whose id has no widget definition (a plugin widget while
    /// its plugin is absent), kept verbatim so the placement is restored when
    /// the definition returns.
    nlohmann::json retained = nlohmann::json::array();
};

/// Soft cap on maximum number of pages
static constexpr size_t kMaxPages = 8;

class PanelWidgetConfig {
  public:
    PanelWidgetConfig(const std::string& panel_id, Config& config);

    /// Load widget order from config, merging with registry defaults.
    /// No-op if already loaded; call mark_dirty() first to force a reload.
    void load();

    /// Mark the cached pages_ vector as stale so the next load() reloads from disk.
    /// Used by the settings overlay and widget catalog when they mutate Config
    /// directly rather than going through this object's setters, and on an
    /// active-printer change. A save held by save_soon() is written first, to
    /// the path it was requested for, so the reload cannot discard it.
    void mark_dirty();

    /// mark_dirty() for a new active printer: the next load() also re-derives
    /// had_saved_layout() from that printer's settings. A plain mark_dirty()
    /// keeps the answer, since this process's own saves would otherwise turn a
    /// printer that never had a saved layout into one that does.
    void mark_printer_changed();

    /// Save current order to config now. Replaces a save held by save_soon().
    void save();

    /// Save once edits settle: kSaveSettleMs after the last request, to the
    /// panel's path as it was at the first. Every save is a flash write that
    /// stalls a slow board's UI for hundreds of ms, and an edit session asks
    /// for one per drop and resize.
    void save_soon();

    /// Write a save held by save_soon() now. Nothing when none is held.
    void flush_pending_save();

    /// A save requested through save_soon() has not been written yet.
    bool save_pending() const {
        return deferred_save_.timer != nullptr;
    }

    static constexpr uint32_t kSaveSettleMs = 2000;

    /// Told after every write that persisted this panel's layout for the
    /// active printer: save(), and save_soon() / flush_pending_save() once
    /// they write. Not told when the config write failed, or when a held
    /// save lands on a printer that is no longer active. Main thread. One
    /// listener process-wide; an empty function removes it.
    static void set_save_listener(std::function<void(const PanelWidgetConfig&)> fn);

    const std::string& panel_id() const {
        return panel_id_;
    }

    /// The layout as save() writes it: `{pages, main_page_index, ...}`, the
    /// input build_payload() takes.
    nlohmann::json published_pages() const {
        return serialize_pages();
    }

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

    /// Get widget entries for a specific page (const). A page index past the
    /// last page reads as an empty page.
    const std::vector<PanelWidgetEntry>& page_entries(size_t page_index) const {
        if (page_index >= pages_.size()) {
            static const std::vector<PanelWidgetEntry> kEmpty;
            return kEmpty;
        }
        return pages_[page_index].widgets;
    }

    /// Get mutable widget entries for a specific page.
    ///
    /// Precondition: @p page_index < page_count(). Unlike page_entries(), an
    /// index past the last page is not checked: there is no page to write to.
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

    /// True when page @p page_index holds at least one placed entry
    /// (PanelWidgetEntry::is_placed). A page index past the last page reads as
    /// unpopulated.
    bool page_is_populated(size_t page_index) const;

    /// Place widget @p id on page @p page_index at (@p col, @p row), spanning
    /// @p colspan x @p rowspan, enabled. Returns the entry's index on that page,
    /// or -1 when @p page_index is past the last page, in which case nothing
    /// changes.
    ///
    /// Leaves exactly one entry named @p id across all pages. The first entry
    /// with that id on the landing page is updated in place; with none there,
    /// the entry is created on it. Every other entry with that id, on any page,
    /// is removed: two entries with one id render the widget twice, and
    /// delete_entry() only removes the first.
    ///
    /// Per-widget config belongs to the widget, not to the page it sat on. The
    /// kept entry's own config stays; an entry without one takes the first
    /// non-empty config among the removed entries, in page order.
    ///
    /// The rectangle is written as given: fitting it to the grid is the
    /// caller's job. Does not save(). @p id is taken by value because callers
    /// pass the id of an entry this call may erase.
    int place_entry(std::string id, size_t page_index, int col, int row, int colspan, int rowspan);

    /// True while another page fits under the cap (kMaxPages).
    bool can_add_page() const {
        return pages_.size() < kMaxPages;
    }

    /// Add a new empty page. Returns the index of the new page, or -1 if at cap.
    /// A @p position of -1 appends past the last page; any other position is
    /// clamped into [0, page_count] and the page is inserted there, shifting
    /// every later page and the main page's index with it. The one place a page
    /// is created: every affordance routes through it.
    int add_page(const std::string& name = "", int position = -1);

    /// Remove a page by index. Returns true if removed.
    /// Refuses, returning false, the last remaining page, the main page and an
    /// index past the last page. Removing a page before the main page shifts
    /// main_page_index down with it.
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

    /// Take entry @p index off page @p page_index the way Edit Mode's trash
    /// does: an added instance ("fan:1") is deleted, a built-in widget stays in
    /// the catalog disabled and unplaced. Does not save().
    void remove_from_page(size_t page_index, size_t index);

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
    ///
    /// The table is resolved by helix::layout::resolve_default_layout() against
    /// a snapshot_capabilities() of the live subjects: fixed anchors, then each
    /// slot's first qualifying alternative. Every candidate the resolver passed
    /// over, and every id the tier's `disabled` list names, comes back disabled
    /// and unplaced. A registry widget the table never names comes back at its
    /// default_enabled with no position; a widget the table placed but the
    /// measured grid cannot seat stays enabled with no position, for
    /// auto-placement. @p signature, when given, receives the
    /// resolver's signature for this result.
    ///
    /// @p overlay, a preset seed from load_preset_overlay(), is applied over the
    /// table; null resolves the table alone.
    static std::vector<PanelWidgetEntry>
    build_default_grid(int grid_cols = 0, int grid_rows = 0, std::string* signature = nullptr,
                       const nlohmann::json* overlay = nullptr);

    /// Values of every capability subject @p table (and @p overlay, a preset
    /// seed, when given) names, read from the live XML subject registry.
    ///
    /// A registry gate subject that is not registered reads 1, the way the home
    /// panel treats a widget whose gate subject does not exist: ungated. A
    /// `requires` subject that is not registered reads 0, since nothing has
    /// said the capability is there.
    static helix::layout::CapabilitySnapshot
    snapshot_capabilities(const nlohmann::json& table, const nlohmann::json* overlay = nullptr);

    /// The shipped default layout table (default_layout.json), parsed; null
    /// when the file is missing or not valid JSON. The parse is kept until the
    /// file's mtime or size changes, so edits to the runtime-editable table
    /// still apply. Never null; hold the pointer, not a reference into it.
    /// LVGL thread only.
    static std::shared_ptr<const nlohmann::json> load_default_table();

    /// The seed of @p preset (panel_widgets/<preset>/home.json), parsed: an
    /// overlay build_default_grid() applies over the default table. Null when
    /// the preset is empty, has no seed file, or the file is not a JSON object.
    /// Cached the way load_default_table() is. LVGL thread only.
    static std::shared_ptr<const nlohmann::json> load_preset_overlay(const std::string& preset);

    /// How many times load_default_table() has read the file from disk.
    static int default_table_reads();

    /// How many times load_preset_overlay() has read a seed file from disk.
    static int preset_overlay_reads();

    /// True while a layout edit session has this panel open. A live default
    /// does not re-resolve then: the arrangement under the user's hands stays
    /// put until the session ends.
    void set_edit_session_open(bool open) {
        edit_session_open_ = open;
    }
    bool edit_session_open() const {
        return edit_session_open_;
    }

    /// True while the saved pages are exactly the arrangement the resolver
    /// produced: a default block is stamped and its hash matches
    /// helix::layout::arrangement_hash() of the current pages. Any change to
    /// what is placed where, or to the page count, makes the layout the user's.
    /// Per-widget config is outside the hash. The older `anchors: "pending"`
    /// tag, which carries no hash, also reads as unedited: it is the builder's
    /// own placement.
    bool is_unedited_default() const;

    /// True when this printer's settings held a panel_widgets node for the
    /// panel when load() first ran, or after reset_to_defaults() ran: someone
    /// has had a say in this layout, so an unedited default here is a chosen
    /// one and is never replaced by a layout copied from the printer's screen.
    /// Derived at load, never persisted.
    bool had_saved_layout() const {
        return had_saved_layout_;
    }

    /// Resolve the default layout again for a @p cols x @p rows grid and, when
    /// the result differs from what is stamped, install it on page 0, stamp it
    /// and save once (through save_soon()). Returns true when it rewrote the
    /// arrangement. The resolver's signature is compared before any entries
    /// are built.
    ///
    /// Only an unedited default (is_unedited_default()) is touched. An
    /// unchanged signature on the same grid writes nothing. On a different
    /// grid, one the user arranged by hand (a parked layout that is not an
    /// unedited default) is left for switch_to_grid() to restore. Per-widget
    /// config carries across for every id that survives.
    ///
    /// @p persist false is for a printer that is not CONNECTED and READY: a
    /// pending block, or a default whose grid changed, is resolved in memory
    /// for display and left pending (empty signature), with nothing saved;
    /// an already resolved default on the same grid is left alone.
    bool reresolve_default_if_changed(int cols, int rows, bool persist = true);

    /// Record the current arrangement as the default's own, keeping its
    /// signature. For placement the populate pass does on the default's behalf
    /// (auto-placing a widget the table left unseated, dropping one the grid
    /// cannot hold), which is not a user edit. No-op without a default block.
    void restamp_default_hash();

    /// Track counts of the active grid, parsed from grid_signature(); {0, 0}
    /// when no grid is stamped.
    std::pair<int, int> stamped_grid() const;

    /// The resolver signature stamped with the default block, or "" when there
    /// is no block or the block is still pending a measured grid.
    std::string default_signature() const;

    /// True when @p panel_node places widgets but names no system for the
    /// numbers it places them at.
    ///
    /// A saved layout is coordinates, and a coordinate means nothing on its
    /// own. From config version 24 on, the node says which system it counts in:
    /// `grid` names the track grid, `layout_units` marks pre-v22 cells, and
    /// a `default` block marks the shipped defaults (`anchors: "pending"` is
    /// its older spelling). A node holding `pages` with a placed widget and none of these is one
    /// that only a build older than its own version stamp can produce — such a build re-serializes
    /// the panel as `pages`/`main_page_index`/`next_page_id` and nothing else, so every key naming
    /// the system is gone while the numbers stay. Reading them as tracks seats each widget at a
    /// coordinate it never meant, so load() rebuilds the panel from defaults instead.
    ///
    /// Static and pure so the ways a layout stays legitimate — a named grid, a
    /// cells tag, pending default anchors, a version below 24 — are testable
    /// without a Config.
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
    /// first measured populate and stamps the signature it resolved for.
    ///
    /// Persisted as a `default` block with an empty signature, written only by
    /// the defaults path, so a layout that predates this or that a user has
    /// since arranged reads as "already placed" and is never overwritten. The
    /// older `anchors: "pending"` key reads the same way.
    bool has_pending_anchors() const {
        return has_default_ && default_signature_.empty();
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
    ///
    /// An unedited default while an edit session is open is ported too, so the
    /// session keeps its arrangement, but it keeps a pending default block and
    /// is not saved: it resolves for the new grid once the session closes.
    void switch_to_grid(int cols, int rows);

    /// Replace this panel's layout with @p layout, a layout arranged on a
    /// layout.cols x layout.rows grid (another screen's), made to fit the
    /// @p cols x @p rows grid this screen shows.
    ///
    /// Screen-scoped widgets are stripped from the import, and this screen's
    /// own default-enabled screen-scoped widgets are added to the main page,
    /// enabled and unplaced. The placements go through the same porter
    /// switch_to_grid() seeds a new grid with, which on an identical grid
    /// copies them one-to-one and clamps anything off it. The result is the
    /// user's layout (no default block), the only arrangement held (every
    /// parked grid is discarded, since each is a port of the layout replaced),
    /// and is saved once.
    ///
    /// Returns false, changing nothing and saving nothing, when either grid has
    /// a non-positive axis or no page keeps a known widget after stripping.
    bool import_layout(const helix::home_layout::ParsedLayout& layout, int cols, int rows);

    /// Apply the default anchors for a now-known grid, stamp the default block
    /// with their signature and hash, and persist. No-op unless has_pending_anchors().
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
    bool had_saved_layout_ = false;
    bool saved_layout_known_ = false;
    /// The `default` block: present when the pages came from the defaults
    /// builder. An empty signature means the anchors still wait for a measured
    /// grid; the hash is arrangement_hash() of the pages as stamped.
    bool has_default_ = false;
    bool edit_session_open_ = false;
    std::string default_signature_;
    /// Signature of a pending block's in-memory resolution, shown while the
    /// printer is not READY. Never persisted.
    std::string provisional_signature_;
    std::string default_hash_;
    std::string grid_signature_;
    /// Arrangements for grids that are not active, keyed by signature. Each
    /// value has the same shape as the active layout's persisted form.
    nlohmann::json parked_grids_ = nlohmann::json::object();
    bool legacy_units_ = false;
    int legacy_rows_ = 0;

    /// The save save_soon() holds: its timer and the path it writes to. A copy
    /// holds none, so a copied config never writes for the original; the
    /// destructor cancels the timer, whose callback holds a raw `this`.
    struct DeferredSave {
        lv_timer_t* timer = nullptr;
        std::string path;
        DeferredSave() = default;
        DeferredSave(const DeferredSave&) {}
        DeferredSave& operator=(const DeferredSave&) {
            return *this;
        }
        ~DeferredSave();
        void cancel();
    };
    DeferredSave deferred_save_;

    /// Write the current state to @p panel_path and save the config file.
    void write_to(const std::string& panel_path);

    std::vector<PanelWidgetEntry> build_defaults() const;

    /// The resolver's answer for the current tier, variant, preset and
    /// capabilities on a @p grid_cols x @p grid_rows grid.
    static helix::layout::ResolvedDefault resolve_default(int grid_cols, int grid_rows,
                                                          const nlohmann::json* overlay);

    /// Record the current pages as the defaults builder's output for
    /// @p signature ("" while no grid has been measured).
    void stamp_default(std::string signature);

    /// Replace page 0 with @p rebuilt, the builder's output for a @p cols x
    /// @p rows grid, carrying per-widget config across for ids that survive,
    /// and stamp it with @p signature as that grid's arrangement. Does not save.
    void install_default(std::vector<PanelWidgetEntry> rebuilt, std::string signature, int cols,
                         int rows);

    /// True when the arrangement parked for grid @p signature is one the user
    /// made rather than an unedited default.
    bool parked_layout_is_users(const std::string& signature) const;

    /// Read the `default` block (or the older `anchors: "pending"` key) from a
    /// serialize_pages() payload or a saved panel node; clears it when absent.
    void read_default_block(const nlohmann::json& node);

    /// The pages payload as it is persisted. Shared by save() and by
    /// switch_to_grid(), which parks exactly what save() would have written.
    nlohmann::json serialize_pages() const;

    /// Replace pages_ from a payload produced by serialize_pages().
    void restore_pages(const nlohmann::json& payload);

    /// True when some page of a restore_pages() payload holds an entry
    /// parse_widget_array() keeps as a known widget, the registry's appended
    /// defaults aside.
    bool payload_holds_a_widget(const nlohmann::json& payload);

    /// Reseat every page's placements, counted against an @p old_cols x
    /// @p old_rows grid, onto a @p cols x @p rows grid through
    /// port_legacy_layout(). An entry the port cannot seat is left unplaced.
    /// Returns how many were seated.
    int port_pages(int old_cols, int old_rows, int cols, int rows);

    /// Parse a JSON array of widget entries into a vector, applying migrations.
    /// If append_registry_defaults is true, appends missing registry widgets.
    /// An entry whose id has no definition is dropped, or appended verbatim to
    /// @p retained when given, so a layout survives a missing plugin.
    std::vector<PanelWidgetEntry> parse_widget_array(const nlohmann::json& arr,
                                                     bool append_registry_defaults = true,
                                                     nlohmann::json* retained = nullptr);

    /// build_default_grid() with this config's preset seed overlay applied, so a seeded default
    /// stays seeded across Reset and pending anchors. reresolve_default_if_changed() applies the
    /// same overlay itself, since it resolves the signature before building.
    std::vector<PanelWidgetEntry> build_default_grid_with_seed(int grid_cols, int grid_rows,
                                                               std::string* signature) const;

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
/// by breakpoint name and resolve through layout::breakpoint_fallback_chain().
/// Exported so the shipped-table tests resolve keys through the same function
/// the loader uses.
const char* choose_breakpoint_key(const nlohmann::json& by_bp, UiBreakpoint breakpoint);

} // namespace helix
