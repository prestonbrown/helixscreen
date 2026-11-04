// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_config.h"

#include "ui_timer_guard.h"

#include "config.h"
#include "core/data_root_resolver.h"
#include "default_layout_resolver.h"
#include "grid_layout.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix_fs.h"
#include "helix_psram_attr.h"
#include "home_layout_share.h"
#include "json_utils.h"
#include "layout_manager.h"
#include "layout_port.h"
#include "panel_widget_registry.h"
#include "runtime_config.h"
#include "text_io.h"
#include "theme_manager.h"

#include <hv/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace helix {

namespace {

/// The config version from which a panel layout that places widgets also names
/// the system its coordinates count in — `grid` for tracks, `layout_units` for
/// pre-v22 cells. No layout below it carries either key, so the absence of both
/// says nothing there.
constexpr int kCoordinateSystemNamedFrom = 24;

} // namespace

bool PanelWidgetConfig::has_uninterpretable_coordinates(const nlohmann::json& panel_node,
                                                        int config_version) {
    if (config_version < kCoordinateSystemNamedFrom || !panel_node.is_object()) {
        return false;
    }
    auto pages = panel_node.find("pages");
    if (pages == panel_node.end() || !pages->is_array()) {
        return false;
    }
    // A `default` block (or its older spelling, "anchors": "pending") names the
    // defaults builder's own placement, saved before a grid was measured and
    // re-anchored once one is.
    if (!helix::json_util::safe_string(panel_node, "grid").empty() ||
        !helix::json_util::safe_string(panel_node, "layout_units").empty() ||
        helix::json_util::safe_string(panel_node, "anchors") == "pending") {
        return false;
    }
    if (const json* block = helix::json_util::find_member(panel_node, "default");
        block && block->is_object()) {
        return false;
    }

    // Only a placed widget carries a number whose units matter, so a layout
    // that places nothing is not this shape whatever its keys say — rebuilding
    // it would throw away deliberate hides to fix a coordinate that is not
    // there. Placed means what has_grid_position() means, read off the node
    // before any of it is parsed into entries.
    for (const auto& page : *pages) {
        if (!page.is_object()) {
            continue;
        }
        auto widgets = page.find("widgets");
        if (widgets == page.end() || !widgets->is_array()) {
            continue;
        }
        for (const auto& entry : *widgets) {
            if (entry.is_object() && helix::json_util::safe_int(entry, "col", -1) >= 0 &&
                helix::json_util::safe_int(entry, "row", -1) >= 0) {
                return true;
            }
        }
    }
    return false;
}

PanelWidgetConfig::PanelWidgetConfig(const std::string& panel_id, Config& config)
    : panel_id_(panel_id), config_(config) {}

