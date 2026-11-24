// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_bypass_policy.h"

#include "macro_choice.h"
#include "printer_discovery.h"
#include "settings_manager.h"

#include <algorithm>

namespace helix {

BypassMacros detect_bypass_macros(const PrinterDiscovery& hw, const BypassMacros& conventional) {
    return {hw.has_macro(conventional.on) ? conventional.on : "",
            hw.has_macro(conventional.off) ? conventional.off : ""};
}

std::vector<std::string> bypass_macro_options(const PrinterDiscovery& hw) {
    std::vector<std::string> out;
    for (const auto& macro : hw.macros()) {
        if (macro.find("BYPASS") != std::string::npos) {
            out.push_back(macro);
        }
    }
    if (out.empty()) {
        return out;
    }
    std::sort(out.begin(), out.end());
    out.insert(out.begin(), kAutoMacro);
    return out;
}

BypassMacros resolve_bypass_macros(const BypassMacros& detected, const std::string& on_choice,
                                   const std::string& off_choice) {
    BypassMacros macros{resolve_macro_choice(on_choice, detected.on),
                        resolve_macro_choice(off_choice, detected.off)};

    // Half a pair cannot round-trip: engaging bypass with no way back strands
    // the machine in a state the UI offers no exit from.
    if (macros.on.empty() || macros.off.empty()) {
        return {};
    }
    return macros;
}

BypassMacros resolve_bypass_macros(const PrinterDiscovery& hw, const BypassMacros& conventional,
                                   const std::string& on_override,
                                   const std::string& off_override) {
    return resolve_bypass_macros(detect_bypass_macros(hw, conventional), on_override, off_override);
}

bool bypass_available_for(const AmsSystemInfo& info) {
    if (info.supports_bypass) {
        return true; // no need to touch settings when the firmware already says yes
    }
    return bypass_available(info.supports_bypass,
                            SettingsManager::instance().get_ams_force_bypass_controls(),
                            bypass_override_offered(info));
}

} // namespace helix
