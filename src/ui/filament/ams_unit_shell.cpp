// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_unit_shell.h"

#include "ams_types.h"
#include "core/text_io.h"

#include <cstring>
#include <string>

namespace helix::ui::tray {

namespace {

/// Which hardware a shell draws. A backend that reports only its firmware has no
/// row, because a drawing of the wrong unit is worse than the generic tray.
struct ShellRow {
    const char* backend_slug;
    const char* unit_name_part; ///< Lowercase; nullptr when the backend names one kind of unit.
    const char* shell_id;
    const char* enclosed_shell_id; ///< nullptr when the unit is never enclosed.
    int only_lanes;                ///< Match only this many lanes; 0 for any.
};

constexpr ShellRow kShellRows[] = {
    {"ace", nullptr, "ace_pro", nullptr, 0},
    // Ahead of the Box Turtle row: AFC names read "<type> <instance>", and an instance may
    // be called anything.
    {"afc", "vivid", "btt_vivid", nullptr, 0},
    {"afc", "quattro_box", "quattro_box", nullptr, 0},
    {"afc", "turtle", "box_turtle", "box_turtle_enclosed", 0},
    // Happy Hare's display_name defaults to the unit's vendor, "QuattroBox".
    {"happy_hare", "quattro", "quattro_box", nullptr, 0},
    {"cfs", nullptr, "cfs", nullptr, 0},
    {"qidi_box", nullptr, "qidi_box", nullptr, 0},
    // An OpenAMS hub reports no model: its single-lane unit is the AMS HT.
    {"openams", nullptr, "ams_ht", nullptr, 1},
    {"openams", nullptr, "ams_2_pro", nullptr, 0},
};

/// Climate readings or a dryer mean the unit is closed up.
[[maybe_unused]] bool has_climate(const AmsUnit& unit, bool has_dryer) {
    if (has_dryer || unit.environment.has_value())
        return true;
    for (const SlotInfo& slot : unit.slots) {
        if (slot.environment.has_value())
            return true;
    }
    return false;
}

} // namespace

const UnitShell* unit_shell_for(const AmsSystemInfo& info, int unit_index, bool has_dryer) {
#ifdef ESP_PLATFORM
    // The firmware image carries no unit art and stays on logos.
    (void)info;
    (void)unit_index;
    (void)has_dryer;
    return nullptr;
#else
    if (unit_index < 0 || unit_index >= static_cast<int>(info.units.size()))
        return nullptr;
    const char* slug = ams_type_telemetry_slug(info.type);
    const AmsUnit& unit = info.units[static_cast<size_t>(unit_index)];
    const std::string unit_name = helix::text_io::to_lower(unit.name);
    for (const ShellRow& row : kShellRows) {
        if (std::strcmp(row.backend_slug, slug) == 0 &&
            (!row.unit_name_part || unit_name.find(row.unit_name_part) != std::string::npos) &&
            (row.only_lanes == 0 || unit.slot_count == row.only_lanes))
            return unit_shell_by_id(row.enclosed_shell_id && has_climate(unit, has_dryer)
                                        ? row.enclosed_shell_id
                                        : row.shell_id);
    }
    return nullptr;
#endif
}

} // namespace helix::ui::tray