std::vector<PanelWidgetEntry> PanelWidgetConfig::parse_widget_array(const nlohmann::json& arr,
                                                                    bool append_registry_defaults,
                                                                    nlohmann::json* retained) {
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

        // An id with no definition is either kept verbatim (a plugin widget
        // while its plugin is absent, so the placement returns when it does)
        // or dropped, when no destination was given. Recording the id here
        // also collapses a duplicate unknown entry to its first copy.
        seen_ids.insert(id);
        if (find_widget_def(id) == nullptr) {
            if (retained) {
                retained->push_back(item);
                spdlog::debug("[PanelWidgetConfig] Keeping unknown widget ID: {}", id);
            } else {
                spdlog::debug("[PanelWidgetConfig] Dropping unknown widget ID: {}", id);
            }
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
    if (!saved_layout_known_) {
        const json node = config_.get<json>(config_.df() + "panel_widgets/" + panel_id_, json());
        const json legacy =
            panel_id_ == "home" ? config_.get<json>("/home_widgets", json()) : json();
        had_saved_layout_ = ((node.is_array() || node.is_object()) && !node.empty()) ||
                            (legacy.is_array() && !legacy.empty());
        saved_layout_known_ = true;
    }
    pages_.clear();
    main_page_index_ = 0;
    next_page_id_ = 1;
    read_default_block(json());
    grid_signature_.clear();
    parked_grids_ = json::object();
    legacy_units_ = false;
    legacy_rows_ = 0;

    // Crash-loop safe mode: the saved layout may be what crashes, so this run
    // shows the shipped defaults and write_to() leaves the saved node alone.
    if (get_runtime_config()->crash_loop_safe_mode) {
        PageConfig page;
        page.id = "main";
        page.widgets = build_defaults();
        pages_.push_back(std::move(page));
        stamp_default("");
        loaded_ = true;
        return;
    }

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

    // A layout that places widgets but names no system for the coordinates is
    // dropped rather than read. Adopting it would hand switch_to_grid() numbers
    // that are not tracks and get them stamped as this grid's arrangement, so
    // the dashboard comes back rearranged with nothing left to restore it from.
    // Falling through to the fresh-install path below reseats the panel from
    // its preset, or from the shipped defaults: a dashboard that visibly
    // returns to defaults is diagnosable, one that comes back subtly wrong is
    // not.
    const int config_version = config_.get<int>("/config_version", 0);
    if (has_uninterpretable_coordinates(saved, config_version)) {
        spdlog::info("[PanelWidgetConfig] '{}': the saved layout places widgets but names no "
                     "coordinate system (config_version {}, no 'grid', no 'layout_units', {} "
                     "page(s)), so its coordinates cannot be read — rebuilding the panel from "
                     "defaults",
                     panel_id_, config_version, saved["pages"].size());
        saved = json();
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
        // The defaults builder's own arrangement, possibly still waiting for a
        // measured grid. Anything else, including the block being absent, which
        // is every layout that predates it, reads as "already placed" and is
        // left alone.
        read_default_block(saved);

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
                page.widgets =
                    parse_widget_array(page_json["widgets"], append_defaults, &page.retained);
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
        const bool defaulted = pages_.empty();
        if (defaulted) {
            PageConfig page;
            page.id = "main";
            page.widgets = build_defaults();
            pages_.push_back(std::move(page));
        }

        const bool migrated = panel_id_ == "home" && migrate_stuck_ams_filament_swap();
        if (defaulted) {
            stamp_default("");
        }
        if (defaulted || migrated) {
            save();
        }

        loaded_ = true;
        return;
    }

    // Legacy format: flat JSON array or null/missing
    if (saved.is_array()) {
        auto entries = parse_widget_array(saved);
        bool defaulted = false;

        if (entries.empty()) {
            entries = build_defaults();
            defaulted = true;
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
                defaulted = true;
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
        if (defaulted) {
            // Left pending and unsaved: the old array stays on disk until the
            // READY resolve writes the default that replaces it.
            stamp_default("");
        } else {
            // Migrate to new format on disk
            save();
        }
        loaded_ = true;
        return;
    }

    // No saved config: breakpoint-driven defaults, with the preset's seed
    // overlay applied when it ships one.
    PageConfig page;
    page.id = "main";
    page.widgets = build_defaults();
    pages_.push_back(std::move(page));
    next_page_id_ = 1;
    // Left pending and unsaved: the first READY populate resolves it from the
    // printer's own capabilities and writes it then.
    stamp_default("");
    loaded_ = true;
}

/// True when @p entry is exactly what parse_widget_array() appends for a
/// registry widget the saved page does not mention: off by default, unplaced,
/// unconfigured, at the registry spans. Only the disabled kind qualifies: an
/// enabled unplaced entry's position in the list is its auto-placement order.
static bool is_appended_registry_default(const PanelWidgetEntry& entry) {
    const auto* def = find_widget_def(entry.id);
    return def != nullptr && def->id == entry.id && !def->default_enabled && !entry.enabled &&
           entry.col < 0 && entry.row < 0 && entry.config.empty() &&
           entry.colspan == def->colspan && entry.rowspan == def->rowspan;
}

nlohmann::json PanelWidgetConfig::serialize_pages() const {
    json pages_json = json::array();
    for (const auto& page : pages_) {
        json page_obj;
        page_obj["id"] = page.id;

        // The first page is read back with the registry defaults appended, so
        // writing those defaults out would cost every printer's settings.json
        // a copy of the registry: about 2.4 KB, on a K-Touch whose whole
        // settings partition is 128 KB.
        const bool registry_appended_on_read = pages_json.empty();
        json widgets_array = json::array();
        for (const auto& entry : page.widgets) {
            if (registry_appended_on_read && is_appended_registry_default(entry)) {
                continue;
            }
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
        // The saved entries no definition vouches for ride after the known
        // ones, verbatim, so the layout keeps them until the definition returns.
        for (const auto& item : page.retained) {
            widgets_array.push_back(item);
        }
        page_obj["widgets"] = std::move(widgets_array);
        pages_json.push_back(std::move(page_obj));
    }

    json out;
    out["pages"] = std::move(pages_json);
    out["main_page_index"] = main_page_index_;
    out["next_page_id"] = next_page_id_;
    // Inside the payload so a parked grid carries the block its own
    // arrangement was stamped with.
    if (has_default_) {
        out["default"] = {{"signature", default_signature_}, {"hash", default_hash_}};
    }
    return out;
}

void PanelWidgetConfig::stamp_default(std::string signature) {
    has_default_ = true;
    provisional_signature_.clear();
    default_signature_ = std::move(signature);
    default_hash_ = helix::layout::arrangement_hash(serialize_pages());
}

void PanelWidgetConfig::read_default_block(const nlohmann::json& node) {
    has_default_ = false;
    default_signature_.clear();
    default_hash_.clear();
    provisional_signature_.clear();
    if (const json* block = helix::json_util::find_member(node, "default");
        block && block->is_object()) {
        has_default_ = true;
        default_signature_ = helix::json_util::safe_string(*block, "signature");
        default_hash_ = helix::json_util::safe_string(*block, "hash");
        return;
    }
    // The older spelling of a block still waiting for a measured grid. It
    // carries no hash and no signature, which is_unedited_default() reads as
    // the builder's own pending default.
    if (helix::json_util::safe_string(node, "anchors") == "pending") {
        has_default_ = true;
    }
}

bool PanelWidgetConfig::is_unedited_default() const {
    if (!has_default_) {
        return false;
    }
    if (default_hash_.empty()) {
        return default_signature_.empty();
    }
    return default_hash_ == helix::layout::arrangement_hash(serialize_pages());
}

void PanelWidgetConfig::restamp_default_hash() {
    if (has_default_) {
        default_hash_ = helix::layout::arrangement_hash(serialize_pages());
    }
}

std::string PanelWidgetConfig::default_signature() const {
    return has_default_ ? default_signature_ : std::string{};
}

void PanelWidgetConfig::mark_dirty() {
    flush_pending_save();
    loaded_ = false;
}

void PanelWidgetConfig::mark_printer_changed() {
    mark_dirty();
    saved_layout_known_ = false;
}

void PanelWidgetConfig::DeferredSave::cancel() {
    helix::ui::lv_timer_cancel_safe(timer);
    timer = nullptr;
    path.clear();
}

PanelWidgetConfig::DeferredSave::~DeferredSave() {
    cancel();
}

void PanelWidgetConfig::save_soon() {
    if (!deferred_save_.timer) {
        deferred_save_.path = config_.df() + "panel_widgets/" + panel_id_;
        deferred_save_.timer = lv_timer_create(
            [](lv_timer_t* t) {
                auto* self = static_cast<PanelWidgetConfig*>(lv_timer_get_user_data(t));
                // One-shot: the runner retires it.
                self->deferred_save_.timer = nullptr;
                const std::string path = std::move(self->deferred_save_.path);
                self->deferred_save_.path.clear();
                self->write_to(path);
            },
            kSaveSettleMs, this);
        lv_timer_set_repeat_count(deferred_save_.timer, 1);
        return;
    }
    lv_timer_reset(deferred_save_.timer);
}

void PanelWidgetConfig::flush_pending_save() {
    if (!deferred_save_.timer) {
        return;
    }
    const std::string path = deferred_save_.path;
    deferred_save_.cancel();
    write_to(path);
}

namespace {

std::function<void(const PanelWidgetConfig&)>& save_listener() {
    static HELIX_PSRAM_BSS std::function<void(const PanelWidgetConfig&)> listener;
    return listener;
}

} // namespace

void PanelWidgetConfig::set_save_listener(std::function<void(const PanelWidgetConfig&)> fn) {
    save_listener() = std::move(fn);
}

void PanelWidgetConfig::save() {
    deferred_save_.cancel();
    write_to(config_.df() + "panel_widgets/" + panel_id_);
}

void PanelWidgetConfig::write_to(const std::string& panel_path) {
    if (get_runtime_config()->crash_loop_safe_mode) {
        return;
    }
    // This node also carries layout state written by builds newer than this
    // one, which arrives whenever an update channel is rolled back. Those keys
    // are unreadable here but must survive the trip, so the node is edited
    // rather than replaced (prestonbrown/helixscreen#1460). A first save, or a
    // legacy array left by an older format, starts from an empty object.
    json root = config_.get<json>(panel_path, json());
    if (!root.is_object()) {
        root = json::object();
    }

    // Every key below is rewritten from member state, and the conditional ones
    // are cleared first: a key this build owns must not outlive the state that
    // asked for it just because an earlier save wrote it.
    for (const char* owned :
         {"anchors", "default", "layout_units", "legacy_rows", "grid", "parked_grids"}) {
        root.erase(owned);
    }
    // Named, not iterated straight off the call: items() is a proxy that does
    // not keep the document it walks alive.
    json owned_keys = serialize_pages();
    for (auto& [key, value] : owned_keys.items()) {
        root[key] = std::move(value);
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

    config_.set<json>(panel_path, root);
    if (!config_.save()) {
        return;
    }
    // A held save can land after the active printer changed; that layout is
    // not the one the connected printer shows.
    if (save_listener() && panel_path == config_.df() + "panel_widgets/" + panel_id_) {
        save_listener()(*this);
    }
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
    main_page_index_ = 0;
    next_page_id_ = 1;
    stamp_default("");
    had_saved_layout_ = true;
}

std::string PanelWidgetConfig::mint_instance_id(const std::string& base_id) {
    int max_n = 0;
    std::string prefix = base_id + ":";

    // Scan ALL pages for existing instance IDs
    for (const auto& page : pages_) {
        for (const auto& entry : page.widgets) {
            if (entry.id.size() > prefix.size() && entry.id.substr(0, prefix.size()) == prefix) {
                auto suffix = entry.id.substr(prefix.size());
                const auto n = helix::text_io::parse_leading<int>(suffix);
                if (n && *n > max_n)
                    max_n = *n;
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

void PanelWidgetConfig::remove_from_page(size_t page_index, size_t index) {
    auto& widgets = pages_[page_index].widgets;
    if (widgets[index].id.find(':') != std::string::npos) {
        widgets.erase(widgets.begin() + static_cast<std::ptrdiff_t>(index));
    } else {
        widgets[index].disable_and_unplace();
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
    // Every page, not the first match: a default-enabled widget placed on a
    // secondary page is also re-appended, unplaced, to the main page on load.
    for (const auto& page : pages_) {
        if (std::any_of(page.widgets.begin(), page.widgets.end(),
                        [&id](const PanelWidgetEntry& e) { return e.id == id && e.is_placed(); })) {
            return true;
        }
    }
    return false;
}

bool PanelWidgetConfig::page_is_populated(size_t page_index) const {
    const auto& entries = page_entries(page_index);
    return std::any_of(entries.begin(), entries.end(),
                       [](const PanelWidgetEntry& e) { return e.is_placed(); });
}

int PanelWidgetConfig::place_entry(std::string id, size_t page_index, int col, int row, int colspan,
                                   int rowspan) {
    if (page_index >= pages_.size()) {
        spdlog::warn("[PanelWidgetConfig] Cannot place '{}' on page {}: {} pages exist", id,
                     page_index, pages_.size());
        return -1;
    }

    // The first entry named id on the landing page is kept; every other one is
    // erased and offers its config. On the landing page the erased entries all
    // come after the kept one, so its index stays valid.
    int kept = -1;
    nlohmann::json carried = nlohmann::json::object();
    for (size_t p = 0; p < pages_.size(); ++p) {
        auto& widgets = pages_[p].widgets;
        size_t i = 0;
        while (i < widgets.size()) {
            if (widgets[i].id != id) {
                ++i;
                continue;
            }
            if (p == page_index && kept < 0) {
                kept = static_cast<int>(i);
                ++i;
                continue;
            }
            if (carried.empty() && !widgets[i].config.empty()) {
                carried = widgets[i].config;
            }
            spdlog::debug("[PanelWidgetConfig] Removing '{}' from page {}: placed on page {}", id,
                          p, page_index);
            widgets.erase(widgets.begin() + static_cast<ptrdiff_t>(i));
        }
    }

    auto& landing = pages_[page_index].widgets;
    if (kept < 0) {
        landing.push_back({std::move(id), true, std::move(carried), col, row, colspan, rowspan});
        return static_cast<int>(landing.size() - 1);
    }
    auto& entry = landing[static_cast<size_t>(kept)];
    entry.enabled = true;
    entry.col = col;
    entry.row = row;
    entry.colspan = colspan;
    entry.rowspan = rowspan;
    if (entry.config.empty() && !carried.empty()) {
        entry.config = std::move(carried);
    }
    return kept;
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

int PanelWidgetConfig::add_page(const std::string& name, int position) {
    if (!can_add_page()) {
        spdlog::warn("[PanelWidgetConfig] Cannot add page: at maximum ({} pages)", kMaxPages);
        return -1;
    }

    PageConfig page;
    page.id = name.empty() ? generate_page_id() : name;
    if (position < 0) {
        position = static_cast<int>(pages_.size());
    }
    position = std::min(position, static_cast<int>(pages_.size()));
    const size_t at = static_cast<size_t>(position);
    pages_.insert(pages_.begin() + static_cast<int64_t>(at), std::move(page));
    // A page landing at or before the main page shifts it one later, so the
    // main page keeps its place in the numbering.
    if (main_page_index_ >= at) {
        ++main_page_index_;
    }
    return position;
}

bool PanelWidgetConfig::remove_page(size_t page_index) {
    // A refusal is an expected answer: callers may offer any page that emptied.
    if (pages_.size() <= 1) {
        spdlog::debug("[PanelWidgetConfig] Not removing the last page");
        return false;
    }
    if (page_index >= pages_.size()) {
        return false;
    }
    if (page_index == main_page_index_) {
        spdlog::debug("[PanelWidgetConfig] Not removing the main page (index {})", page_index);
        return false;
    }

    // A removed page's undefined-widget entries move to the main page rather
    // than dying with it: the plugin they wait for is absent either way, and
    // the main page is the one that survives every removal.
    json retained = std::move(pages_[page_index].retained);
    pages_.erase(pages_.begin() + static_cast<ptrdiff_t>(page_index));

    // Adjust main_page_index
    if (main_page_index_ > page_index) {
        --main_page_index_;
    }

    for (const auto& item : retained) {
        pages_[main_page_index_].retained.push_back(item);
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

} // namespace

namespace {

/// A parsed runtime-editable JSON file, reused until the file's path, mtime or
/// size changes.
struct ParsedFileCache {
    std::string path;
    std::int64_t mtime_ns = -1;
    std::int64_t size = -1;
    std::shared_ptr<const nlohmann::json> parsed;
    int reads = 0;
};

ParsedFileCache& default_table_cache() {
    static ParsedFileCache cache;
    return cache;
}

ParsedFileCache& preset_overlay_cache() {
    static ParsedFileCache cache;
    return cache;
}

/// @p path parsed through @p cache: @p interpret turns the file's text into the
/// tree to keep. A file that cannot be read is null. Callers share one
/// immutable tree; a reload swaps the pointer, so a tree a caller still holds
/// stays valid.
template <typename Interpret>
std::shared_ptr<const nlohmann::json>
load_cached_json(ParsedFileCache& cache, const std::string& path, Interpret interpret) {
    const auto mtime = path.empty() ? std::nullopt : helix::fs::mtime_ns(path);
    const auto size = path.empty() ? std::nullopt : helix::text_io::file_size(path);
    const bool have_stat = mtime && size;
    const std::int64_t mtime_ns = have_stat ? *mtime : -1;
    const std::int64_t size_bytes = have_stat ? static_cast<std::int64_t>(*size) : -1;
    if (have_stat && cache.parsed && path == cache.path && mtime_ns == cache.mtime_ns &&
        size_bytes == cache.size) {
        return cache.parsed;
    }

    ++cache.reads;
    nlohmann::json tree;
    if (const auto text = helix::text_io::read_file(path)) {
        tree = interpret(*text);
    }
    auto parsed = std::make_shared<const nlohmann::json>(std::move(tree));
    if (have_stat) {
        cache.path = path;
        cache.mtime_ns = mtime_ns;
        cache.size = size_bytes;
        cache.parsed = parsed;
    }
    return parsed;
}

} // namespace

int PanelWidgetConfig::default_table_reads() {
    return default_table_cache().reads;
}

int PanelWidgetConfig::preset_overlay_reads() {
    return preset_overlay_cache().reads;
}

std::shared_ptr<const nlohmann::json> PanelWidgetConfig::load_default_table() {
    return load_cached_json(default_table_cache(), helix::find_readable("default_layout.json"),
                            [](const std::string& text) {
                                nlohmann::json layout = nlohmann::json::parse(text, nullptr, false);
                                if (layout.is_discarded()) {
                                    spdlog::warn("[PanelWidgetConfig] Failed to parse "
                                                 "default_layout.json: not valid JSON");
                                    return nlohmann::json();
                                }
                                return layout;
                            });
}

std::shared_ptr<const nlohmann::json>
PanelWidgetConfig::load_preset_overlay(const std::string& preset) {
    if (preset.empty()) {
        static const auto none = std::make_shared<const nlohmann::json>();
        return none;
    }
    const std::string seed_path = helix::find_readable("panel_widgets/" + preset + "/home.json");
    return load_cached_json(preset_overlay_cache(), seed_path, [&](const std::string& text) {
        nlohmann::json seed = nlohmann::json::parse(text, nullptr, false);
        if (!seed.is_object()) {
            spdlog::warn("[PanelWidgetConfig] Preset seed '{}' is not a JSON object", seed_path);
            return nlohmann::json();
        }
        return seed;
    });
}

helix::layout::CapabilitySnapshot
PanelWidgetConfig::snapshot_capabilities(const nlohmann::json& table,
                                         const nlohmann::json* overlay) {
    std::set<std::string> gates;
    for (const auto& def : get_all_widget_defs()) {
        if (def.hardware_gate_subject) {
            gates.insert(def.hardware_gate_subject);
        }
    }

    std::vector<std::string> names = helix::layout::subjects_named_by(table);
    if (overlay) {
        const auto more = helix::layout::subjects_named_by(*overlay);
        names.insert(names.end(), more.begin(), more.end());
    }

    helix::layout::CapabilitySnapshot snap;
    for (const auto& name : names) {
        if (lv_subject_t* subj = lv_xml_get_subject(nullptr, name.c_str())) {
            snap.subjects[name] = lv_subject_get_int(subj);
        } else {
            snap.subjects[name] = gates.count(name) ? 1 : 0;
        }
    }
    return snap;
}

namespace {

/// The name the default table keys the current breakpoint's tier by.
const char* current_tier_name() {
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    const UiBreakpoint breakpoint = bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj))
                                            : UiBreakpoint::Medium; // Default MEDIUM

    // One name per UiBreakpoint tier. Indexed by the enum, so it must stay
    // seven long: XXLarge is 6 and a six-entry table read past its end.
    static const char* bp_names[] = {"micro", "tiny",   "small",  "medium",
                                     "large", "xlarge", "xxlarge"};
    static_assert(std::size(bp_names) == to_int(UiBreakpoint::XXLarge) + 1,
                  "bp_names must cover every UiBreakpoint tier");
    return bp_names[to_int(breakpoint)];
}

} // namespace

helix::layout::ResolvedDefault PanelWidgetConfig::resolve_default(int grid_cols, int grid_rows,
                                                                  const nlohmann::json* overlay) {
    // A missing or malformed table is null, which the resolver reports as
    // unusable.
    const auto layout_ptr = load_default_table();
    const nlohmann::json& layout = *layout_ptr;

    // Anchor tables are keyed by layout variant in the same most-specific-first
    // order LayoutManager uses for ui_xml/ overrides: "variants": { "portrait":
    // [...] } wins over the top-level (landscape) "anchors" on a portrait panel,
    // and TINY_PORTRAIT falls through tiny_portrait → portrait → base (#1216).
    const Config* cfg = Config::get_instance();
    const std::string preset = cfg ? cfg->get_preset() : std::string{};
    const auto caps = snapshot_capabilities(layout, overlay);
    return helix::layout::resolve_default_layout(
        layout, overlay, current_tier_name(), grid_cols, grid_rows,
        LayoutManager::instance().variant_chain(), preset, caps);
}

std::vector<PanelWidgetEntry> PanelWidgetConfig::build_default_grid(int grid_cols, int grid_rows,
                                                                    std::string* signature,
                                                                    const nlohmann::json* overlay) {
    const auto& defs = get_all_widget_defs();
    const char* bp_name = current_tier_name();
    const bool portrait = is_portrait_layout(LayoutManager::instance().type());

    auto resolved = resolve_default(grid_cols, grid_rows, overlay);
    if (signature) {
        *signature = resolved.signature;
    }

    // Widgets the table switches off here: the tier's `disabled` list and every
    // slot candidate that was passed over. Leaving a widget out of the anchors
    // does NOT disable it, since the registry pass below appends it at its
    // default_enabled, so this has to be said explicitly.
    const std::set<std::string> disabled_ids(resolved.disabled.begin(), resolved.disabled.end());

    std::vector<helix::layout::ResolvedPlacement> anchors = std::move(resolved.placed);

    // Fallback: if the table placed nothing, use hardcoded defaults so the
    // dashboard always has printer_image, print_status, and tips placed sensibly.
    if (!resolved.ok) {
        spdlog::debug("[PanelWidgetConfig] Using hardcoded anchor fallback (bp={}, {})", bp_name,
                      portrait ? "portrait" : "landscape");
        if (portrait) {
            anchors = {
                {"printer_image", 0, 0, 8, 4, {}},
                {"print_status", 0, 4, 8, 4, {}},
            };
        } else {
            anchors = {
                {"printer_image", 0, 0, 4, 4, {}},
                {"print_status", 0, 4, 4, 4, {}},
                {"tips", 4, 0, 4, 4, {}},
            };
        }
    }

    // Drop anchors the measured grid cannot hold. Keeping one would hand it to
    // clamp_to_grid(), which shrinks the span and walks the origin back until it
    // fits — on a grid narrower than the table assumed that lands several
    // widgets on the same track, each looking like a chosen position. An
    // unseated widget goes through the auto-placement pass below instead, which
    // is the honest answer: this anchor does not describe this grid.
    //
    // The table chose these widgets for this printer, so one it cannot seat
    // stays on, waiting for auto-placement, rather than falling back to its
    // registry default_enabled.
    std::set<std::string> unseated;
    if (grid_cols > 0 && grid_rows > 0) {
        // Manual join — fmt::join needs fmt/ranges.h, which spdlog's bundled
        // copy does not carry.
        std::string rejected;
        int rejected_count = 0;
        anchors.erase(std::remove_if(anchors.begin(), anchors.end(),
                                     [&](const helix::layout::ResolvedPlacement& a) {
                                         // A widget this tier switches off is never placed, so
                                         // whether its anchor fits is not a fact about anything.
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
                                         unseated.insert(a.id);
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
        result.push_back({a.id, true, a.config.is_object() ? a.config : nlohmann::json::object(),
                          a.col, a.row, a.colspan, a.rowspan});
        fixed_ids.insert(a.id);
    }

    // All other widgets: enabled/disabled per registry, no grid position.
    // Positions computed dynamically at populate time.
    // Multi-instance widgets (fan_stack, thermistor, etc.) are included once as their
    // base ID — only additional instances (fan_stack:1, fan_stack:2) are user-added.
    for (const auto& def : defs) {
        if (fixed_ids.count(def.id) > 0)
            continue;
        const bool enabled = def.default_enabled || unseated.count(def.id) > 0;
        result.push_back({def.id, enabled, {}, -1, -1, def.colspan, def.rowspan});
    }

    // Applied after the registry pass so it reaches widgets that were appended
    // by default rather than anchored.
    for (auto& entry : result) {
        if (disabled_ids.count(entry.id) == 0) {
            continue;
        }
        entry.disable_and_unplace();
        spdlog::debug("[PanelWidgetConfig] '{}' disabled by the default layout at bp={}", entry.id,
                      bp_name);
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
        if (!fil->is_placed())
            continue;

        spdlog::info("[PanelWidgetConfig] Migrating stuck ams/filament on page '{}': "
                     "ams(-1,-1) + filament({},{}) → ams({},{}), filament disabled",
                     page.id, fil->col, fil->row, fil->col, fil->row);
        ams->col = fil->col;
        ams->row = fil->row;
        fil->disable_and_unplace();
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

std::vector<PanelWidgetEntry>
PanelWidgetConfig::build_default_grid_with_seed(int grid_cols, int grid_rows,
                                                std::string* signature) const {
    const auto overlay = load_preset_overlay(config_.get_preset());
    return build_default_grid(grid_cols, grid_rows, signature,
                              overlay->is_null() ? nullptr : overlay.get());
}

std::vector<PanelWidgetEntry> PanelWidgetConfig::build_defaults() const {
    return build_default_grid_with_seed(0, 0, nullptr);
}

void PanelWidgetConfig::restore_pages(const nlohmann::json& payload) {
    pages_.clear();
    read_default_block(payload);
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
                page.widgets =
                    parse_widget_array(page_json["widgets"], pages_.empty(), &page.retained);
            }
            pages_.push_back(std::move(page));
        }
    }
    if (pages_.empty()) {
        PageConfig page;
        page.id = "main";
        page.widgets = build_defaults();
        pages_.push_back(std::move(page));
        stamp_default("");
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
    // An unedited default open in Edit Mode is ported like any arrangement, so
    // the session keeps what it is showing, but it stays the builder's.
    const bool keep_default = edit_session_open_ && is_unedited_default();
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
    const int seated =
        (old_cols > 0 && old_rows > 0) ? port_pages(old_cols, old_rows, cols, rows) : 0;

    grid_signature_ = target;
    if (keep_default) {
        // Pending, it resolves for this grid at the first populate after the
        // session closes, and writes then.
        stamp_default("");
        spdlog::info("[PanelWidgetConfig] '{}': carried the default onto the {} grid for the "
                     "open edit session; it resolves when the session closes",
                     panel_id_, target);
        return;
    }

    // The ported pages are no longer the resolver's output for any grid.
    has_default_ = false;
    default_signature_.clear();
    default_hash_.clear();
    spdlog::info("[PanelWidgetConfig] '{}': seeded the {} grid from {} ({} widget(s) reseated, "
                 "the rest auto-placed)",
                 panel_id_, target, outgoing, seated);
    save();
}

int PanelWidgetConfig::port_pages(int old_cols, int old_rows, int cols, int rows) {
    int seated = 0;
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
    return seated;
}

bool PanelWidgetConfig::payload_holds_a_widget(const nlohmann::json& payload) {
    const json* pages = helix::json_util::find_member(payload, "pages");
    if (!pages || !pages->is_array()) {
        return false;
    }
    for (const auto& page_json : *pages) {
        const json* widgets = helix::json_util::find_member(page_json, "widgets");
        if (widgets && widgets->is_array() &&
            !parse_widget_array(*widgets, /*append_registry_defaults=*/false).empty()) {
            return true;
        }
    }
    return false;
}

bool PanelWidgetConfig::import_layout(const helix::home_layout::ParsedLayout& layout, int cols,
                                      int rows) {
    if (cols <= 0 || rows <= 0 || layout.cols <= 0 || layout.rows <= 0) {
        spdlog::warn("[PanelWidgetConfig] '{}': import refused, grid {}x{} -> {}x{}", panel_id_,
                     layout.cols, layout.rows, cols, rows);
        return false;
    }
    // Validated in full on a copy: restore_pages() rebuilds defaults from a
    // payload with no usable page, so it must only ever see one that has one.
    json payload = layout.pages_payload;
    if (!helix::home_layout::strip_screen_scoped(payload) || !payload_holds_a_widget(payload)) {
        spdlog::warn("[PanelWidgetConfig] '{}': import refused, no page holds a printer widget",
                     panel_id_);
        return false;
    }

    restore_pages(payload);
    // The import is the user's arrangement, whatever bookkeeping it carried.
    read_default_block(json());
    legacy_units_ = false;
    legacy_rows_ = 0;
    loaded_ = true;

    // Every parked grid is a port of the layout being replaced, and would bring
    // it back on the next grid change.
    parked_grids_ = json::object();
    // Run on an identical grid too: there the port copies each placement as it
    // is and clamps one that lies off the grid.
    const int seated = port_pages(layout.cols, layout.rows, cols, rows);
    grid_signature_ = std::to_string(cols) + "x" + std::to_string(rows);

    // Screen-scoped widgets describe this screen, so it keeps its own. A copy
    // the registry appended disabled is replaced by the enabled one.
    auto& main_widgets = pages_[main_page_index_].widgets;
    int added = 0;
    for (auto& entry : build_defaults()) {
        if (!entry.enabled || !is_screen_scoped(entry.id) || is_enabled(entry.id)) {
            continue;
        }
        for (auto& page : pages_) {
            auto& w = page.widgets;
            w.erase(std::remove_if(w.begin(), w.end(),
                                   [&](const PanelWidgetEntry& e) { return e.id == entry.id; }),
                    w.end());
        }
        entry.col = -1;
        entry.row = -1;
        main_widgets.push_back(std::move(entry));
        ++added;
    }

    spdlog::info("[PanelWidgetConfig] '{}': imported a {}x{} layout onto the {} grid ({} page(s), "
                 "{} widget(s) seated, {} screen widget(s) added)",
                 panel_id_, layout.cols, layout.rows, grid_signature_, pages_.size(), seated,
                 added);
    save();
    return true;
}

void PanelWidgetConfig::apply_pending_anchors(int grid_cols, int grid_rows) {
    if (!has_pending_anchors() || grid_cols <= 0 || grid_rows <= 0) {
        return;
    }

    // Rebuild rather than patch. The anchor table decides enable/disable per
    // tier and carries per-widget config, and a grid-qualified entry may differ
    // from the tier entry in any of those, not only in coordinates — so the
    // answer for this grid is whatever build_default_grid() says for it.
    //
    // Page 0 only. Extra pages are user-made, and a user who has built one has
    // arranged page 0 too, so a layout still tagged pending has exactly one.
    std::string signature;
    auto rebuilt = build_default_grid_with_seed(grid_cols, grid_rows, &signature);
    if (rebuilt.empty() || pages_.empty()) {
        return;
    }
    install_default(std::move(rebuilt), std::move(signature), grid_cols, grid_rows);

    int anchored = 0;
    for (const auto& e : pages_[0].widgets) {
        if (e.has_grid_position()) {
            ++anchored;
        }
    }
    spdlog::info("[PanelWidgetConfig] '{}': applied default anchors for a {}x{} track grid, {} of "
                 "{} widget(s) placed",
                 panel_id_, grid_cols, grid_rows, anchored, pages_[0].widgets.size());
    save();
}

void PanelWidgetConfig::install_default(std::vector<PanelWidgetEntry> rebuilt,
                                        std::string signature, int cols, int rows) {
    // Per-widget config (an LED strip, a print_status style) is outside the
    // arrangement, so it belongs to the widget and survives a re-resolve. The
    // table's config for the new placement is the base: a key the widget's
    // config does not set keeps the table's value.
    for (auto& entry : rebuilt) {
        for (const auto& old : pages_[0].widgets) {
            if (old.id != entry.id || old.config.empty()) {
                continue;
            }
            if (entry.config.is_object() && old.config.is_object()) {
                entry.config.update(old.config);
            } else {
                entry.config = old.config;
            }
            break;
        }
    }
    pages_[0].widgets = std::move(rebuilt);
    grid_signature_ = std::to_string(cols) + "x" + std::to_string(rows);
    stamp_default(std::move(signature));
}

bool PanelWidgetConfig::parked_layout_is_users(const std::string& signature) const {
    auto it = parked_grids_.find(signature);
    if (it == parked_grids_.end() || !it->is_object()) {
        return false;
    }
    const json* block = helix::json_util::find_member(*it, "default");
    if (!block || !block->is_object()) {
        return true;
    }
    const std::string hash = helix::json_util::safe_string(*block, "hash");
    if (hash.empty()) {
        return !helix::json_util::safe_string(*block, "signature").empty();
    }
    return hash != helix::layout::arrangement_hash(*it);
}

bool PanelWidgetConfig::reresolve_default_if_changed(int cols, int rows, bool persist) {
    if (edit_session_open_ || cols <= 0 || rows <= 0 || pages_.empty() || !is_unedited_default()) {
        return false;
    }
    const std::string target = std::to_string(cols) + "x" + std::to_string(rows);
    const bool grid_changes = grid_signature_ != target;
    if (grid_changes && parked_layout_is_users(target)) {
        return false;
    }
    // Capabilities read while the printer is not READY do not rearrange a
    // default that has already resolved; only a pending one, or one whose grid
    // changed, needs an arrangement to show meanwhile.
    if (!persist && !grid_changes && !has_pending_anchors()) {
        return false;
    }

    // The signature decides; the entries are built only when it moved. A
    // pending block has no signature yet, so it always differs when persisting.
    const auto overlay = load_preset_overlay(config_.get_preset());
    const nlohmann::json* seed = overlay->is_null() ? nullptr : overlay.get();
    const std::string signature = resolve_default(cols, rows, seed).signature;
    const std::string& shown = persist ? default_signature_ : provisional_signature_;
    if (!grid_changes && signature == shown) {
        return false;
    }
    auto rebuilt = build_default_grid(cols, rows, nullptr, seed);
    if (rebuilt.empty()) {
        return false;
    }

    if (!persist) {
        // Shown, but left pending and unsaved: the first READY populate
        // resolves it from the printer's own values and writes it then.
        install_default(std::move(rebuilt), "", cols, rows);
        provisional_signature_ = signature;
        spdlog::debug("[PanelWidgetConfig] '{}': resolved the default for the {} grid for display "
                      "while the printer is not READY ({}); it stays pending",
                      panel_id_, target, signature);
        return true;
    }

    const std::string from = default_signature_;
    install_default(std::move(rebuilt), signature, cols, rows);
    // This grid's arrangement is the one just resolved; a parked copy of an
    // older default for it would only shadow it on the next switch.
    parked_grids_.erase(target);
    spdlog::info("[PanelWidgetConfig] '{}': re-resolved the default layout for the {} grid ({} "
                 "-> {})",
                 panel_id_, target, from.empty() ? "pending" : from, signature);
    save_soon();
    return true;
}

std::pair<int, int> PanelWidgetConfig::stamped_grid() const {
    return {parse_grid_axis(grid_signature_, /*want_cols=*/true),
            parse_grid_axis(grid_signature_, /*want_cols=*/false)};
}

} // namespace helix
