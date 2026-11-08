// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include "hv/json.hpp"

namespace helix {

/// Whether a printer.gcode.help reply (the full JSON-RPC envelope) lists
/// @p command; nullopt when the reply carries no command list.
///
/// Klipper lists only commands registered with a description, which every
/// command a caller here asks about has. An unknown command is not an error
/// to Klipper, so this list is the only way to tell one apart before sending.
[[nodiscard]] inline std::optional<bool> gcode_help_lists(const nlohmann::json& response,
                                                          const char* command) {
    const auto result = response.find("result");
    if (result == response.end() || !result->is_object()) {
        return std::nullopt;
    }
    return result->contains(command);
}

} // namespace helix
