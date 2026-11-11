// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Lane Status Surface (lane-status-spec RFC 0001) as a consumer reads it:
// the `lane_status` Klipper object, and the `lane_data` records its producer
// writes, joined to the object's lanes by `lane_name`. Pure functions over
// JSON, so every field's reading is testable without a backend.

#pragma once

#include "ams_types.h"
#include "filament_slot_override.h"
#include "json_fwd.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::lane_status {

/// The Klipper object every producer publishes (SPEC.md §2.1).
inline constexpr const char* kObject = "lane_status";

/// The spec version major this reader understands. A minor bump only adds
/// optional keys; another major may change meaning (REFERENCE.md §8 V1).
inline constexpr std::string_view kSpecMajor = "0";

/// The capability tokens this reader acts on (REFERENCE.md Part A §6.2).
inline constexpr const char* kCapabilityLoad = "load";
inline constexpr const char* kCapabilityUnload = "unload";

enum class Kind {
    Unknown,    ///< Absent or a value the spec does not define.
    StatusOnly, ///< Reports another system and drives nothing.
    Controller, ///< Drives the hardware through the commands it lists.
};

/// One reading of a `lane_status` object, every key the spec defines.
struct Status {
    /// spec_version names a major this reader understands. False means the
    /// object is not read at all: no lanes, no commands.
    bool readable = false;
    std::string spec_version;
    std::string producer_name;
    std::string producer_version;
    std::string producer_url;
    Kind kind = Kind::Unknown;
    std::string wraps;
    std::vector<std::string> capabilities;
    std::string automap;
    std::vector<std::string> compat_profiles;
    std::vector<std::string> units;
    /// Lane names in display order, only those usable as a G-code parameter
    /// (`[A-Za-z0-9_]+`) and each once.
    std::vector<std::string> lanes;
    /// Empty when no lane feeds the active extruder.
    std::string current_lane;
    std::string state;
    bool error_active = false;
    std::string error_message;
    std::string error_type;

    [[nodiscard]] bool has_capability(std::string_view token) const;
    /// A command is offered only by a controller that lists it.
    [[nodiscard]] bool offers(std::string_view token) const;
    /// The display index of @p lane in lanes, or -1.
    [[nodiscard]] int lane_index(std::string_view lane) const;
};

/// Read a whole `lane_status` object (the merge of every update so far).
/// A key that is absent, null or of the wrong type reads as its empty value;
/// keys the spec does not define are ignored.
[[nodiscard]] Status parse_status(const nlohmann::json& object);

/// True for a name the spec allows (`[A-Za-z0-9_]+`, SPEC.md §2.2).
[[nodiscard]] bool is_lane_name(std::string_view name);

/// The action a producer's `state` and `error` describe. The vocabulary is
/// AFC's capitalized words (REFERENCE.md §4.1); one this reader does not
/// know reads as idle.
[[nodiscard]] AmsAction action_for(const Status& status);

/// One lane's `lane_data` record, read for the consumer.
struct LaneRecord {
    /// Identity in the shape HelixScreen's own records read into.
    ams::FilamentSlotOverride identity;
    /// The record's explicit presence; nullopt when it states none.
    std::optional<bool> loaded;
    /// The slicer tool the lane serves (`tool`: "T3"), or -1.
    int tool = -1;
};

/// The record each of @p lanes reads from a `lane_data` namespace document,
/// parallel to @p lanes.
///
/// A record joins a lane by its `lane_name` (the outer key is advisory and
/// `lane` is the slicer's index, not the display order). A record whose
/// `writer` names another producer than @p producer is skipped; a legacy
/// record with no `writer` is read only for a lane the producer wrote no
/// record for, and joins by `lane_name`, else by its `lane` index. A record
/// whose `lss_version` major is not one this reader knows is skipped.
[[nodiscard]] std::vector<std::optional<LaneRecord>>
join_lane_records(const nlohmann::json& namespace_doc, const std::vector<std::string>& lanes,
                  const std::string& producer);

} // namespace helix::lane_status
