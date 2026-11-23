// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "json_fwd.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace helix::unit_override {

/// Per-tile unit text for a sensor-style home tile (prestonbrown/helixscreen#939).
/// The text replaces the unit after the tile's readings. It never converts the
/// reading: an empty string draws no unit, and nullopt keeps the tile's default.

/// The widget-config key the override persists under.
constexpr const char* kKey = "unit_override";
/// Longest unit text the dialog accepts, in characters.
constexpr uint32_t kMaxChars = 8;
/// Longest unit text stored, in bytes: kMaxChars of four-byte UTF-8, and within
/// the subject buffers that draw it.
constexpr size_t kMaxBytes = kMaxChars * 4;

/// @p unit cut to kMaxBytes on a character boundary.
std::optional<std::string> clamp(std::optional<std::string> unit);

/// The override stored in @p config, or nullopt when there is none.
std::optional<std::string> from_config(const nlohmann::json& config);

/// Store @p unit in @p config, or drop the key when it is nullopt.
void to_config(nlohmann::json& config, const std::optional<std::string>& unit);

/// The unit a tile draws: the override itself when set, else @p default_unit.
std::string resolve(const std::optional<std::string>& unit, const std::string& default_unit);

/// The configure card's Unit row text: "None" for an empty override, else resolve().
std::string unit_row_value(const std::optional<std::string>& unit, const std::string& default_unit);

/// Ask for the unit text in a text-entry dialog. @p apply_live runs on accept
/// while @p token lives; once a rebuild has retired it, the answer goes to the
/// stored config of @p instance_id on @p panel and the panel rebuilds.
void show_entry(std::function<void(const std::string&)> apply_live, LifetimeToken token,
                std::string panel, std::string instance_id, std::string initial);

} // namespace helix::unit_override
