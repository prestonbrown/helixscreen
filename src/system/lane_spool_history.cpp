// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lane_spool_history.h"

#include "async_lifetime_guard.h"
#include "color_utils.h"
#include "i_moonraker_api.h"
#include "moonraker_error.h"
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

/// @p item's string member @p key, or empty when it is absent or not a string.
std::string string_member(const nlohmann::json& item, const char* key) {
    const auto it = item.find(key);
    return (it != item.end() && it->is_string()) ? it->get<std::string>() : std::string();
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
        Entry e;
        e.material = string_member(item, "material");
        if (e.material.empty()) {
            continue;
        }
        e.brand = string_member(item, "brand");
        e.product_name = string_member(item, "product");
        e.catalog_id = string_member(item, "catalog_id");
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
        // Member by member: an initializer list builds a temporary json per
        // field and costs several times the code.
        nlohmann::json item = nlohmann::json::object();
        item["material"] = e.material;
        item["brand"] = e.brand;
        item["product"] = e.product_name;
        item["catalog_id"] = e.catalog_id;
        item["color"] = e.color_rgb;
        item["total_g"] = e.total_weight_g;
        list.push_back(std::move(item));
    }
    return list;
}

void load(IMoonrakerAPI& api, helix::SharedCallback<void(std::vector<Entry>)> on_loaded) {
    api.database_get_item(
        kDbNamespace, kDbKey,
        [on_loaded](const nlohmann::json& value) { on_loaded(from_json(value)); },
        // A key nothing has written yet answers not-found: an empty history.
        [on_loaded](const MoonrakerError&) { on_loaded({}); });
}

std::optional<std::vector<Entry>> base_for_record(const nlohmann::json* stored,
                                                  const MoonrakerError* error) {
    if (stored) {
        return from_json(*stored);
    }
    if (error && error->is_not_found()) {
        return std::vector<Entry>{};
    }
    return std::nullopt;
}

namespace {

/// Records run one read-modify-write at a time, on the main thread; a record
/// arriving meanwhile waits and rides the next write, so two lanes saved back
/// to back both land.
struct Recorder {
    bool busy = false;
    std::optional<LifetimeToken> api_alive; ///< the API the running write uses
    std::vector<Entry> waiting;
};

Recorder& recorder() {
    static Recorder r;
    return r;
}

void run_record(IMoonrakerAPI& api);

void finish_write(IMoonrakerAPI& api) {
    Recorder& r = recorder();
    if (r.waiting.empty()) {
        r.busy = false;
        r.api_alive.reset();
        return;
    }
    run_record(api);
}

void write_history(IMoonrakerAPI& api, std::vector<Entry> list) {
    Recorder& r = recorder();
    for (const auto& entry : r.waiting) {
        list = with_recent(std::move(list), entry);
    }
    r.waiting.clear();
    const LifetimeToken alive = api.lifetime_token();
    IMoonrakerAPI* target = &api;
    api.database_post_item(
        kDbNamespace, kDbKey, to_json(list),
        [alive, target]() {
            alive.defer("LaneSpoolHistory::written", [target]() { finish_write(*target); });
        },
        [alive, target](const MoonrakerError& err) {
            spdlog::warn("[LaneSpoolHistory] Not recorded: {}", err.message);
            alive.defer("LaneSpoolHistory::write_failed", [target]() { finish_write(*target); });
        });
}

void run_record(IMoonrakerAPI& api) {
    Recorder& r = recorder();
    r.busy = true;
    r.api_alive = api.lifetime_token();
    const LifetimeToken alive = api.lifetime_token();
    IMoonrakerAPI* target = &api;
    api.database_get_item(
        kDbNamespace, kDbKey,
        [alive, target](const nlohmann::json& value) {
            alive.defer("LaneSpoolHistory::read", [target, value]() {
                write_history(*target, *base_for_record(&value, nullptr));
            });
        },
        [alive, target](const MoonrakerError& err) {
            alive.defer("LaneSpoolHistory::read_failed", [target, err]() {
                if (auto base = base_for_record(nullptr, &err)) {
                    write_history(*target, std::move(*base));
                    return;
                }
                // A read that failed says nothing about what is stored: writing
                // now would replace the whole history with these entries.
                spdlog::warn("[LaneSpoolHistory] Not recorded, history unreadable: {}",
                             err.message);
                Recorder& rec = recorder();
                rec.waiting.clear();
                rec.busy = false;
                rec.api_alive.reset();
            });
        });
}

} // namespace

void record(IMoonrakerAPI& api, const SlotInfo& slot) {
    const std::optional<Entry> entry = entry_from_slot(slot);
    if (!entry) {
        return;
    }
    Recorder& r = recorder();
    // A write whose API is gone never finishes; the next record starts afresh.
    if (r.busy && r.api_alive && r.api_alive->expired()) {
        r.busy = false;
        r.waiting.clear();
    }
    r.waiting.push_back(*entry);
    if (!r.busy) {
        run_record(api);
    }
}

} // namespace helix::lane_spool_history
