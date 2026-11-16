// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hv/json.hpp" // JSON_HPP_OK: a std::optional json member

namespace helix {
class PrinterDiscovery;
} // namespace helix

/**
 * @file multiace_status.h
 * @brief multiACE's engine contract on a Snapmaker U1, read into plain structs.
 *
 * multiACE (decay71/multiACE) hangs Anycubic ACE Pro units off the U1's four
 * heads. Its stable contract is a gcode vocabulary and one `ace` status object
 * (its docs/ENGINE_API.md, `api_version` 1). Every index on the wire is
 * 0-based: heads, ACE units and bays alike.
 *
 * Moonraker sends deltas: a frame names only the top-level fields that
 * changed, so every parsed field is a std::optional and an omitted one reads
 * as nullopt. A field that arrives replaces what was held, whole (`aces`
 * arrives whole whenever any nested value moves). Pure: no I/O, no locks.
 */
namespace helix::multiace {

/// The heads on a U1.
inline constexpr int kHeadCount = 4;
/// Bays on one ACE Pro.
inline constexpr int kBaysPerUnit = 4;
/// The only engine contract this reader speaks.
inline constexpr int kApiVersion = 1;

/// The engine's operating mode. NORMAL swaps the stock firmware files back in
/// and drives no ACE; MULTI feeds bay h of every unit to head h; HEAD wires
/// each ACE-fed head to one unit and swaps between its four bays.
enum class Mode { NORMAL, MULTI, HEAD, UNKNOWN };

/// How one head is fed: by an ACE, by the U1's own side feeder, or by hand.
enum class HeadFeed { ACE, FEEDER, MANUAL };

/// The bay feeding a head (`head_source[h]`), as multiACE recorded it on load.
struct HeadSource {
    int ace = -1;
    int slot = -1;
    std::string type;
    /// Bare hex as the engine spells it: "" is unknown, "000000" a declared
    /// black. head_source_color() reads it.
    std::string color_hex;
    std::string brand;
    std::string subtype;
    bool load_failed = false;
};

/// One bay of one unit (`aces[k].slots[n]`).
struct Bay {
    int index = 0;
    SlotStatus status = SlotStatus::UNKNOWN;
    std::string material, subtype, brand, sku, uid;
    /// nullopt when the bay states no colour, including the raw [0,0,0] the
    /// device reports for a bay with no material.
    std::optional<uint32_t> color;
    int rfid = 0;
};

/// One ACE unit (`aces[k]`).
struct Unit {
    int idx = 0;
    bool connected = false;
    std::string protocol, model, firmware, status, connect_error;
    std::optional<float> temp, humidity;
    /// ACE Pro capabilities with this unit's reported dryer state laid over.
    DryerInfo dryer;
    std::vector<int> gate_status;
    int feed_assist = -1;
    std::vector<Bay> bays;
};

/// `last_swap_result`: the most recent swap's outcome.
struct LastSwap {
    int head = -1, ace = -1, slot = -1;
    std::string status;
};

/// Everything one frame said about the `ace` object.
struct StatusDelta {
    std::optional<int> api_version;
    std::optional<Mode> mode;
    std::optional<std::array<int, kHeadCount>> head_ace;
    std::optional<std::array<bool, kHeadCount>> head_feeder, head_manual;
    /// nullopt entry: that head is unloaded.
    std::optional<std::array<std::optional<HeadSource>, kHeadCount>> head_source;
    std::optional<std::vector<int>> ace_heads;
    std::optional<std::vector<Unit>> aces;
    std::optional<bool> swap_in_progress, unload_all_active, quad_replenish, quad_first;
    std::optional<std::string> swap_phase;
    /// Engaged with a default LastSwap when the field arrives as null.
    std::optional<LastSwap> last_swap_result;
    std::optional<int64_t> event_seq;
    std::optional<nlohmann::json> spools, spool_binding;
};

/// One `multiace_event <name> key=val ... seq=<n>` line from the console.
struct Event {
    std::string name;
    std::map<std::string, std::string> fields;
    int64_t seq = -1;
};

/// A Snapmaker U1 running multiACE: the U1's filament_detect beside a plain
/// `ace` object. A bare `ace`, `ace_device` or `filament_hub` is some other
/// ACE driver.
[[nodiscard]] bool present(const helix::PrinterDiscovery& hw);

[[nodiscard]] StatusDelta parse_ace_status(const nlohmann::json& ace);

/// The colour @p source names: nullopt for "" (unknown), a value otherwise,
/// "000000" being black.
[[nodiscard]] std::optional<uint32_t> head_source_color(const HeadSource& source);

/// How @p mode feeds a head flagged @p manual / @p feeder: manual wins, and the
/// feeder flag counts only in HEAD mode.
[[nodiscard]] HeadFeed head_feed(Mode mode, bool manual, bool feeder);

/// The (ace, bay) pairs that can feed @p head: in HEAD mode the four bays of
/// the unit it is wired to, in MULTI mode bay @p head of every unit, and none
/// for a head the ACE does not feed or in any other mode.
[[nodiscard]] std::vector<std::pair<int, int>>
candidate_bays(Mode mode, int head, const std::array<int, kHeadCount>& head_ace,
               const std::array<HeadFeed, kHeadCount>& feed, int unit_count);

/// The head bay (@p ace, @p slot) can feed, or nullopt when none can. HEAD mode
/// answers the ACE-fed head wired to @p ace (the engine refuses a load from any
/// other unit); MULTI mode answers head @p slot when the ACE feeds it.
[[nodiscard]] std::optional<int> head_for_bay(Mode mode, int ace, int slot,
                                              const std::array<int, kHeadCount>& head_ace,
                                              const std::array<HeadFeed, kHeadCount>& feed);

/// The global slot index of bay @p slot on ACE @p ace. Indices 0..3 are the
/// U1 heads; bays follow in unit order.
[[nodiscard]] constexpr int bay_global(int ace, int slot) {
    return kHeadCount + ace * kBaysPerUnit + slot;
}

/// The (ace, bay) a global slot index names, or nullopt for a head index.
[[nodiscard]] std::optional<std::pair<int, int>> bay_of(int global);

/// Parse one console line. The engine emits it through Klipper's respond, so a
/// leading "// " is accepted. nullopt for any other line, a missing name, or a
/// token that is not key=value.
[[nodiscard]] std::optional<Event> parse_event_line(std::string_view line);

/// `ACE_LOAD_HEAD HEAD=h`, plus `ACE=k SLOT=s` when @p bay is set; a bay is
/// always sent whole, since the engine defaults a missing SLOT per mode. The
/// bare form leaves the bay to the engine, which is well-defined only in HEAD
/// mode and for a head the ACE does not feed.
[[nodiscard]] std::string load_gcode(int head, std::optional<std::pair<int, int>> bay);
[[nodiscard]] std::string unload_gcode(int head);
[[nodiscard]] std::string unload_all_gcode();
[[nodiscard]] std::string dry_gcode(int ace, int temp_c, int duration_min);
[[nodiscard]] std::string stop_dry_gcode(int ace);

} // namespace helix::multiace
