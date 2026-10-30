// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ace_vocabulary.h"
#include "ams_error.h"
#include "ams_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "hv/json.hpp"

/**
 * @file ace_slot_parse.h
 * @brief How an Anycubic ACE Pro states a bay and its dryer, for every driver
 *        that reports one.
 *
 * The ACE backend (ValgACE, native GoKlipper, the ACEPRO driver, the REST
 * bridge) and the multiACE backend on a Snapmaker U1 read the same hub
 * firmware's vocabulary: slot status words, an `[r,g,b]` colour, a material
 * under `material` or `type`, and a dryer object. One reader per field keeps
 * the backends from drifting apart. Pure: no I/O, no locks.
 */
namespace helix::ace {

/// The ACE Pro dryer's hardware range.
inline constexpr float kDryerMinTempC = 35.0f;
inline constexpr float kDryerMaxTempC = 55.0f;
inline constexpr int kDryerMaxDurationMin = 720;

/// An ACE Pro dryer's capabilities, inactive. ACE_START_DRYING and
/// ACE_STOP_DRYING are the whole surface, with no set-temperature-while-running
/// command, so both live-adjust flags stay false and every adjustment stops and
/// restarts the cycle.
[[nodiscard]] DryerInfo ace_pro_dryer_info();

/// The colour a slot states: an `[r,g,b]` triplet of numbers, or a hex string
/// in the shared lane colour grammar. Empty when the value carries no colour (a
/// short or non-numeric array, a string the grammar refuses, any other type).
/// That is a different answer from pure black, which ACE reports for an empty
/// bay and which is a real reading.
[[nodiscard]] std::optional<uint32_t> parse_slot_color(const nlohmann::json& color_val);

/// One slot's material: `material` (ACEPRO driver / native / multiACE) wins over
/// `type` (ValgACE) when both are stated. Empty or absent states no reading.
[[nodiscard]] std::optional<std::string> read_slot_material(const nlohmann::json& slot_json);

/// The driver tool a slot entry names in `tool`, when it is an integer in
/// [0, @p tool_limit). Anything else (absent, not a number, out of range)
/// names none: the number sizes the published tool map.
[[nodiscard]] std::optional<int> read_slot_tool(const nlohmann::json& slot_json, int tool_limit);

/// What a slot entry's `rfid` field says about the bay's tag reader.
enum class TagRead {
    Silent,     ///< no `rfid` field: the frame says nothing about the reader
    Memory,     ///< false: the hub states its own memory of the bay, not a read
    InProgress, ///< 0 (idle, before a read starts) or 3 (identifying)
    NoTag,      ///< 1: a finished read that found no tag
    Identified, ///< true, or 2: this frame's material and colour came off the tag
};

/// Decode `rfid`: ValgACE sends a bool, the multiACE lineage the integers of
/// ACEResearch's PROTOCOL.md (0 not found, 1 failed to identify, 2 identified,
/// 3 identifying).
[[nodiscard]] TagRead read_tag_state(const nlohmann::json& slot_json);

/// empty/runout -> EMPTY; available/loaded/ready/preload/running -> AVAILABLE;
/// anything else (including "unknown") -> UNKNOWN.
[[nodiscard]] SlotStatus slot_status_from_string(const std::string& status_str);

/// What one status-shaped object states about its dryer, under either
/// spelling: `dryer` (ValgACE/native) or `dryer_status` (ACEPRO driver,
/// multiACE), each with either nested key set ({status, target_temp, duration,
/// remain_time} or {active, current_temp, remaining_minutes, duration_minutes}),
/// plus the top-level ambient `temp`. Where both key sets state a field, the
/// second set's value is the reading.
struct DryerDelta {
    std::optional<bool> active;
    /// A dryer current temp, else the ambient `temp`.
    std::optional<float> current_temp_c;
    std::optional<float> target_temp_c;
    std::optional<int> duration_min;
    std::optional<int> remaining_min;

    /// Overlays the fields this delta carries; an omitted field keeps its value.
    void apply(DryerInfo& dryer) const;
};

[[nodiscard]] DryerDelta read_dryer_state(const nlohmann::json& data);

/// Fold one status-shaped object's dryer state into @p dryer, under either
/// spelling: `dryer` (ValgACE/native) or `dryer_status` (ACEPRO driver,
/// multiACE), each with either nested key set ({status, target_temp, duration,
/// remain_time} or {active, current_temp, remaining_minutes, duration_minutes}).
/// Also reads the top-level ambient `temp`, which a stated dryer current temp
/// overrides. Absent keys leave @p dryer as it was.
void apply_dryer_state(const nlohmann::json& data, DryerInfo& dryer);

/// The refusal for a drying request outside @p dryer's range, or nullopt when
/// @p temp_c and @p duration_min are both in range.
[[nodiscard]] std::optional<AmsError> check_dryer_request(const DryerInfo& dryer, float temp_c,
                                                          int duration_min);

/// The (temperature, duration) a stop-and-restart adjustment of a running
/// cycle starts with: a negative @p temp_c keeps the cycle's target, a negative
/// @p duration_min keeps what is left of it (the session length when the dryer
/// reports no remaining time).
[[nodiscard]] std::pair<float, int> restart_request(const DryerInfo& dryer, float temp_c,
                                                    int duration_min);

} // namespace helix::ace
