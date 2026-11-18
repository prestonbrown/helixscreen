// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The fan object a tool's own `fan` names. klipper-toolchanger's `[tool] fan:`
// takes a fan object name or a bare fan_generic name and looks them up in that
// order, and its tool status reports the name as written.

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace helix::tool_fans {

/// The fan objects @p tool_fan can name, in the order they are looked up.
[[nodiscard]] inline std::array<std::string, 2> object_candidates(const std::string& tool_fan) {
    return {tool_fan, "fan_generic " + tool_fan};
}

/// The fan object in @p objects that @p tool_fan names, or empty.
[[nodiscard]] inline std::string object_for(const std::string& tool_fan,
                                            const std::vector<std::string>& objects) {
    if (tool_fan.empty())
        return {};
    for (const auto& candidate : object_candidates(tool_fan)) {
        if (std::find(objects.begin(), objects.end(), candidate) != objects.end())
            return candidate;
    }
    return {};
}

} // namespace helix::tool_fans
