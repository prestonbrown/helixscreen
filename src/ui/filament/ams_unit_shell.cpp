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
    /// Lowercase; nullptr when the backend names one kind of unit. A leading '\n' anchors it to
    /// the start of the unit's name or model.
    const char* unit_name_part;
    const char* shell_id;
    const char* enclosed_shell_id; ///< nullptr when the unit is never enclosed.
    int only_lanes;                ///< Match only this many lanes; 0 for any.
    bool selector_rail = false;    ///< Match only units that report a linear selector.
    /// Match only the Nth unit (from 0) whose name matches this row, for hardware a backend
    /// reports as several units that draw differently; -1 for every one.
    int nth_named = -1;
    /// The whole device's shell, for a unit drawn as one slice of it; nullptr otherwise.
    const char* device_shell_id = nullptr;
    bool model_unreported = false; ///< Match only units that report no model.
};

[[maybe_unused]] std::string unit_name_key(const AmsUnit& unit) {
    return helix::text_io::to_lower("\n" + unit.name + "\n" + unit.model);
}

[[maybe_unused]] bool name_matches(const ShellRow& row, const std::string& key) {
    return !row.unit_name_part || key.find(row.unit_name_part) != std::string::npos;
}

/// How many units before unit_index carry a name this row matches.
[[maybe_unused]] int named_before(const ShellRow& row, const AmsSystemInfo& info, int unit_index) {
    int n = 0;
    for (int i = 0; i < unit_index; ++i)
        n += name_matches(row, unit_name_key(info.units[static_cast<size_t>(i)])) ? 1 : 0;
    return n;
}

/// An OpenAMS unit that reports no model, under any backend: the single-lane unit is the AMS
/// HT, and the rest are OpenAMS boards in an original AMS.
constexpr ShellRow unreported_openams(const char* backend_slug, const char* unit_name_part,
                                      bool single_lane) {
    return {backend_slug,
            unit_name_part,
            single_lane ? "ams_ht" : "ams",
            nullptr,
            single_lane ? 1 : 0,
            false,
            -1,
            nullptr,
            true};
}

