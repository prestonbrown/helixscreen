// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The unit_conversions.h conversions applied to a field of a Moonraker status object.
#pragma once

#include "unit_conversions.h"

#include "hv/json.hpp" // JSON_HPP_OK: inline readers of json fields

namespace helix::units {

/**
 * @brief Extract temperature from JSON and convert to decidegrees
 * @param obj JSON object containing the temperature field
 * @param key Key name for the temperature value
 * @param default_value Value to return if key is missing or not a number
 * @return Temperature in decidegrees
 *
 * @note Returns default_value if obj is not a JSON object (e.g., array, primitive, null)
 */
inline int json_to_decidegrees(const nlohmann::json& obj, const char* key, int default_value = 0) {
    if (!obj.is_object())
        return default_value;
    if (obj.contains(key) && obj[key].is_number()) {
        return to_decidegrees(obj[key].get<double>());
    }
    return default_value;
}

/**
 * @brief Extract ratio from JSON and convert to percent
 * @param obj JSON object containing the ratio field
 * @param key Key name for the ratio value
 * @param default_value Value to return if key is missing or not a number
 * @return Percent as integer
 *
 * @note Returns default_value if obj is not a JSON object (e.g., array, primitive, null)
 */
inline int json_to_percent(const nlohmann::json& obj, const char* key, int default_value = 0) {
    if (!obj.is_object())
        return default_value;
    if (obj.contains(key) && obj[key].is_number()) {
        return to_percent(obj[key].get<double>());
    }
    return default_value;
}

/**
 * @brief Extract length from JSON and convert to centimillimeters
 * @param obj JSON object containing the length field
 * @param key Key name for the length value (in mm)
 * @param default_value Value to return if key is missing or not a number
 * @return Length in centimillimeters
 *
 * @note Returns default_value if obj is not a JSON object (e.g., array, primitive, null)
 */
inline int json_to_centimm(const nlohmann::json& obj, const char* key, int default_value = 0) {
    if (!obj.is_object())
        return default_value;
    if (obj.contains(key) && obj[key].is_number()) {
        return to_centimm(obj[key].get<double>());
    }
    return default_value;
}

} // namespace helix::units
