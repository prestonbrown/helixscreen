// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_config.h"

#include "config.h"
#include "grid_layout.h"
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

void PanelWidgetConfig::load() {
    entries_.clear();

    // Per-panel path: /printers/{active}/panel_widgets/<panel_id>
    std::string panel_path = config_.df() + "panel_widgets/" + panel_id_;
    auto saved = config_.get<json>(panel_path, json());

    // Migration: move legacy "home_widgets" to "panel_widgets.home"
    if (panel_id_ == "home" && (saved.is_null() || !saved.is_array())) {
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

    if (!saved.is_array()) {
        entries_ = build_defaults();
        save(); // Persist default grid positions for future launches
        return;
    }

    std::set<std::string> seen_ids;

    for (const auto& item : saved) {
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

        // Load optional per-widget config
        nlohmann::json widget_config;
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
        entries_.push_back({id, enabled, widget_config, col, row_val, colspan, rowspan});
    }

    // Append any new widgets from registry that are not in saved config
    for (const auto& def : get_all_widget_defs()) {
        if (seen_ids.count(def.id) == 0) {
            spdlog::debug("[PanelWidgetConfig] Appending new widget: {} (default_enabled={})",
                          def.id, def.default_enabled);
            entries_.push_back({def.id, def.default_enabled, {}, -1, -1, def.colspan, def.rowspan});
        }
    }

    if (entries_.empty()) {
        entries_ = build_defaults();
        return;
    }

    // If no entries have grid positions, this is a pre-grid config — reset to defaults.
    bool has_any_grid =
        std::any_of(entries_.begin(), entries_.end(),
                    [](const PanelWidgetEntry& e) { return e.has_grid_position(); });
    if (!has_any_grid) {
        spdlog::info(
            "[PanelWidgetConfig] Pre-grid config detected, resetting to default grid for '{}'",
            panel_id_);
        entries_ = build_defaults();
        save();
    }
}

void PanelWidgetConfig::save() {
    json widgets_array = json::array();
    for (const auto& entry : entries_) {
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
    config_.set<json>(config_.df() + "panel_widgets/" + panel_id_, widgets_array);
    config_.save();
}

void PanelWidgetConfig::reorder(size_t from_index, size_t to_index) {
    if (from_index >= entries_.size() || to_index >= entries_.size()) {
        return;
    }
    if (from_index == to_index) {
        return;
    }

    // Extract element, then insert at new position
    auto entry = std::move(entries_[from_index]);
    entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(from_index));
    entries_.insert(entries_.begin() + static_cast<ptrdiff_t>(to_index), std::move(entry));
}

void PanelWidgetConfig::set_enabled(size_t index, bool enabled) {
    if (index >= entries_.size()) {
        return;
    }
    entries_[index].enabled = enabled;
}

void PanelWidgetConfig::reset_to_defaults() {
    entries_ = build_defaults();
}

bool PanelWidgetConfig::is_enabled(const std::string& id) const {
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&id](const PanelWidgetEntry& e) { return e.id == id; });
    return it != entries_.end() && it->enabled;
}

nlohmann::json PanelWidgetConfig::get_widget_config(const std::string& id) const {
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&id](const PanelWidgetEntry& e) { return e.id == id; });
    if (it != entries_.end() && !it->config.empty()) {
        return it->config;
    }
    return nlohmann::json::object();
}

void PanelWidgetConfig::set_widget_config(const std::string& id, const nlohmann::json& config) {
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&id](const PanelWidgetEntry& e) { return e.id == id; });
    if (it != entries_.end()) {
        it->config = config;
        save();
    } else {
        spdlog::debug("[PanelWidgetConfig] set_widget_config: widget '{}' not found", id);
    }
}

// Breakpoint name to index mapping for default_layout.json
static int breakpoint_name_to_index(const std::string& name) {
    if (name == "tiny") return 0;
    if (name == "small") return 1;
    if (name == "medium") return 2;
    if (name == "large") return 3;
    if (name == "xlarge") return 4;
    return -1;
}

std::vector<PanelWidgetEntry> PanelWidgetConfig::build_default_grid() {
    const auto& defs = get_all_widget_defs();

    // Determine current breakpoint for per-breakpoint anchor sizing
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    int breakpoint = bp_subj ? lv_subject_get_int(bp_subj) : 2; // Default MEDIUM

    static const char* bp_names[] = {"tiny", "small", "medium", "large", "xlarge"};
    const char* bp_name = (breakpoint >= 0 && breakpoint <= 4) ? bp_names[breakpoint] : "medium";

    // Load anchor placements from config/default_layout.json (runtime-editable).
    // Falls back to registry defaults if file is missing or malformed.
    struct AnchorPlacement {
        std::string id;
        int col, row, colspan, rowspan;
    };
    std::vector<AnchorPlacement> anchors;

    std::ifstream layout_file("config/default_layout.json");
    if (layout_file.is_open()) {
        try {
            nlohmann::json layout = nlohmann::json::parse(layout_file);
            for (const auto& anchor : layout.value("anchors", nlohmann::json::array())) {
                std::string id = anchor.value("id", "");
                if (id.empty() || !find_widget_def(id))
                    continue;

                auto placements = anchor.value("placements", nlohmann::json::object());
                if (placements.contains(bp_name)) {
                    auto& p = placements[bp_name];
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
    for (const auto& def : defs) {
        if (fixed_ids.count(def.id) > 0)
            continue;
        result.push_back({def.id, def.default_enabled, {}, -1, -1, def.colspan, def.rowspan});
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

bool PanelWidgetConfig::is_grid_format() const {
    return std::any_of(entries_.begin(), entries_.end(),
                       [](const PanelWidgetEntry& e) { return e.has_grid_position(); });
}

std::vector<PanelWidgetEntry> PanelWidgetConfig::build_defaults() {
    return build_default_grid();
}

} // namespace helix
