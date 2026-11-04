// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "json_fwd.h"
#include "moonraker_types.h"
#include "spoolman_types.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

/// The context tables plugin menu handlers receive (docs/devel/PLUGIN_DEVELOPMENT.md,
/// "Menu items"). Pure: built from the facts each menu already holds. A fact the
/// printer did not report, or that has not loaded yet, is left out of the table, so
/// the handler reads it as nil.
namespace helix::plugin_menu {

using json = nlohmann::json;

/// What the print details screen knows about the file it shows.
struct PrintFileFacts {
    std::string name;
    std::string dir;
    std::string local_path; ///< non-empty for a USB file
    std::optional<FileMetadata> meta;
    std::vector<std::string> materials; ///< per tool, as the details screen shows them
    std::vector<std::string> colors;    ///< per tool, "#RRGGBB"
    std::vector<double> tool_grams;     ///< per tool; empty when unknown
    std::set<int> tools_used;           ///< empty when unknown
};

json print_file_context(const PrintFileFacts& facts);
/// `global_index` is the slot's index across every unit, `unit` the unit holding it
/// (null when unknown), `label` the slot's name as the menu header shows it.
json slot_context(const SlotInfo& slot, int global_index, const AmsUnit* unit,
                  const std::string& label);
json spool_context(const SpoolInfo& spool);
/// `widget_id` is the full `<plugin>__<name>` id; the table carries the declared name.
json tile_context(const std::string& widget_id);

} // namespace helix::plugin_menu
