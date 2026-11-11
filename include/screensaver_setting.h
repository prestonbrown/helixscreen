// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_registry.h"

#include <string>

#include "hv/json.hpp" // JSON_HPP_OK: inline reader of the json

/**
 * @file screensaver_setting.h
 * @brief Reading the persisted screensaver choice
 *
 * The setting holds a stable registry name. It is read leniently, because settings.json is
 * hand-edited, restored from old backups and edited through Moonraker.
 */

namespace helix::ui {

/// Config path of the persisted screensaver choice, a name such as "maze" or "off".
inline constexpr const char* kScreensaverSettingPath = "/display/screensaver_type";

/// The screensaver a stored value selects: a registered name or "off" in any case, or a legacy
/// integer type number read through kLegacyScreensaverTypeNames. A missing node, an unknown
/// name, an out-of-range number or any other JSON type reads as `fallback`.
inline ScreensaverType screensaver_from_setting(const nlohmann::json* node,
                                                ScreensaverType fallback) {
    if (node == nullptr) {
        return fallback;
    }
    if (node->is_string()) {
        return screensaver_from_name(node->get_ref<const std::string&>()).value_or(fallback);
    }
    if (node->is_number_integer()) {
        const char* name = legacy_screensaver_name(node->get<long long>());
        return name != nullptr ? screensaver_from_name(name).value_or(fallback) : fallback;
    }
    return fallback;
}

} // namespace helix::ui
