// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

#include "hv/json.hpp"

namespace helix {

/// Persisted per-instance config for a FavoriteMacroWidget.
struct FavoriteMacroConfig {
    std::string macro;  ///< Assigned macro name (empty = unconfigured)
    std::string icon;   ///< Custom icon name (empty = "play" default)
    uint32_t color = 0; ///< Custom icon color RGB (0 = theme default)

    /// Gate every run behind a prompt: the parameter-entry modal when the macro
    /// takes parameters, otherwise the confirmation dialog. Off means one tap
    /// runs the macro with no parameters and no dialog. Dangerous macros keep
    /// their own hard confirmation either way.
    ///
    /// Stored as "require_confirmation". Configs written before that key existed
    /// carry the inverse "skip_param_prompt", which favorite_macro_config_from_json()
    /// still reads; see also migrate_v22_to_v23() in src/system/config.cpp.
    bool require_confirmation = true;
};

/// Parse config from PanelWidgetConfig JSON (tolerant of missing/wrong types).
FavoriteMacroConfig favorite_macro_config_from_json(const nlohmann::json& j);

/// Serialize config, omitting default-valued fields to keep JSON minimal.
nlohmann::json favorite_macro_config_to_json(const FavoriteMacroConfig& c);

} // namespace helix
