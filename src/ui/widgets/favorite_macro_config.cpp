// SPDX-License-Identifier: GPL-3.0-or-later
#include "favorite_macro_config.h"

#include <cstdint>

namespace helix {

FavoriteMacroConfig favorite_macro_config_from_json(const nlohmann::json& j) {
    FavoriteMacroConfig c;
    if (j.contains("macro") && j["macro"].is_string())
        c.macro = j["macro"].get<std::string>();
    if (j.contains("icon") && j["icon"].is_string())
        c.icon = j["icon"].get<std::string>();
    if (j.contains("color") && j["color"].is_number_integer()) {
        int64_t v = j["color"].get<int64_t>();
        if (v >= 0 && v <= 0xFFFFFF)
            c.color = static_cast<uint32_t>(v);
    }
    // Two spellings of the same switch. "require_confirmation" is what this
    // build writes; "skip_param_prompt" is the pre-v23 key with the opposite
    // polarity, still read here so a config that never passed through
    // migrate_v22_to_v23() (a preset asset, a hand-edited file, a widget config
    // restored from an older backup) keeps the user's choice instead of
    // silently reverting to the default.
    if (j.contains("require_confirmation") && j["require_confirmation"].is_boolean()) {
        c.require_confirmation = j["require_confirmation"].get<bool>();
    } else if (j.contains("skip_param_prompt") && j["skip_param_prompt"].is_boolean()) {
        c.require_confirmation = !j["skip_param_prompt"].get<bool>();
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

} // namespace helix
