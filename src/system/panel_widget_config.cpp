// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_config.h"

#include "config.h"
#include "data_root_resolver.h"
#include "grid_layout.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "json_utils.h"
#include "layout_manager.h"
#include "layout_port.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <hv/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

namespace helix {

PanelWidgetConfig::PanelWidgetConfig(const std::string& panel_id, Config& config)
    : panel_id_(panel_id), config_(config) {}

std::vector<PanelWidgetEntry> PanelWidgetConfig::parse_widget_array(const nlohmann::json& arr,
                                                                    bool append_registry_defaults) {
    std::vector<PanelWidgetEntry> result;
    std::set<std::string> seen_ids;

    for (const auto& item : arr) {
        if (!item.is_object() || !item.contains("id") || !item.contains("enabled")) {
            continue;
        }

        // Validate field types before extraction
        if (!item["id"].is_string() || !item["enabled"].is_boolean()) {
            spdlog::debug(
                "[PanelWidgetConfig] Skipping malformed widget entry (wrong field types)");
            continue;
        }

        std::string id = item["id"].get<std::string>();

        // Migration: favorite_macro_N -> favorite_macro:N
        {
            static const std::string prefix = "favorite_macro_";
            if (id.size() > prefix.size() && id.substr(0, prefix.size()) == prefix) {
                auto suffix = id.substr(prefix.size());
                bool all_digits =
                    !suffix.empty() && std::all_of(suffix.begin(), suffix.end(),
                                                   [](char c) { return c >= '0' && c <= '9'; });
                if (all_digits) {
                    std::string new_id = "favorite_macro:" + suffix;
                    spdlog::info("[PanelWidgetConfig] Migrating '{}' -> '{}'", id, new_id);
                    id = new_id;
                }
            }
        }

        // Migration: spoolman -> active_spool (widget shows any active material, not just Spoolman)
        if (id == "spoolman") {
            spdlog::info("[PanelWidgetConfig] Migrating 'spoolman' -> 'active_spool'");
            id = "active_spool";
        }

        bool enabled = item["enabled"].get<bool>();

        // Skip duplicates
        if (seen_ids.count(id) > 0) {
            spdlog::debug("[PanelWidgetConfig] Skipping duplicate widget ID: {}", id);
            continue;
        }

        // Skip unknown widget IDs (not in registry)
        if (find_widget_def(id) == nullptr) {
            spdlog::debug("[PanelWidgetConfig] Dropping unknown widget ID: {}", id);
            continue;
        }

        // Load optional per-widget config. Default to {} rather than the
        // default-constructed JSON null so downstream set_config() implementations
        // can use .value("key", default) without guarding against null. Layouts
        // written before a widget gained config fields omit "config" entirely;
        // that path used to ship a JSON null and crash the widget on lookup
        // (json::type_error::306, regression introduced in v0.99.54 by 5ac58e051).
        nlohmann::json widget_config = nlohmann::json::object();
        if (item.contains("config") && item["config"].is_object()) {
            widget_config = item["config"];
        }

        // Load grid placement coordinates (default to -1 = auto-place).
        // A missing span takes the registry default, not 1: an entry written
        // before the grid moved to half-cell tracks carries no spans at all,
        // and one track is a quarter of the area the widget expects.
        const auto* span_def = find_widget_def(id);
        int col = -1;
        int row_val = -1;
        int colspan = span_def ? span_def->colspan : 1;
        int rowspan = span_def ? span_def->rowspan : 1;
        if (item.contains("col") && item["col"].is_number_integer()) {
            col = item["col"].get<int>();
        }
        if (item.contains("row") && item["row"].is_number_integer()) {
            row_val = item["row"].get<int>();
        }
        if (item.contains("colspan") && item["colspan"].is_number_integer()) {
            colspan = item["colspan"].get<int>();
        }
        if (item.contains("rowspan") && item["rowspan"].is_number_integer()) {
            rowspan = item["rowspan"].get<int>();
        }

        seen_ids.insert(id);
        result.push_back({id, enabled, widget_config, col, row_val, colspan, rowspan});
    }

    // Append any new widgets from registry that are not in saved config
    // (only for primary/main page — secondary pages are user-curated)
    if (append_registry_defaults) {
        for (const auto& def : get_all_widget_defs()) {
            if (seen_ids.count(def.id) == 0) {
                spdlog::debug("[PanelWidgetConfig] Appending new widget: {} (default_enabled={})",
                              def.id, def.default_enabled);
                result.push_back(
                    {def.id, def.default_enabled, {}, -1, -1, def.colspan, def.rowspan});
            }
        }
    }

    return result;
}

void PanelWidgetConfig::load() {
    if (loaded_) {
        return;
    }
    pages_.clear();
    main_page_index_ = 0;
    next_page_id_ = 1;
    pending_anchors_ = false;
    grid_signature_.clear();
    parked_grids_ = json::object();
    legacy_units_ = false;
    legacy_rows_ = 0;

    // Per-panel path: /printers/{active}/panel_widgets/<panel_id>
    std::string panel_path = config_.df() + "panel_widgets/" + panel_id_;
    auto saved = config_.get<json>(panel_path, json());

    // Migration: move legacy "home_widgets" to "panel_widgets.home"
    if (panel_id_ == "home" && (saved.is_null() || (!saved.is_array() && !saved.is_object()))) {
        auto legacy = config_.get<json>("/home_widgets", json());
        if (legacy.is_array() && !legacy.empty()) {
            spdlog::info("[PanelWidgetConfig] Migrating legacy home_widgets to panel_widgets.home");
            config_.set<json>(panel_path, legacy);
            // Remove legacy key
            config_.get_json("").erase("home_widgets");
            config_.save();
            saved = legacy;
        }
    }

    // Format detection: object with "pages" key = new multi-page format
    if (saved.is_object() && saved.contains("pages") && saved["pages"].is_array()) {
        // New multi-page format.
        //
        // settings.json is hand-editable, so every read here goes through the
        // safe_* helpers: .value() throws type_error.302 on a key that is
        // PRESENT with a null value (missing keys are fine), and one such key
        // would abort the whole panel load instead of costing one field.
        main_page_index_ =
            static_cast<size_t>(helix::json_util::safe_int(saved, "main_page_index"));
        next_page_id_ = helix::json_util::safe_int(saved, "next_page_id");

        // Two independent tags, each naming work that config load cannot do
        // because no grid has been measured yet. They never coexist on one
        // layout — a pre-v22 layout is not a freshly-defaulted one — but both
        // are read here so whichever is present survives to populate.
        //
        // Defaults written before any grid was measured. Compared against the
        // one value build_defaults() writes, so anything else — including the
        // key being absent, which is every layout that predates this — reads as
        // "already placed" and is left alone.
        pending_anchors_ = helix::json_util::safe_string(saved, "anchors") == "pending";

        // Which grid `pages` counts against, and the arrangements parked for
        // other grids. Absent on every layout written before per-grid storage,
        // which reads as "not stamped yet" and is handled by switch_to_grid().
        grid_signature_ = helix::json_util::safe_string(saved, "grid");
        if (auto p = saved.find("parked_grids"); p != saved.end() && p->is_object()) {
            parked_grids_ = *p;
        }

        // Pre-v22 coordinates, tagged by the migration for PanelWidgetManager to
        // port once a measured grid exists. Compared against the one value the
        // migration writes, so a hand-edit of anything else reads as "already
        // ported" and leaves the layout alone rather than porting it twice.
        legacy_units_ = helix::json_util::safe_string(saved, "layout_units") == "cells_v21";
        legacy_rows_ = helix::json_util::safe_int(saved, "legacy_rows");

        size_t page_idx = 0;
        for (const auto& page_json : saved["pages"]) {
            // Degrade per-page, not per-dashboard. `"pages": ["main"]` — an
            // easy hand-edit slip — makes page_json a string, and .value() on a
            // non-object throws type_error.306.
            if (!page_json.is_object()) {
                spdlog::warn("[PanelWidgetConfig] Skipping non-object page entry at index {}",
                             page_idx);
                ++page_idx;
                continue;
            }
            PageConfig page;
            page.id = helix::json_util::safe_string(page_json, "id");
            if (page_json.contains("widgets") && page_json["widgets"].is_array()) {
                // Only append registry defaults for the first page (main/default
                // page). Keyed off pages_ rather than page_idx so a skipped
                // leading entry doesn't cost the first real page its defaults.
                bool append_defaults = pages_.empty();
                page.widgets = parse_widget_array(page_json["widgets"], append_defaults);
            }
            pages_.push_back(std::move(page));
            ++page_idx;
        }

        // Default next_page_id to pages_.size() if missing/zero
        if (next_page_id_ <= 0) {
            next_page_id_ = static_cast<int>(pages_.size());
        }

        // Clamp main_page_index
        if (main_page_index_ >= pages_.size()) {
            main_page_index_ = 0;
        }

        // Ensure at least one page
        if (pages_.empty()) {
            PageConfig page;
            page.id = "main";
            page.widgets = build_defaults();
            pending_anchors_ = true;
            pages_.push_back(std::move(page));
            save();
        }

        if (panel_id_ == "home" && migrate_stuck_ams_filament_swap()) {
            save();
        }

        loaded_ = true;
        return;
    }

    // Legacy format: flat JSON array or null/missing
    if (saved.is_array()) {
        auto entries = parse_widget_array(saved);

        if (entries.empty()) {
            entries = build_defaults();
            pending_anchors_ = true;
        } else {
            // If no entries have grid positions, this is a pre-grid config — reset to defaults.
            bool has_any_grid =
                std::any_of(entries.begin(), entries.end(),
                            [](const PanelWidgetEntry& e) { return e.has_grid_position(); });
            if (!has_any_grid) {
                spdlog::info("[PanelWidgetConfig] Pre-grid config detected, resetting to default "
                             "grid for '{}'",
                             panel_id_);
                entries = build_defaults();
                pending_anchors_ = true;
            }
        }

        // Wrap in a single page
        PageConfig page;
        page.id = "main";
        page.widgets = std::move(entries);
        pages_.push_back(std::move(page));
        next_page_id_ = 1;

        if (panel_id_ == "home") {
            migrate_stuck_ams_filament_swap();
        }

        // Migrate to new format on disk
        save();
        loaded_ = true;
        return;
    }

    // No saved config — try a preset-shipped seed first, then fall back to
    // breakpoint-driven defaults.
    if (try_populate_from_preset_seed()) {
        save(); // Persist seeded layout so user edits survive reload
        loaded_ = true;
        return;
    }

    PageConfig page;
    page.id = "main";
    page.widgets = build_defaults();
    pending_anchors_ = true;
    pages_.push_back(std::move(page));
    next_page_id_ = 1;
    save(); // Persist default grid positions for future launches
    loaded_ = true;
}

bool PanelWidgetConfig::try_populate_from_preset_seed() {
    std::string preset = config_.get_preset();
    if (preset.empty()) {
        return false;
    }

    std::string rel_path = "panel_widgets/" + preset + "/" + panel_id_ + ".json";
    std::string seed_path = helix::find_readable(rel_path);
    std::ifstream in(seed_path);
    if (!in.is_open()) {
        return false;
    }

    nlohmann::json seed;
    try {
        seed = nlohmann::json::parse(in);
    } catch (const std::exception& e) {
        spdlog::warn("[PanelWidgetConfig] Failed to parse preset seed '{}': {}", seed_path,
                     e.what());
        return false;
    }

    if (!seed.is_object() || !seed.contains("pages") || !seed["pages"].is_array()) {
        spdlog::warn("[PanelWidgetConfig] Preset seed '{}' missing 'pages' array", seed_path);
        return false;
    }

    // The try above covers only json::parse — these reads were outside it. Same
    // null/non-object hazard as the settings.json path in load(); see the
    // comments there. A seed that fails here would otherwise unwind out of
    // load(), taking dashboard construction with it.
    main_page_index_ = static_cast<size_t>(helix::json_util::safe_int(seed, "main_page_index"));
    next_page_id_ = helix::json_util::safe_int(seed, "next_page_id");

    size_t page_idx = 0;
    for (const auto& page_json : seed["pages"]) {
        if (!page_json.is_object()) {
            spdlog::warn("[PanelWidgetConfig] Preset seed '{}': skipping non-object page entry "
                         "at index {}",
                         seed_path, page_idx);
            ++page_idx;
            continue;
        }
        PageConfig page;
        page.id = helix::json_util::safe_string(page_json, "id");
        if (page_json.contains("widgets") && page_json["widgets"].is_array()) {
            bool append_defaults = pages_.empty();
            page.widgets = parse_widget_array(page_json["widgets"], append_defaults);
        }
        pages_.push_back(std::move(page));
        ++page_idx;
    }

    if (pages_.empty()) {
        return false;
    }
    if (next_page_id_ <= 0) {
        next_page_id_ = static_cast<int>(pages_.size());
    }
    if (main_page_index_ >= pages_.size()) {
        main_page_index_ = 0;
    }

    spdlog::info("[PanelWidgetConfig] Seeded panel '{}' from preset '{}' ({})", panel_id_, preset,
                 seed_path);
    return true;
}

nlohmann::json PanelWidgetConfig::serialize_pages() const {
    json pages_json = json::array();
    for (const auto& page : pages_) {
        json page_obj;
        page_obj["id"] = page.id;

        json widgets_array = json::array();
        for (const auto& entry : page.widgets) {
            json item = {{"id", entry.id}, {"enabled", entry.enabled}};
            if (!entry.config.empty()) {
                item["config"] = entry.config;
            }
            // Always write grid coordinates so auto-placed positions survive reload
            item["col"] = entry.col;
            item["row"] = entry.row;
            item["colspan"] = entry.colspan;
            item["rowspan"] = entry.rowspan;
            widgets_array.push_back(std::move(item));
        }
        page_obj["widgets"] = std::move(widgets_array);
        pages_json.push_back(std::move(page_obj));
    }

    json out;
    out["pages"] = std::move(pages_json);
    out["main_page_index"] = main_page_index_;
    out["next_page_id"] = next_page_id_;
    return out;
}

void PanelWidgetConfig::save() {
    json root = serialize_pages();
    // Has to survive a save that lands before the anchors are applied: the
    // placement engine writes auto-placed positions back on every populate, so
    // dropping the tag here would make an unanchored default look arranged.
    if (pending_anchors_) {
        root["anchors"] = "pending";
    }
    // Survives a save that happens before the port has run — the placement
    // engine writes back auto-placed positions on every populate, so dropping
    // the tag here would silently re-read cell coordinates as tracks.
    if (legacy_units_) {
        root["layout_units"] = "cells_v21";
        root["legacy_rows"] = legacy_rows_;
    }
    // The grid these coordinates count against, and the arrangements belonging
    // to grids that are not active. Both omitted while empty so a single-grid
    // config — every printer — is byte-identical to what it wrote before.
    if (!grid_signature_.empty()) {
        root["grid"] = grid_signature_;
    }
    if (!parked_grids_.empty()) {
        root["parked_grids"] = parked_grids_;
    }

    config_.set<json>(config_.df() + "panel_widgets/" + panel_id_, root);
    config_.save();
}

void PanelWidgetConfig::clear_legacy_units() {
    if (!legacy_units_) {
        return;
    }
    legacy_units_ = false;
    legacy_rows_ = 0;
    save();
}

void PanelWidgetConfig::reorder(size_t from_index, size_t to_index) {
    auto& e = pages_[0].widgets;
    if (from_index >= e.size() || to_index >= e.size()) {
        return;
    }
    if (from_index == to_index) {
        return;
    }

    // Extract element, then insert at new position
    auto entry = std::move(e[from_index]);
    e.erase(e.begin() + static_cast<ptrdiff_t>(from_index));
    e.insert(e.begin() + static_cast<ptrdiff_t>(to_index), std::move(entry));
}

void PanelWidgetConfig::set_enabled(size_t index, bool enabled) {
    auto& e = pages_[0].widgets;
    if (index >= e.size()) {
        return;
    }
    e[index].enabled = enabled;
}

bool PanelWidgetConfig::set_enabled_by_id(const std::string& id, bool enabled) {
    for (auto& page : pages_) {
        auto it = std::find_if(page.widgets.begin(), page.widgets.end(),
                               [&id](const PanelWidgetEntry& e) { return e.id == id; });
        if (it != page.widgets.end()) {
            it->enabled = enabled;
            return true;
        }
    }
    return false;
}

void PanelWidgetConfig::reset_to_defaults() {
    // Reset page 0 to defaults, remove all other pages
    pages_.resize(1);
    pages_[0].id = "main";
    pages_[0].widgets = build_defaults();
    pending_anchors_ = true;
    main_page_index_ = 0;
    next_page_id_ = 1;
}

std::string PanelWidgetConfig::mint_instance_id(const std::string& base_id) {
    int max_n = 0;
    std::string prefix = base_id + ":";

    // Scan ALL pages for existing instance IDs
    for (const auto& page : pages_) {
        for (const auto& entry : page.widgets) {
            if (entry.id.size() > prefix.size() && entry.id.substr(0, prefix.size()) == prefix) {
                auto suffix = entry.id.substr(prefix.size());
                try {
                    int n = std::stoi(suffix);
                    if (n > max_n)
                        max_n = n;
                } catch (...) {
                }
            }
        }
    }
    return base_id + ":" + std::to_string(max_n + 1);
}

void PanelWidgetConfig::delete_entry(const std::string& id) {
    // Search all pages, remove first match
    for (auto& page : pages_) {
        auto it = std::find_if(page.widgets.begin(), page.widgets.end(),
                               [&id](const PanelWidgetEntry& e) { return e.id == id; });
        if (it != page.widgets.end()) {
            page.widgets.erase(it);
            return;
        }
    }
}

bool PanelWidgetConfig::is_enabled(const std::string& id) const {
    for (const auto& page : pages_) {
        auto it = std::find_if(page.widgets.begin(), page.widgets.end(),
                               [&id](const PanelWidgetEntry& e) { return e.id == id; });
        if (it != page.widgets.end()) {
            return it->enabled;
        }
    }
    return false;
}

bool PanelWidgetConfig::is_placed(const std::string& id) const {
    for (const auto& page : pages_) {
        auto it = std::find_if(page.widgets.begin(), page.widgets.end(),
                               [&id](const PanelWidgetEntry& e) { return e.id == id; });
        if (it != page.widgets.end()) {
            return it->enabled && it->has_grid_position();
        }
    }
    return false;
}

nlohmann::json PanelWidgetConfig::get_widget_config(const std::string& id) const {
    for (const auto& page : pages_) {
        auto it = std::find_if(page.widgets.begin(), page.widgets.end(),
                               [&id](const PanelWidgetEntry& e) { return e.id == id; });
        if (it != page.widgets.end() && !it->config.empty()) {
            return it->config;
        }
    }
    return nlohmann::json::object();
}

void PanelWidgetConfig::set_widget_config(const std::string& id, const nlohmann::json& config) {
    for (auto& page : pages_) {
        auto it = std::find_if(page.widgets.begin(), page.widgets.end(),
                               [&id](const PanelWidgetEntry& e) { return e.id == id; });
        if (it != page.widgets.end()) {
            it->config = config;
            save();
            return;
        }
    }
    spdlog::debug("[PanelWidgetConfig] set_widget_config: widget '{}' not found", id);
}

int PanelWidgetConfig::add_page(const std::string& name) {
    if (pages_.size() >= MAX_PAGES) {
        spdlog::warn("[PanelWidgetConfig] Cannot add page: at maximum ({} pages)", MAX_PAGES);
        return -1;
    }

    PageConfig page;
    page.id = name.empty() ? generate_page_id() : name;
    pages_.push_back(std::move(page));
    return static_cast<int>(pages_.size() - 1);
}

bool PanelWidgetConfig::remove_page(size_t page_index) {
    if (pages_.size() <= 1) {
        spdlog::warn("[PanelWidgetConfig] Cannot remove last page");
        return false;
    }
    if (page_index >= pages_.size()) {
        return false;
    }
    if (page_index == main_page_index_) {
        spdlog::warn("[PanelWidgetConfig] Cannot remove main page (index {})", page_index);
        return false;
    }

    pages_.erase(pages_.begin() + static_cast<ptrdiff_t>(page_index));

    // Adjust main_page_index
    if (main_page_index_ > page_index) {
        --main_page_index_;
    }

    return true;
}

std::string PanelWidgetConfig::generate_page_id() {
    return "page_" + std::to_string(next_page_id_++);
}

/// Declared in panel_widget_config.h so the shipped-table tests resolve keys
/// through this function rather than reimplementing the fallback chain.
const char* choose_breakpoint_key(const nlohmann::json& by_bp, UiBreakpoint breakpoint) {
    // Fallback chain: micro→tiny→small, xlarge→large (matches theme_manager)
    static const char* fallback_order[][3] = {
        {"micro", "tiny", "small"},     // bp=0 Micro
        {"tiny", "small", nullptr},     // bp=1 Tiny
        {"small", nullptr},             // bp=2 Small
        {"medium", nullptr},            // bp=3 Medium
        {"large", nullptr},             // bp=4 Large
        {"xlarge", "large", nullptr},   // bp=5 XLarge
        {"xxlarge", "xlarge", "large"}, // bp=6 XXLarge
    };
    static_assert(std::size(fallback_order) == to_int(UiBreakpoint::XXLarge) + 1,
                  "fallback_order must cover every UiBreakpoint tier");

    const int bp_idx = to_int(breakpoint);
    if (bp_idx < 0 || bp_idx >= static_cast<int>(std::size(fallback_order))) {
        return nullptr;
    }
    for (int i = 0; i < 3 && fallback_order[bp_idx][i]; ++i) {
        if (by_bp.contains(fallback_order[bp_idx][i])) {
            return fallback_order[bp_idx][i];
        }
    }
    return nullptr;
}

namespace {

/// Placement key for a tier on a measured grid, or "" when nothing matches.
///
/// Walks the same tier fallback chain, but tries the grid-qualified form
/// ("<tier>@<cols>x<rows>") before the bare tier name at each rung. Tier
/// specificity still dominates: a table entry for this tier on some other grid
/// beats one for a coarser tier on this exact grid, because the tier is what
/// decides how much text has to fit. The grid breaks ties within a tier, which
/// is the case the UI scale creates - one panel, one tier, a different track
/// count per scale.
///
/// With @p grid_cols / @p grid_rows at 0 this is exactly choose_breakpoint_key,
/// so a caller that cannot measure a grid resolves as it always did.
std::string choose_placement_key(const nlohmann::json& by_bp, UiBreakpoint breakpoint,
                                 int grid_cols, int grid_rows) {
    static const char* fallback_order[][3] = {
        {"micro", "tiny", "small"},     // bp=0 Micro
        {"tiny", "small", nullptr},     // bp=1 Tiny
        {"small", nullptr},             // bp=2 Small
        {"medium", nullptr},            // bp=3 Medium
        {"large", nullptr},             // bp=4 Large
        {"xlarge", "large", nullptr},   // bp=5 XLarge
        {"xxlarge", "xlarge", "large"}, // bp=6 XXLarge
    };
    static_assert(std::size(fallback_order) == to_int(UiBreakpoint::XXLarge) + 1,
                  "fallback_order must cover every UiBreakpoint tier");

    const int bp_idx = to_int(breakpoint);
    if (bp_idx < 0 || bp_idx >= static_cast<int>(std::size(fallback_order))) {
        return {};
    }

    const bool have_grid = grid_cols > 0 && grid_rows > 0;
    const std::string suffix =
        have_grid ? "@" + std::to_string(grid_cols) + "x" + std::to_string(grid_rows)
                  : std::string{};

    for (int i = 0; i < 3 && fallback_order[bp_idx][i]; ++i) {
        const std::string tier = fallback_order[bp_idx][i];
        if (have_grid && by_bp.contains(tier + suffix)) {
            return tier + suffix;
        }
        if (by_bp.contains(tier)) {
            return tier;
        }
    }
    return {};
}

/// One axis of a "<cols>x<rows>" signature, or 0 when it does not parse.
int parse_grid_axis(const std::string& signature, bool want_cols) {
    const auto x = signature.find('x');
    if (x == std::string::npos) {
        return 0;
    }
    const std::string part = want_cols ? signature.substr(0, x) : signature.substr(x + 1);
    if (part.empty() || part.find_first_not_of("0123456789") != std::string::npos) {
        return 0;
    }
    return std::atoi(part.c_str());
}

/// Whether an anchor's rectangle lies wholly inside a track grid.
bool anchor_fits_grid(int col, int row, int colspan, int rowspan, int cols, int rows) {
    return col >= 0 && row >= 0 && colspan >= 1 && rowspan >= 1 && col + colspan <= cols &&
           row + rowspan <= rows;
}

/// Read a table's `disabled` map — `{ "<breakpoint>": ["id", ...] }` — into `out`.
///
/// The base table and every variant carry the same optional key, so both go
/// through this rather than the variant branch owning a private copy. `table` is
/// the object holding `anchors`: the top-level document for the base table, the
/// variant object otherwise.
void collect_disabled(const nlohmann::json& table, UiBreakpoint breakpoint,
                      std::set<std::string>& out) {
    auto d = table.find("disabled");
    if (d == table.end() || !d->is_object()) {
        return;
    }
    const char* key = choose_breakpoint_key(*d, breakpoint);
    if (!key) {
        return;
    }
    const auto& ids = (*d)[key];
    if (!ids.is_array()) {
        return;
    }
    for (const auto& id : ids) {
        if (id.is_string()) {
            out.insert(id.get<std::string>());
        }
    }
}

} // namespace

std::vector<PanelWidgetEntry> PanelWidgetConfig::build_default_grid(int grid_cols, int grid_rows) {
    const auto& defs = get_all_widget_defs();

    // Determine current breakpoint for per-breakpoint anchor sizing
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint breakpoint = bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj))
                                      : UiBreakpoint::Medium; // Default MEDIUM

    // One name per UiBreakpoint tier. Indexed by the enum, so it must stay
    // seven long: XXLarge is 6 and a six-entry table read past its end.
    static const char* bp_names[] = {"micro", "tiny",   "small",  "medium",
                                     "large", "xlarge", "xxlarge"};
    static_assert(std::size(bp_names) == to_int(UiBreakpoint::XXLarge) + 1,
                  "bp_names must cover every UiBreakpoint tier");
    const char* bp_name = bp_names[to_int(breakpoint)];

    // Anchor tables are keyed by layout variant in the same most-specific-first
    // order LayoutManager uses for ui_xml/ overrides: "variants": { "portrait":
    // [...] } wins over the top-level (landscape) "anchors" on a portrait panel,
    // and TINY_PORTRAIT falls through tiny_portrait → portrait → base.
    //
    // Without this a 480x800 portrait panel — cramped axis 480, i.e. breakpoint
    // MEDIUM — got the LANDSCAPE medium anchors, authored against a 6-column
    // grid. `tips` (col 2, colspan 4) and the col-3 temperature anchors do not
    // exist on a 3-column portrait grid, so three of five anchors were
    // unreachable and silently fell through to auto-place (#1216).
    const bool portrait = is_portrait_layout(LayoutManager::instance().type());

    // Load anchor placements from config/default_layout.json (runtime-editable).
    // Falls back to registry defaults if file is missing or malformed.
    struct AnchorPlacement {
        std::string id;
        int col, row, colspan, rowspan;
        nlohmann::json config; // per-widget config carried by the anchor, may be null
    };
    std::vector<AnchorPlacement> anchors;

    // Widgets the chosen variant switches off at this breakpoint. Leaving a
    // widget out of the anchor list does NOT disable it — parse_widget_array()
    // appends every registry widget that is missing, at its default_enabled — so
    // "not on this tier" needs saying explicitly.
    std::set<std::string> disabled_ids;

    std::ifstream layout_file(helix::find_readable("default_layout.json"));
    if (layout_file.is_open()) {
        try {
            nlohmann::json layout = nlohmann::json::parse(layout_file);
            // find + is_array rather than .value("anchors", array()): default_
            // layout.json is runtime-editable, and .value() throws type_error
            // .302 on a key present with a null value. The catch below would
            // turn that into "no anchors at all" — every anchor lost to one bad
            // key. Each read below degrades to its own default instead.
            auto anchors_it = layout.is_object() ? layout.find("anchors") : layout.end();
            const nlohmann::json empty_array = nlohmann::json::array();
            const nlohmann::json* anchor_list =
                (anchors_it != layout.end() && anchors_it->is_array()) ? &*anchors_it
                                                                       : &empty_array;

            // Most-specific variant table wins; the base "anchors" array is the
            // final fallback, exactly as ui_xml/ resolves an override.
            //
            // A variant is either a bare anchor array (the original shape) or an
            // object carrying "anchors" plus optional "disabled". Both are
            // accepted: the file is runtime-editable and shipped copies predate
            // the object form.
            std::string variant_used = "base";
            auto variants_it = layout.is_object() ? layout.find("variants") : layout.end();
            if (variants_it != layout.end() && variants_it->is_object()) {
                for (const auto& dir : LayoutManager::instance().variant_chain()) {
                    auto v = variants_it->find(dir);
                    if (v == variants_it->end()) {
                        continue;
                    }
                    if (v->is_array() && !v->empty()) {
                        anchor_list = &*v;
                        variant_used = dir;
                        break;
                    }
                    if (!v->is_object()) {
                        continue;
                    }
                    auto va = v->find("anchors");
                    if (va == v->end() || !va->is_array() || va->empty()) {
                        continue;
                    }
                    anchor_list = &*va;
                    variant_used = dir;

                    collect_disabled(*v, breakpoint, disabled_ids);
                    break;
                }
            }
            // Whichever table won owns the disable list. Without the base case a
            // landscape tier could never switch a widget off: LayoutType
            // STANDARD has an empty variant chain, so no variant object is ever
            // consulted, and leaving a widget out of the anchors does not
            // disable it — parse_widget_array() appends it at its registry
            // default_enabled.
            if (variant_used == "base") {
                collect_disabled(layout, breakpoint, disabled_ids);
            }

            for (const auto& anchor : *anchor_list) {
                if (!anchor.is_object())
                    continue;
                std::string id = helix::json_util::safe_string(anchor, "id");
                const auto* def = id.empty() ? nullptr : find_widget_def(id);
                if (!def)
                    continue;

                auto placements_it = anchor.find("placements");
                if (placements_it == anchor.end() || !placements_it->is_object())
                    continue;
                const nlohmann::json& placements = *placements_it;

                const std::string chosen_name =
                    choose_placement_key(placements, breakpoint, grid_cols, grid_rows);
                if (!chosen_name.empty()) {
                    // find, not operator[]: `placements` is a const reference
                    // now, and const operator[] on a missing key is only
                    // JSON_ASSERT-guarded — under NDEBUG it dereferences end().
                    auto p_it = placements.find(chosen_name);
                    if (p_it == placements.end() || !p_it->is_object())
                        continue;
                    const nlohmann::json& p = *p_it;
                    // A placement that omits a span takes the registry span, not
                    // one track: one track is a quarter of the area every
                    // widget now declares as its minimum, and this file is
                    // runtime-editable, so a hand-authored placement can leave
                    // the spans out. Matches parse_widget_array().
                    //
                    // "config" rides on the anchor so a default layout can pick a
                    // widget's variant — portrait print_status ships Detailed —
                    // without a C++ branch per widget. Anchor-level first, then
                    // placement-level so a single tier can override.
                    nlohmann::json cfg;
                    if (auto ac = anchor.find("config"); ac != anchor.end() && ac->is_object()) {
                        cfg = *ac;
                    }
                    if (auto pc = p.find("config"); pc != p.end() && pc->is_object()) {
                        for (auto it2 = pc->begin(); it2 != pc->end(); ++it2) {
                            cfg[it2.key()] = it2.value();
                        }
                    }
                    anchors.push_back({id, helix::json_util::safe_int(p, "col", 0),
                                       helix::json_util::safe_int(p, "row", 0),
                                       helix::json_util::safe_int(p, "colspan", def->colspan),
                                       helix::json_util::safe_int(p, "rowspan", def->rowspan),
                                       std::move(cfg)});
                }
            }
            spdlog::debug(
                "[PanelWidgetConfig] Loaded {} anchors from default_layout.json (bp={}, table={})",
                anchors.size(), bp_name, variant_used);
        } catch (const std::exception& e) {
            spdlog::warn("[PanelWidgetConfig] Failed to parse default_layout.json: {}", e.what());
            anchors.clear();
        }
    }

    // Fallback: if no anchors loaded, use hardcoded defaults so the dashboard
    // always has printer_image, print_status, and tips placed sensibly.
    if (anchors.empty()) {
        spdlog::debug("[PanelWidgetConfig] Using hardcoded anchor fallback (bp={}, {})", bp_name,
                      portrait ? "portrait" : "landscape");
        if (portrait) {
            anchors = {
                {"printer_image", 0, 0, 8, 4},
                {"print_status", 0, 4, 8, 4},
            };
        } else {
            anchors = {
                {"printer_image", 0, 0, 4, 4},
                {"print_status", 0, 4, 4, 4},
                {"tips", 4, 0, 4, 4},
            };
        }
    }

    // Drop anchors the measured grid cannot hold. Keeping one would hand it to
    // clamp_to_grid(), which shrinks the span and walks the origin back until it
    // fits — on a grid narrower than the table assumed that lands several
    // widgets on the same track, each looking like a chosen position. An
    // unseated widget goes through the auto-placement pass below instead, which
    // is the honest answer: this anchor does not describe this grid.
    if (grid_cols > 0 && grid_rows > 0) {
        // Manual join — fmt::join needs fmt/ranges.h, which spdlog's bundled
        // copy does not carry.
        std::string rejected;
        int rejected_count = 0;
        anchors.erase(std::remove_if(anchors.begin(), anchors.end(),
                                     [&](const AnchorPlacement& a) {
                                         // A widget this tier switches off is never placed, so
                                         // whether its anchor fits is not a fact about anything.
                                         // `tips` on micro reaches here only because it has no
                                         // micro placement and inherits small's, which is authored
                                         // for a taller grid — warning about it would put a false
                                         // alarm in every CC1 log.
                                         if (disabled_ids.count(a.id) > 0) {
                                             return false;
                                         }
                                         if (anchor_fits_grid(a.col, a.row, a.colspan, a.rowspan,
                                                              grid_cols, grid_rows)) {
                                             return false;
                                         }
                                         if (!rejected.empty()) {
                                             rejected += ", ";
                                         }
                                         rejected += a.id;
                                         ++rejected_count;
                                         return true;
                                     }),
                      anchors.end());
        if (rejected_count > 0) {
            spdlog::warn("[PanelWidgetConfig] {} of {} default anchors do not fit the {}x{} track "
                         "grid and will be auto-placed: {}",
                         rejected_count, rejected_count + static_cast<int>(anchors.size()),
                         grid_cols, grid_rows, rejected);
        }
    }

    // Build result: anchored widgets first, then all others with auto-placement
    std::vector<PanelWidgetEntry> result;
    result.reserve(defs.size());
    std::set<std::string> fixed_ids;

    for (const auto& a : anchors) {
        if (!find_widget_def(a.id))
            continue;
        result.push_back({a.id, true, a.config.is_object() ? a.config : nlohmann::json{}, a.col,
                          a.row, a.colspan, a.rowspan});
        fixed_ids.insert(a.id);
    }

    // All other widgets: enabled/disabled per registry, no grid position.
    // Positions computed dynamically at populate time.
    // Multi-instance widgets (fan_stack, thermistor, etc.) are included once as their
    // base ID — only additional instances (fan_stack:1, fan_stack:2) are user-added.
    for (const auto& def : defs) {
        if (fixed_ids.count(def.id) > 0)
            continue;
        result.push_back({def.id, def.default_enabled, {}, -1, -1, def.colspan, def.rowspan});
    }

    // Variant-scoped disable: a widget the layout switches off at this
    // breakpoint. Applied after the registry pass so it reaches widgets that
    // were appended by default rather than anchored, and before the AMS swap so
    // that swap still has the final say on filament vs ams.
    if (!disabled_ids.empty()) {
        for (auto& entry : result) {
            if (disabled_ids.count(entry.id) == 0) {
                continue;
            }
            entry.enabled = false;
            entry.col = -1;
            entry.row = -1;
            spdlog::debug("[PanelWidgetConfig] '{}' disabled by layout variant at bp={}", entry.id,
                          bp_name);
        }
    }

    bool ams_present = false;
    int ams_slot_count = 0;
    {
        lv_subject_t* ams_subj = lv_xml_get_subject(nullptr, "ams_slot_count");
        if (ams_subj) {
            ams_slot_count = lv_subject_get_int(ams_subj);
            if (ams_slot_count > 0)
                ams_present = true;
        }
        spdlog::debug("[PanelWidgetConfig] build_default_grid: ams_slot_count={} ({})",
                      ams_slot_count, ams_present ? "AMS widget" : "filament widget");
    }

    // Filament/AMS swap: the AMS widget subsumes the role of the filament sensor
    // widget on printers with multi-material hardware, so enable one or the other.
    if (ams_present) {
        auto fil_it = std::find_if(result.begin(), result.end(),
                                   [](const PanelWidgetEntry& e) { return e.id == "filament"; });
        auto ams_it = std::find_if(result.begin(), result.end(),
                                   [](const PanelWidgetEntry& e) { return e.id == "ams"; });
        if (fil_it != result.end())
            fil_it->enabled = false;
        if (ams_it != result.end())
            ams_it->enabled = true;
    }

    // Bed temperature: move to end so it is the last widget placed.
    {
        auto it = std::find_if(result.begin(), result.end(),
                               [](const PanelWidgetEntry& e) { return e.id == "bed_temperature"; });
        if (it != result.end()) {
            auto entry = std::move(*it);
            result.erase(it);
            result.push_back(std::move(entry));
        }
    }

    // Safety: ensure at least some widgets are enabled
    bool any_enabled = std::any_of(result.begin(), result.end(),
                                   [](const PanelWidgetEntry& e) { return e.enabled; });
    if (!any_enabled) {
        spdlog::warn("[PanelWidgetConfig] No widgets enabled — enabling registry defaults");
        for (auto& entry : result) {
            const auto* def = find_widget_def(entry.id);
            if (def && def->default_enabled) {
                entry.enabled = true;
            }
        }
    }

    return result;
}

