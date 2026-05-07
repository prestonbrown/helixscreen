// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_config.h"

#include "config.h"
#include "data_root_resolver.h"
#include "grid_layout.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <hv/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <set>

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

        // Load grid placement coordinates (default to -1 = auto-place)
        int col = -1;
        int row_val = -1;
        int colspan = 1;
        int rowspan = 1;
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
        // New multi-page format
        main_page_index_ = saved.value("main_page_index", 0);
        next_page_id_ = saved.value("next_page_id", 0);

        size_t page_idx = 0;
        for (const auto& page_json : saved["pages"]) {
            PageConfig page;
            page.id = page_json.value("id", "");
            if (page_json.contains("widgets") && page_json["widgets"].is_array()) {
                // Only append registry defaults for the first page (main/default page)
                bool append_defaults = (page_idx == 0);
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

    main_page_index_ = seed.value("main_page_index", 0);
    next_page_id_ = seed.value("next_page_id", 0);

    size_t page_idx = 0;
    for (const auto& page_json : seed["pages"]) {
        PageConfig page;
        page.id = page_json.value("id", "");
        if (page_json.contains("widgets") && page_json["widgets"].is_array()) {
            bool append_defaults = (page_idx == 0);
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

void PanelWidgetConfig::save() {
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

    json root;
    root["pages"] = std::move(pages_json);
    root["main_page_index"] = main_page_index_;
    root["next_page_id"] = next_page_id_;

    config_.set<json>(config_.df() + "panel_widgets/" + panel_id_, root);
    config_.save();
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
    if (pages_.size() >= kMaxPages) {
        spdlog::warn("[PanelWidgetConfig] Cannot add page: at maximum ({} pages)", kMaxPages);
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

// Breakpoint name to index mapping for default_layout.json
static int breakpoint_name_to_index(const std::string& name) {
    if (name == "tiny")
        return 0;
    if (name == "small")
        return 1;
    if (name == "medium")
        return 2;
    if (name == "large")
        return 3;
    if (name == "xlarge")
        return 4;
    return -1;
}

std::vector<PanelWidgetEntry> PanelWidgetConfig::build_default_grid() {
    const auto& defs = get_all_widget_defs();

    // Determine current breakpoint for per-breakpoint anchor sizing
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint breakpoint = bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj))
                                      : UiBreakpoint::Medium; // Default MEDIUM

    static const char* bp_names[] = {"micro", "tiny", "small", "medium", "large", "xlarge"};
    const char* bp_name = bp_names[to_int(breakpoint)];

    // Load anchor placements from config/default_layout.json (runtime-editable).
    // Falls back to registry defaults if file is missing or malformed.
    struct AnchorPlacement {
        std::string id;
        int col, row, colspan, rowspan;
    };
    std::vector<AnchorPlacement> anchors;

    std::ifstream layout_file(helix::find_readable("default_layout.json"));
    if (layout_file.is_open()) {
        try {
            nlohmann::json layout = nlohmann::json::parse(layout_file);
            for (const auto& anchor : layout.value("anchors", nlohmann::json::array())) {
                std::string id = anchor.value("id", "");
                if (id.empty() || !find_widget_def(id))
                    continue;

                auto placements = anchor.value("placements", nlohmann::json::object());

                // Fallback chain: micro→tiny→small, xlarge→large (matches theme_manager)
                static const char* fallback_order[][3] = {
                    {"micro", "tiny", "small"},   // bp=0
                    {"tiny", "small", nullptr},   // bp=1
                    {"small", nullptr},           // bp=2
                    {"medium", nullptr},          // bp=3
                    {"large", nullptr},           // bp=4
                    {"xlarge", "large", nullptr}, // bp=5
                };
                const char* chosen_name = nullptr;
                int bp_idx = to_int(breakpoint);
                if (bp_idx >= 0 && bp_idx <= 5) {
                    for (int i = 0; i < 3 && fallback_order[bp_idx][i]; ++i) {
                        if (placements.contains(fallback_order[bp_idx][i])) {
                            chosen_name = fallback_order[bp_idx][i];
                            break;
                        }
                    }
                }
                if (chosen_name) {
                    auto& p = placements[chosen_name];
                    anchors.push_back({id, p.value("col", 0), p.value("row", 0),
                                       p.value("colspan", 1), p.value("rowspan", 1)});
                }
            }
            spdlog::debug("[PanelWidgetConfig] Loaded {} anchors from default_layout.json (bp={})",
                          anchors.size(), bp_name);
        } catch (const std::exception& e) {
            spdlog::warn("[PanelWidgetConfig] Failed to parse default_layout.json: {}", e.what());
            anchors.clear();
        }
    }

    // Fallback: if no anchors loaded, use hardcoded defaults so the dashboard
    // always has printer_image, print_status, and tips placed sensibly.
    if (anchors.empty()) {
        spdlog::debug("[PanelWidgetConfig] Using hardcoded anchor fallback (bp={})", bp_name);
        anchors = {
            {"printer_image", 0, 0, 2, 2},
            {"print_status", 0, 2, 2, 2},
            {"tips", 2, 0, 4, 2},
        };
    }

    // Build result: anchored widgets first, then all others with auto-placement
    std::vector<PanelWidgetEntry> result;
    result.reserve(defs.size());
    std::set<std::string> fixed_ids;

    for (const auto& a : anchors) {
        if (!find_widget_def(a.id))
            continue;
        result.push_back({a.id, true, {}, a.col, a.row, a.colspan, a.rowspan});
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

    // Bed temperature: always last, enabled conditionally.
    // Large/xlarge: always enabled. Small/medium: only when no AMS present (no room).
    {
        auto it = std::find_if(result.begin(), result.end(),
                               [](const PanelWidgetEntry& e) { return e.id == "bed_temperature"; });
        if (it != result.end()) {
            bool is_large = (to_int(breakpoint) >= to_int(UiBreakpoint::Large));
            it->enabled = is_large || !ams_present;
            // Move to end so it's the last widget placed
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

} // namespace helix
