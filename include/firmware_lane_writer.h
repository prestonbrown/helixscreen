// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Firmware that writes the lane records itself.
//
// On most printers with a filament system HelixScreen either reads lane_data
// from the firmware's own plugin (AFC, Happy Hare) or writes it as the only
// writer (AD5X IFS, CFS, ACE). Some firmwares grow their own lane_data writer
// later, and once they do, lane_data must keep one writer: HelixScreen sends
// every slot edit through the firmware's edit command, with every field the
// record carries, and keeps its own notes in a private namespace.
//
// This module is the ONLY place that knows which firmwares do that, from which
// version, and how their edit command spells the fields. Backends ask these
// functions and never name a firmware. Adding one means adding one Provider to
// the table in firmware_lane_writer.cpp.

#include "json_fwd.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace helix {
class PrinterDiscovery;
} // namespace helix

namespace helix::lane_writer {

/// Klipper status objects that answer firmware_writes_lane_records(). Empty on
/// a printer whose objects list names no such firmware.
std::vector<std::string> required_status_objects(const PrinterDiscovery& hw);

/// Whether this printer's objects list names a firmware that may write the lane
/// records itself. Its version, and so the answer, arrives by status: a backend
/// that writes lane_data waits for firmware_writes_lane_records() before it
/// writes anything.
bool firmware_may_write_lane_records(const PrinterDiscovery& hw);

/// Whether the firmware writes lane_data itself and takes a slot's whole
/// identity on its edit command, read from one status frame. nullopt when the
/// frame does not say (the object or its version key is not in it): no news,
/// never a no.
std::optional<bool> firmware_writes_lane_records(const nlohmann::json& status);

/// What a slot edit states beyond type and color. An unset text field and a
/// non-positive number are sent empty, which clears the firmware's value; a
/// temperature left nullopt is not sent, so the firmware keeps its own.
struct SlotIdentity {
    int spool_id = 0;
    std::string vendor;
    std::string name;
    /// OrcaSlicer's filament preset id (orca_filament_id_for()).
    std::string filament_id;
    /// Read from status as a value, 0 when unset.
    std::optional<int> bed_temp;
    /// The nozzle temperature the user set. Sent only: the firmware's status
    /// states its own resolved temperature, never this one.
    std::optional<int> nozzle_temp;
};

/// The edit command's parameters for @p identity, each preceded by a space,
/// for a firmware where firmware_writes_lane_records() is true. Every
/// identity field goes out, set or cleared, because the caller holds the
/// slot's whole state; a temperature only when stated. Text goes out as
/// sendable_text(), logged when that dropped anything; a nozzle temperature
/// outside what the command takes is left out, logged.
std::string slot_identity_params(const SlotIdentity& identity);

/// @p text as the edit command can carry it inside quotes: without the
/// characters Klipper's parser cuts the line at or that end the quoting
/// (`"`, `\`, `#`, `;`, `*`), the ones the firmware refuses in a value, control
/// or non-ASCII bytes, and trimmed. What the firmware then stores.
std::string sendable_text(const std::string& text);

/// Per slot index (0-based), the identity the firmware states in @p status, a
/// status frame of a firmware where firmware_writes_lane_records() is true.
/// The spool link, vendor, name, Orca id and bed temperature are read; an
/// unset one reads empty or 0. A slot the frame does not carry, or carries
/// without these fields, is absent: no news for it.
std::map<int, SlotIdentity> slot_identities(const nlohmann::json& status);

} // namespace helix::lane_writer
