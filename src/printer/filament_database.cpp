// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_database.h"

#include "core/text_io.h"
#include "filament_catalog.h"
#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <mutex>
#include <unordered_set>

#include "hv/json.hpp"

namespace filament {

namespace {

/// Every string a MaterialInfo points at. Append-only, so a pointer handed out
/// under one snapshot stays valid after a reload replaces it. It grows only by
/// distinct names the user writes into the overlay.
std::mutex g_intern_mutex;
std::unordered_set<std::string> g_interned;

const char* intern(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_intern_mutex);
    return g_interned.insert(s).first->c_str();
}

struct Tables {
    std::shared_ptr<const std::vector<MaterialInfo>> merged;
    std::shared_ptr<const std::vector<MaterialInfo>> shipped;
};

std::mutex g_table_mutex;
Tables g_tables;

int get_int(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<int>() : 0;
}

/// One `types` object, shipped or user, as a MaterialInfo. Missing fields read
/// as 0 / "", the same as a type the table does not know.
MaterialInfo to_material_info(const nlohmann::json& t, bool user_defined) {
    const std::string name = helix::json_util::safe_string(t, "name");
    std::string group = helix::json_util::safe_string(t, "compat_group");
    std::string category = helix::json_util::safe_string(t, "category");
    if (user_defined) {
        // A type with no group of its own would be endless-spool compatible
        // with nothing or, worse, with a material it happens to share "" with.
        if (group.empty())
            group = name;
        if (category.empty())
            category = "Custom";
    }
    auto density = t.find("density");
    MaterialInfo m{
        .name = intern(name),
        .nozzle_min = get_int(t, "nozzle_min"),
        .nozzle_max = get_int(t, "nozzle_max"),
        .bed_temp = get_int(t, "bed"),
        .category = intern(category),
        .dry_temp_c = get_int(t, "dry_temp"),
        .dry_time_min = get_int(t, "dry_time"),
        .density_g_cm3 =
            (density != t.end() && density->is_number()) ? density->get<float>() : 0.0f,
        .chamber_temp_c = get_int(t, "chamber"),
        .compat_group = intern(group),
    };
    m.user_defined = user_defined;
    return m;
}

/// Drop every known numeric field of a hand-written patch whose value is not a
/// number, so the shipped value stands. A quoted "205" would otherwise read as
/// 0, and a null is merge_patch's "delete": either one preheats to a midpoint
/// of the real bound and 0.
void drop_non_numeric_fields(nlohmann::json& patch, const std::string& name,
                             const std::string& path) {
    static constexpr const char* kNumericKeys[] = {"nozzle_min", "nozzle_max", "bed",    "chamber",
                                                   "dry_temp",   "dry_time",   "density"};
    for (const char* key : kNumericKeys) {
        auto it = patch.find(key);
        if (it != patch.end() && !it->is_number()) {
            spdlog::warn("[filament] ignoring non-numeric '{}' of type '{}' in {}", key, name,
                         path);
            patch.erase(it);
        }
    }
}

/// A type the user defines needs a nozzle range to preheat to: there is no
/// shipped row to fill the gaps.
bool has_usable_nozzle_range(const nlohmann::json& t) {
    const int lo = get_int(t, "nozzle_min");
    const int hi = get_int(t, "nozzle_max");
    return lo > 0 && hi >= lo;
}

/// The asset's `types` array, without materializing the ~360 products beside
/// it: a freed DOM does not hand its pages back, so parsing them here would
/// raise the arena high-water mark for good on the smallest boards.
nlohmann::json read_asset_types(const std::string& path) {
    auto text = helix::text_io::read_file(path);
    if (!text) {
        spdlog::error("[filament] material types asset not found: '{}'", path);
        return nlohmann::json::array();
    }
    auto doc = nlohmann::json::parse(
        *text,
        [](int depth, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
            return !(depth == 1 && event == nlohmann::json::parse_event_t::key &&
                     parsed != "types");
        },
        false);
    if (doc.is_discarded()) {
        spdlog::error("[filament] material types parse failed: {}", path);
        return nlohmann::json::array();
    }
    if (doc.is_object() && doc.contains("types") && doc["types"].is_array())
        return std::move(doc["types"]);
    spdlog::error("[filament] {} has no `types` array", path);
    return nlohmann::json::array();
}

Tables build_tables(const std::string& asset_path, const std::string& overlay_path) {
    nlohmann::json asset_types = read_asset_types(asset_path);
    std::vector<nlohmann::json> rows;
    for (auto& t : asset_types) {
        if (t.is_object() && !helix::json_util::safe_string(t, "name").empty())
            rows.push_back(std::move(t));
    }
    const size_t shipped_count = rows.size();
    std::vector<MaterialInfo> shipped;
    shipped.reserve(shipped_count);
    for (const auto& r : rows)
        shipped.push_back(to_material_info(r, false));

    for (auto& patch : helix::printer::FilamentCatalog::load_user_types_from(overlay_path)) {
        const std::string name = helix::json_util::safe_string(patch, "name");
        if (name.empty()) {
            spdlog::warn("[filament] skipping a user type with no name in {}", overlay_path);
            continue;
        }
        drop_non_numeric_fields(patch, name, overlay_path);
        const std::string key = helix::text_io::to_lower(std::string(resolve_alias(name)));
        auto hit = std::find_if(rows.begin(), rows.end(), [&](const nlohmann::json& r) {
            return helix::text_io::to_lower(helix::json_util::safe_string(r, "name")) == key;
        });
        if (hit != rows.end()) {
            // The shipped spelling stays canonical; the patch may name it in any case.
            const std::string canonical = helix::json_util::safe_string(*hit, "name");
            hit->merge_patch(patch);
            (*hit)["name"] = canonical;
        } else if (has_usable_nozzle_range(patch)) {
            rows.push_back(patch);
        } else {
            spdlog::warn("[filament] skipping user type '{}' in {}: it needs nozzle_min > 0 and "
                         "nozzle_max >= nozzle_min",
                         name, overlay_path);
        }
    }

    std::vector<MaterialInfo> table;
    table.reserve(rows.size());
    for (size_t i = 0; i < rows.size(); ++i)
        table.push_back(to_material_info(rows[i], i >= shipped_count));
    spdlog::debug("[filament] {} material types ({} shipped) from '{}' + '{}'", table.size(),
                  shipped_count, asset_path, overlay_path);
    return {std::make_shared<const std::vector<MaterialInfo>>(std::move(table)),
            std::make_shared<const std::vector<MaterialInfo>>(std::move(shipped))};
}

Tables current_tables() {
    {
        std::lock_guard<std::mutex> lock(g_table_mutex);
        if (g_tables.merged)
            return g_tables;
    }
    // Two first callers racing both build and the later one wins: wasteful
    // once, never wrong.
    reload_materials();
    std::lock_guard<std::mutex> lock(g_table_mutex);
    return g_tables;
}

} // namespace

// NAMESPACE_OK: extends filament::, the material table's existing namespace
void load_materials_from(const std::string& asset_path, const std::string& overlay_path) {
    Tables built = build_tables(asset_path, overlay_path);
    std::lock_guard<std::mutex> lock(g_table_mutex);
    g_tables = std::move(built);
}

// NAMESPACE_OK: extends filament::, the material table's existing namespace
void reload_materials() {
    using helix::printer::FilamentCatalog;
    load_materials_from(FilamentCatalog::builtin_asset_path(),
                        FilamentCatalog::user_overlay_path());
}

// NAMESPACE_OK: extends filament::, the material table's existing namespace
std::shared_ptr<const std::vector<MaterialInfo>> materials() {
    return current_tables().merged;
}

// NAMESPACE_OK: extends filament::, the material table's existing namespace
std::shared_ptr<const std::vector<MaterialInfo>> shipped_materials() {
    return current_tables().shipped;
}

} // namespace filament
