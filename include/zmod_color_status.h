// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Z-Mod's `zmod_color` Klipper status object. The AD5X (IFS lanes) and the
// Creator 5 Pro (toolheads) publish the same schema, so both readers parse it
// here.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "hv/json.hpp"

namespace helix::zmod_color {

struct Slot {
    /// Empty when the firmware reports its "?" unset sentinel: a live frame
    /// sends Material "?" with HEX "", which would otherwise render as a
    /// literal "?" in the UI.
    std::string material;
    std::string hex; ///< As published, no '#'; may be empty
};

/// `slots[]`, indexed by 1-based `ID` minus one and sized @p max_slots. Entries
/// the frame did not carry, or carried malformed, stay nullopt. nullopt overall
/// when the object has no `slots` array: a delta frame that did not change it.
std::optional<std::vector<std::optional<Slot>>> parse_slots(const nlohmann::json& obj,
                                                            int max_slots);

/// `palette[]` as (index, 0xRRGGBB) in firmware index order. An unparseable
/// entry is skipped and the rest keep their own index. nullopt when absent.
std::optional<std::vector<std::pair<int, std::uint32_t>>> parse_palette(const nlohmann::json& obj);

/// `valid_types[]` without the "?" sentinel. nullopt when absent.
std::optional<std::vector<std::string>> parse_valid_types(const nlohmann::json& obj);

} // namespace helix::zmod_color
