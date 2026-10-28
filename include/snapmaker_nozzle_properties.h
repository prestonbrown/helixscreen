// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

/**
 * @file snapmaker_nozzle_properties.h
 * @brief The U1's per-head nozzle record: what it reports, and how to change it.
 *
 * Each extruder object reports the nozzle fitted to its head (nozzle_diameter,
 * nozzle_volume_type), which the firmware keeps outside configfile.settings.
 * The firmware's control/nozzle_properties webhook, reached through Moonraker
 * as a JSON-RPC method, changes it. That call is a SETTER: a field it is not
 * given keeps its value, and a write is not free even when the value is the
 * same (a 0.2 mm write can clear the head's filament record). So a change
 * carries only the field that differs from what the head reports, and a pick
 * equal to the report sends nothing.
 *
 * The webhook refuses inside a successful reply: {"state": "error", "message":
 * ...}. A caller must read the reply, not only the transport's outcome.
 *
 * Pure: no I/O, no LVGL, no Moonraker. The backend owns the plumbing.
 */
namespace helix::snapmaker::nozzle {

/// The JSON-RPC method Moonraker registers for the firmware's webhook.
inline constexpr const char* kMethod = "printer.control.nozzle_properties";

/// How long a change may take to be answered.
inline constexpr uint32_t kTimeoutMs = 10000;

/// The diameters the firmware accepts, in mm. It compares them exactly, so a
/// write sends one of these values, never a reading.
inline constexpr std::array<double, 4> kDiameters = {0.2, 0.4, 0.6, 0.8};

/// The flow types the firmware accepts, in its own spelling.
inline constexpr const char* kStandard = "standard";
inline constexpr const char* kHighFlow = "high_flow";

/// One head's nozzle. Unset means the head has not reported that field.
struct Nozzle {
    std::optional<double> diameter;
    std::optional<std::string> volume_type;
};

/// The diameter choices a row lists: "0.2 mm" .. "0.8 mm", in kDiameters order.
[[nodiscard]] std::vector<std::string> diameter_options();

/// The flow-type choices a row lists: "Standard", "High Flow".
[[nodiscard]] std::vector<std::string> flow_options();

/// The diameter option naming @p mm, or "" for a diameter the firmware does
/// not accept.
[[nodiscard]] std::string diameter_option(double mm);

/// The flow option naming @p volume_type (the firmware's spelling), or "".
[[nodiscard]] std::string flow_option(const std::string& volume_type);

/// The kDiameters value @p option names, or nullopt for text no row lists.
[[nodiscard]] std::optional<double> diameter_from_option(const std::string& option);

/// The firmware spelling @p option names, or nullopt for text no row lists.
[[nodiscard]] std::optional<std::string> volume_type_from_option(const std::string& option);

/// The params of a change to head @p head: {"extruder": head} plus each field
/// @p wanted sets that differs from @p reported. A diameter the head has not
/// reported counts as differing. nullopt when nothing differs, so nothing is
/// sent.
[[nodiscard]] std::optional<nlohmann::json> change_params(int head, const Nozzle& reported,
                                                          const Nozzle& wanted);

/// How the firmware answered a change.
enum class ReplyKind {
    Accepted,  ///< {"state": "success"}
    Refused,   ///< {"state": "error"}; message is the firmware's own words, maybe empty
    Unexpected ///< any other shape: nothing says the change was made
};

struct Reply {
    ReplyKind kind = ReplyKind::Unexpected;
    std::string message;
};

/// Reads @p response, either the whole JSON-RPC message ({"result": {...}}) or
/// the result alone.
[[nodiscard]] Reply read_reply(const nlohmann::json& response);

} // namespace helix::snapmaker::nozzle
