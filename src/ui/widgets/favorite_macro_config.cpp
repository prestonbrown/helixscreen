// SPDX-License-Identifier: GPL-3.0-or-later
#include "favorite_macro_config.h"

#include "json_utils.h"
#include "tile_color.h"

#include <algorithm>
#include <cstdint>

#include "hv/json.hpp"

namespace helix {

FavoriteMacroConfig favorite_macro_config_from_json(const nlohmann::json& j) {
    FavoriteMacroConfig c;
    if (auto macro = json_util::read_field<std::string>(j, "macro"))
        c.macro = std::move(*macro);
    if (auto icon = json_util::read_field<std::string>(j, "icon"))
        c.icon = std::move(*icon);
    c.color = tile_color::color_from_config(j);
    // Two spellings of the same switch. "require_confirmation" is what this
    // build writes; "skip_param_prompt" is the pre-v23 key with the opposite
    // polarity, still read here so a config that never passed through
    // migrate_v22_to_v23() (a preset asset, a hand-edited file, a widget config
    // restored from an older backup) keeps the user's choice instead of
    // silently reverting to the default.
    if (auto confirm = json_util::read_field<bool>(j, "require_confirmation")) {
        c.require_confirmation = *confirm;
    } else if (auto skip = json_util::read_field<bool>(j, "skip_param_prompt")) {
        c.require_confirmation = !*skip;
    }
    return c;
}

nlohmann::json favorite_macro_config_to_json(const FavoriteMacroConfig& c) {
    nlohmann::json j;
    j["macro"] = c.macro;
    if (!c.icon.empty())
        j["icon"] = c.icon;
    if (c.color != 0)
        j["color"] = c.color;
    // Default (true) is omitted; the legacy key is never written back, so a
    // config that still carries it sheds it on the next save.
    if (!c.require_confirmation)
        j["require_confirmation"] = false;
    return j;
}

std::vector<std::string> pickable_macro_names(const std::unordered_set<std::string>& macros) {
    std::vector<std::string> names;
    for (const auto& macro : macros) {
        if (!macro.empty() && macro[0] != '_')
            names.push_back(macro);
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace helix