bool PanelWidgetConfig::migrate_stuck_ams_filament_swap() {
    bool mutated = false;
    for (auto& page : pages_) {
        PanelWidgetEntry* ams = nullptr;
        PanelWidgetEntry* fil = nullptr;
        for (auto& entry : page.widgets) {
            if (entry.id == "ams")
                ams = &entry;
            else if (entry.id == "filament")
                fil = &entry;
        }
        if (!ams || !fil)
            continue;
        if (!ams->enabled || ams->has_grid_position())
            continue;
        if (!fil->enabled || !fil->has_grid_position())
            continue;

        spdlog::info("[PanelWidgetConfig] Migrating stuck ams/filament on page '{}': "
                     "ams(-1,-1) + filament({},{}) → ams({},{}), filament disabled",
                     page.id, fil->col, fil->row, fil->col, fil->row);
        ams->col = fil->col;
        ams->row = fil->row;
        fil->enabled = false;
        fil->col = -1;
        fil->row = -1;
        mutated = true;
    }
    return mutated;
}

bool PanelWidgetConfig::is_grid_format() const {
    for (const auto& page : pages_) {
        if (std::any_of(page.widgets.begin(), page.widgets.end(),
                        [](const PanelWidgetEntry& e) { return e.has_grid_position(); })) {
            return true;
        }
    }
    return false;
}