constexpr ShellRow kShellRows[] = {
    {"ace", nullptr, "ace_pro", nullptr, 0},
    // Anchored type tokens come first, since an AFC instance name can be anything.
    // AFC calls the ACE Pro "ACE" and the ACE Pro 2 "ACE2"; one drawing is both.
    {"afc", "\nace ", "ace_pro", nullptr, 0},
    {"afc", "\nace2 ", "ace_pro", nullptr, 0},
    {"afc", "\ncfs ", "cfs", nullptr, 0},
    // "htlf " is the type token: Claymore instances are often named HTLF_claymore_N.
    {"afc", "htlf ", "htlf", nullptr, 0},
    // AFC types the HTLF Claymore "Claymore".
    {"afc", "\nclaymore ", "htlf", nullptr, 0},
    // Ahead of the Box Turtle row: AFC names read "<type> <instance>", and an instance may
    // be called anything.
    unreported_openams("afc", "openams", true),
    unreported_openams("afc", "openams", false),
    {"afc", "vivid", "btt_vivid", nullptr, 0},
    {"afc", "night_owl", "night_owl", nullptr, 0},
    {"afc", "bmcu", "bmcu", nullptr, 0},
    {"afc", "quattro_box", "quattro_box", nullptr, 0},
    {"afc", "emu ", "emu", nullptr, 0},
    {"afc", "turtle", "box_turtle", "box_turtle_enclosed", 0},
    // Happy Hare's display_name defaults to the unit's vendor, "QuattroBox".
    {"happy_hare", "quattro", "quattro_box", nullptr, 0},
    {"happy_hare", "mmx", "mmx", nullptr, 0},
    {"happy_hare", "3ms", "3ms", nullptr, 0},
    {"happy_hare", "emu", "emu", nullptr, 0},
    {"happy_hare", "kms", "kms", nullptr, 0},
    // A YAMMU reports vendor "Other"; only a display_name of "YAMMU" names it. Happy Hare
    // reports its two drawers as two units: the first is the upper drawer, the second the lower.
    {"happy_hare", "yammu", "yammu_lower", nullptr, 0, false, 1, "yammu"},
    {"happy_hare", "yammu", "yammu_upper", nullptr, 0, false, -1, "yammu"},
    // Happy Hare publishes the vendor as "VVD" and names the unit "BTT ViViD" by default.
    {"happy_hare", "vvd", "btt_vivid", nullptr, 0},
    {"happy_hare", "qidi", "qidi_box", nullptr, 0},
    {"happy_hare", "htlf", "htlf", nullptr, 0},
    {"happy_hare", "nightowl", "night_owl", nullptr, 0},
    {"happy_hare", "boxturtle", "box_turtle", "box_turtle_enclosed", 0},
    {"ifs", nullptr, "ad5x_ifs", nullptr, 0},
    // Happy Hare's vendor constants; only a real selector is one of these rails.
    {"happy_hare", "ercf", "ercf_rail", nullptr, 0, true},
    {"happy_hare", "tradrack", "tradrack_rail", nullptr, 0, true},
    {"cfs", nullptr, "cfs", nullptr, 0},
    // multiACE reports each ACE beside the SnapSwap as "ACE <n>".
    {"snapmaker", "\nace ", "ace_pro", nullptr, 0},
    {"qidi_box", nullptr, "qidi_box", nullptr, 0},
    // openams publishes each unit's family as its model; a family with no drawing keeps the logo.
    {"openams", "\nams1", "ams", nullptr, 0},
    {"openams", "\nams2", "ams_2_pro", nullptr, 0},
    {"openams", "\nams_ht", "ams_ht", nullptr, 0},
    {"openams", "\nboxturtle", "box_turtle", "box_turtle_enclosed", 0},
    {"openams", "\nnightowl", "night_owl", nullptr, 0},
    {"openams", "\nquattrobox", "quattro_box", nullptr, 0},
    {"openams", "\nbtt_vivid", "btt_vivid", nullptr, 0},
    {"openams", "\nqidi_box", "qidi_box", nullptr, 0},
    {"openams", "\nhtlf", "htlf", nullptr, 0},
    {"openams", "\nemu", "emu", nullptr, 0},
    {"openams", "\nkms", "kms", nullptr, 0},
    {"openams", "\nmmx", "mmx", nullptr, 0},
    {"openams", "\n3ms", "3ms", nullptr, 0},
    {"openams", "\nercf", "ercf_rail", nullptr, 0, true},
    {"openams", "\ntradrack", "tradrack_rail", nullptr, 0, true},
    // klipper_openams publishes no family.
    unreported_openams("openams", nullptr, true),
    unreported_openams("openams", nullptr, false),
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

#ifndef ESP_PLATFORM
namespace {

/// The first row that draws unit @p unit_index, or nullptr.
const ShellRow* row_for(const AmsSystemInfo& info, int unit_index) {
    if (unit_index < 0 || unit_index >= static_cast<int>(info.units.size()))
        return nullptr;
    const char* slug = ams_type_telemetry_slug(info.type);
    const AmsUnit& unit = info.units[static_cast<size_t>(unit_index)];
    const std::string unit_name = unit_name_key(unit);
    for (const ShellRow& row : kShellRows) {
        if (std::strcmp(row.backend_slug, slug) == 0 && name_matches(row, unit_name) &&
            (row.only_lanes == 0 || unit.slot_count == row.only_lanes) &&
            (!row.selector_rail || unit.topology == PathTopology::LINEAR) &&
            (!row.model_unreported || unit.model.empty()) &&
            (row.nth_named < 0 || named_before(row, info, unit_index) == row.nth_named))
            return &row;
    }
    return nullptr;
}

const UnitShell* shell_of(const ShellRow& row, const AmsUnit& unit, bool has_dryer) {
    return unit_shell_by_id(row.enclosed_shell_id && has_climate(unit, has_dryer)
                                ? row.enclosed_shell_id
                                : row.shell_id);
}

} // namespace
#endif

const UnitShell* unit_shell_for(const AmsSystemInfo& info, int unit_index, bool has_dryer) {
#ifdef ESP_PLATFORM
    // The firmware image carries no unit art and stays on logos.
    (void)info;
    (void)unit_index;
    (void)has_dryer;
    return nullptr;
#else
    const ShellRow* row = row_for(info, unit_index);
    return row ? shell_of(*row, info.units[static_cast<size_t>(unit_index)], has_dryer) : nullptr;
#endif
}

DeviceShell device_shell_for(const AmsSystemInfo& info, int unit_index, bool has_dryer) {
#ifdef ESP_PLATFORM
    (void)info;
    (void)unit_index;
    (void)has_dryer;
    return {};
#else
    const ShellRow* row = row_for(info, unit_index);
    if (!row)
        return {};
    if (!row->device_shell_id)
        return {shell_of(*row, info.units[static_cast<size_t>(unit_index)], has_dryer),
                {unit_index}};
    DeviceShell device{unit_shell_by_id(row->device_shell_id), {}};
    for (int i = 0; i < static_cast<int>(info.units.size()); ++i) {
        const ShellRow* other = row_for(info, i);
        if (other && other->device_shell_id &&
            std::strcmp(other->device_shell_id, row->device_shell_id) == 0)
            device.unit_indices.push_back(i);
    }
    return device;
#endif
}

} // namespace helix::ui::tray
