// include/ams_status_json.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"
#include "json_utils.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace helix::ams {

// The typed readers live in json_util; the AMS parsers name them unqualified.
using json_util::read_field;
using json_util::read_integer;
using json_util::read_integer_field;
using json_util::read_scalar;

/// Every entry of the array @p obj[key], each read by @p reader, for a lane
/// array whose length is the firmware's to decide. nullopt when the key is
/// missing or not an array; otherwise one entry per array element, nullopt where
/// @p reader refused it, so the size stays what the frame sent.
template <typename T, typename Reader = std::optional<T> (*)(const nlohmann::json&)>
[[nodiscard]] std::optional<std::vector<std::optional<T>>>
read_array(const nlohmann::json& obj, const char* key, Reader reader = read_scalar<T>) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) {
        return std::nullopt;
    }
    std::vector<std::optional<T>> out;
    out.reserve(it->size());
    for (const auto& entry : *it) {
        out.push_back(reader(entry));
    }
    return out;
}

/// The first N entries of the array @p obj[key], each read by @p reader. An
/// entry is nullopt when the array is missing, shorter than its index, or holds
/// something @p reader refuses, so a delta frame that omits or mistypes a lane
/// says nothing about it.
template <typename T, std::size_t N, typename Reader = std::optional<T> (*)(const nlohmann::json&)>
[[nodiscard]] std::array<std::optional<T>, N>
read_indexed(const nlohmann::json& obj, const char* key, Reader reader = read_scalar<T>) {
    std::array<std::optional<T>, N> out;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) {
        return out;
    }
    for (std::size_t i = 0; i < N && i < it->size(); ++i) {
        out[i] = reader((*it)[i]);
    }
    return out;
}

} // namespace helix::ams