std::vector<PanelWidgetEntry> PanelWidgetConfig::build_defaults() {
    return build_default_grid();
}

void PanelWidgetConfig::restore_pages(const nlohmann::json& payload) {
    pages_.clear();
    main_page_index_ = static_cast<size_t>(helix::json_util::safe_int(payload, "main_page_index"));
    next_page_id_ = helix::json_util::safe_int(payload, "next_page_id");
    if (payload.contains("pages") && payload["pages"].is_array()) {
        for (const auto& page_json : payload["pages"]) {
            if (!page_json.is_object()) {
                continue;
            }
            PageConfig page;
            page.id = helix::json_util::safe_string(page_json, "id");
            if (page_json.contains("widgets") && page_json["widgets"].is_array()) {
                page.widgets = parse_widget_array(page_json["widgets"], pages_.empty());
            }
            pages_.push_back(std::move(page));
        }
    }
    if (pages_.empty()) {
        PageConfig page;
        page.id = "main";
        page.widgets = build_defaults();
        pending_anchors_ = true;
        pages_.push_back(std::move(page));
    }
    if (main_page_index_ >= pages_.size()) {
        main_page_index_ = 0;
    }
    if (next_page_id_ <= 0) {
        next_page_id_ = static_cast<int>(pages_.size());
    }
}

