// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_subscription_merge.h"

#include <string>
#include <unordered_set>

#include "hv/json.hpp"

namespace helix {

namespace {

bool is_field_list(const nlohmann::json& v) {
    if (!v.is_array())
        return false;
    for (const auto& f : v) {
        if (!f.is_string())
            return false;
    }
    return true;
}

} // namespace

nlohmann::json merge_subscription_objects(const nlohmann::json& app, const nlohmann::json& extra) {
    nlohmann::json merged = app;
    if (!extra.is_object())
        return merged;

    for (auto it = extra.begin(); it != extra.end(); ++it) {
        const auto& fields = it.value();
        if (!fields.is_null() && !is_field_list(fields))
            continue;

        auto existing = merged.find(it.key());
        if (existing == merged.end()) {
            merged[it.key()] = fields;
            continue;
        }
        if (existing->is_null())
            continue;
        if (fields.is_null()) {
            *existing = nullptr;
            continue;
        }
        if (!existing->is_array())
            continue;

        std::unordered_set<std::string> seen;
        for (const auto& f : *existing) {
            if (f.is_string())
                seen.insert(f.get<std::string>());
        }
        for (const auto& f : fields) {
            if (seen.insert(f.get<std::string>()).second)
                existing->push_back(f);
        }
    }
    return merged;
}

} // namespace helix
