// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

#include <string>
#include <unordered_map>

namespace helix {

/// Results from analyzing M106/M107/M141 macro gcode text
struct MacroFanAnalysis {
    /// Map of "output_pin fanN" -> M106 index N
    std::unordered_map<std::string, int> fan_indices;
    /// Map of "output_pin fanN" -> suggested display name/role
    std::unordered_map<std::string, std::string> role_hints;
};

/// Analyzes Klipper macro gcode text to extract fan index mappings and role hints.
/// Used to auto-detect output_pin fan roles on Creality printers.
class MacroFanAnalyzer {
  public:
    /// Analyze configfile.settings JSON for fan-related macros.
    /// Looks for gcode_macro m106, m107, m141 keys.
    MacroFanAnalysis analyze(const nlohmann::json& config_settings) const;

  private:
    /// Extract SET_PIN PIN=fanN patterns from gcode text
    void extract_set_pin_fans(const std::string& gcode, MacroFanAnalysis& result) const;
    /// Check M141 macro for chamber circulation fan references
    void extract_m141_roles(const std::string& gcode, MacroFanAnalysis& result) const;
};

} // namespace helix