void PanelWidgetConfig::switch_to_grid(int cols, int rows) {
    if (cols <= 0 || rows <= 0 || pages_.empty()) {
        return;
    }
    const std::string target = std::to_string(cols) + "x" + std::to_string(rows);
    if (grid_signature_ == target) {
        return;
    }

    // Never stamped. Which grid these coordinates were arranged on is not
    // recoverable, so the honest move is to claim the grid we are on now and
    // change nothing else — reseating a real arrangement on a guess is the
    // failure this whole change exists to stop.
    if (grid_signature_.empty()) {
        grid_signature_ = target;
        spdlog::info("[PanelWidgetConfig] '{}': layout adopted by the {} grid", panel_id_, target);
        save();
        return;
    }

    const std::string outgoing = grid_signature_;
    const int old_cols = parse_grid_axis(outgoing, /*want_cols=*/true);
    const int old_rows = parse_grid_axis(outgoing, /*want_cols=*/false);

    // Park the outgoing arrangement before touching anything.
    parked_grids_[outgoing] = serialize_pages();

    if (auto it = parked_grids_.find(target); it != parked_grids_.end() && it->is_object()) {
        restore_pages(*it);
        parked_grids_.erase(target);
        grid_signature_ = target;
        spdlog::info("[PanelWidgetConfig] '{}': restored the saved {} arrangement (was {})",
                     panel_id_, target, outgoing);
        save();
        return;
    }

    // First visit. Seed from the arrangement being left rather than from the
    // shipped defaults: it is the layout the user was last looking at, and the
    // remapper seats it on the new grid or drops a widget to auto-placement,
    // per widget, rather than failing the layout.
    int seated = 0;
    if (old_cols > 0 && old_rows > 0) {
        for (auto& page : pages_) {
            std::vector<LegacyPlacement> saved;
            saved.reserve(page.widgets.size());
            for (const auto& e : page.widgets) {
                saved.push_back({e.id, e.col, e.row, e.colspan, e.rowspan});
            }
            const auto ported = port_legacy_layout(saved, old_cols, old_rows, cols, rows);
            for (size_t i = 0; i < page.widgets.size() && i < ported.size(); ++i) {
                if (ported[i].seated) {
                    page.widgets[i].col = ported[i].col;
                    page.widgets[i].row = ported[i].row;
                    page.widgets[i].colspan = ported[i].colspan;
                    page.widgets[i].rowspan = ported[i].rowspan;
                    ++seated;
                } else {
                    page.widgets[i].col = -1;
                    page.widgets[i].row = -1;
                }
            }
        }
    }

    grid_signature_ = target;
    spdlog::info("[PanelWidgetConfig] '{}': seeded the {} grid from {} ({} widget(s) reseated, "
                 "the rest auto-placed)",
                 panel_id_, target, outgoing, seated);
    save();
}

