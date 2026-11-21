// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lane_spool_history.h"

#include "color_utils.h"
#include "i_moonraker_api.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include "hv/json.hpp"

namespace helix::lane_spool_history {

namespace {

bool same_filament(const Entry& a, const Entry& b) {
    return helix::text_io::equals_ignore_case(a.material, b.material) &&
           helix::text_io::equals_ignore_case(a.brand, b.brand) &&
           helix::text_io::equals_ignore_case(a.product_name, b.product_name) &&
           a.color_rgb == b.color_rgb;
}

} // namespace

std::optional<Entry> entry_from_slot(const SlotInfo& slot) {
    if (slot.material.empty()) {
        return std::nullopt;
    }
    Entry e;
    e.material = slot.material;
    e.brand =
        helix::text_io::equals_ignore_case(slot.brand, "Generic") ? std::string() : slot.brand;
    e.product_name = slot.product_name;
    e.catalog_id = slot.catalog_id;
    e.color_rgb = slot.color_rgb;
    e.total_weight_g = slot.total_weight_g > 0 ? slot.total_weight_g : 0;
    return e;
}

std::vector<Entry> with_recent(std::vector<Entry> list, const Entry& entry, size_t max) {
    std::vector<Entry> out;
    out.reserve(std::min(max, list.size() + 1));
    out.push_back(entry);
    for (auto& e : list) {
        if (out.size() >= max) {
            break;
        }
        if (!same_filament(e, entry)) {
            out.push_back(std::move(e));
        }
    }
    return out;
}

SpoolInfo as_spool(const Entry& entry) {
    SpoolInfo s;
    s.material = entry.material;
    s.vendor = entry.brand;
    s.filament_name = entry.product_name;
    s.color_hex = helix::color_to_hex_string(entry.color_rgb);
    s.initial_weight_g = entry.total_weight_g;
    s.remaining_weight_g = entry.total_weight_g;
    return s;
}

std::vector<Entry> from_json(const nlohmann::json& value) {
    std::vector<Entry> out;
    if (!value.is_array()) {
        return out;
    }
    for (const auto& item : value) {
        if (!item.is_object()) {
            continue;
        }
        auto text = [&item](const char* key) {
            const auto it = item.find(key);
            return (it != item.end() && it->is_string()) ? it->get<std::string>() : std::string();
        };
        Entry e;
        e.material = text("material");
        if (e.material.empty()) {
            continue;
        }
        e.brand = text("brand");
        e.product_name = text("product");
        e.catalog_id = text("catalog_id");
        const auto rgb = item.find("color");
        if (rgb != item.end() && rgb->is_number_unsigned()) {
            e.color_rgb = rgb->get<uint32_t>() & 0xFFFFFF;
        }
        const auto total = item.find("total_g");
        if (total != item.end() && total->is_number()) {
            e.total_weight_g = total->get<float>();
        }
        out.push_back(std::move(e));
        if (out.size() >= kMaxEntries) {
            break;
        }
    }
    return out;
}

nlohmann::json to_json(const std::vector<Entry>& entries) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& e : entries) {
        list.push_back({{"material", e.material},
                        {"brand", e.brand},
                        {"product", e.product_name},
                        {"catalog_id", e.catalog_id},
                        {"color", e.color_rgb},
                        {"total_g", e.total_weight_g}});
    }
    return list;
}

void load(IMoonrakerAPI& api, std::function<void(std::vector<Entry>)> on_loaded) {
    auto done = std::make_shared<std::function<void(std::vector<Entry>)>>(std::move(on_loaded));
    api.database_get_item(
        kDbNamespace, kDbKey, [done](const nlohmann::json& value) { (*done)(from_json(value)); },
        // A key nothing has written yet answers not-found: an empty history.
        [done](const MoonrakerError&) { (*done)({}); });
}

void record(IMoonrakerAPI& api, const SlotInfo& slot) {
    const std::optional<Entry> entry = entry_from_slot(slot);
    if (!entry) {
        return;
    }
    IMoonrakerAPI* target = &api;
    load(api, [target, entry = *entry](std::vector<Entry> list) {
        target->database_post_item(
            kDbNamespace, kDbKey, to_json(with_recent(std::move(list), entry)), nullptr,
            [](const MoonrakerError& err) {
                spdlog::warn("[LaneSpoolHistory] Not recorded: {}", err.message);
            });
    });
}

} // namespace helix::lane_spool_history