void PanelWidgetConfig::apply_pending_anchors(int grid_cols, int grid_rows) {
    if (!pending_anchors_ || grid_cols <= 0 || grid_rows <= 0) {
        return;
    }

    // Rebuild rather than patch. The anchor table decides enable/disable per
    // tier and carries per-widget config, and a grid-qualified entry may differ
    // from the tier entry in any of those, not only in coordinates — so the
    // answer for this grid is whatever build_default_grid() says for it.
    //
    // Page 0 only. Extra pages are user-made, and a user who has built one has
    // arranged page 0 too, so a layout still tagged pending has exactly one.
    const auto rebuilt = build_default_grid(grid_cols, grid_rows);
    if (!rebuilt.empty() && !pages_.empty()) {
        pages_[0].widgets = rebuilt;
    }

    int anchored = 0;
    for (const auto& e : pages_[0].widgets) {
        if (e.has_grid_position()) {
            ++anchored;
        }
    }
    spdlog::info("[PanelWidgetConfig] '{}': applied default anchors for a {}x{} track grid, {} of "
                 "{} widget(s) placed",
                 panel_id_, grid_cols, grid_rows, anchored, pages_[0].widgets.size());

    pending_anchors_ = false;
    grid_signature_ = std::to_string(grid_cols) + "x" + std::to_string(grid_rows);
    save();
}

} // namespace helix
