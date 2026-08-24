// SPDX-License-Identifier: GPL-3.0-or-later
#include "hardware_role_registry.h"

#include "config.h"
#include "printer_discovery.h"
#include "printer_hardware.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {
namespace {

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// --- is_candidate predicates: applied by Tier 1a/1b only (NOT Tier 0) ---
// These gate heuristic/canonical picks so a fallback guess can't pick a wrong-category object
// (e.g. guess_part_cooling_fan() falling back to fans_[0] which is a controller_fan).

// Part cooling: anything that is NOT an auto-controlled fan type.
bool is_part_fan_candidate(const std::string& o) {
    return o.rfind("heater_fan ", 0) != 0 && o.rfind("controller_fan ", 0) != 0 &&
           o.rfind("temperature_fan ", 0) != 0;
}
// Hotend / heatbreak cooling fan.
// NOTE: intentionally avoids bare "heat" to prevent matching "heater_bed".
bool is_hotend_fan_candidate(const std::string& o) {
    return o.rfind("heater_fan ", 0) == 0 || o.find("hotend") != std::string::npos ||
           o.find("heatbreak") != std::string::npos;
}
// Chamber circulation / filter fan.
bool is_chamber_fan_candidate(const std::string& o) {
    return o.find("chamber") != std::string::npos || o.find("nevermore") != std::string::npos ||
           o.find("filter") != std::string::npos || o.rfind("temperature_fan ", 0) == 0;
}
// Exhaust / vent fan.
bool is_exhaust_fan_candidate(const std::string& o) {
    return o.find("exhaust") != std::string::npos || o.find("vent") != std::string::npos ||
           o.find("external") != std::string::npos;
}
bool is_bed_heater_candidate(const std::string& o) {
    return o.find("bed") != std::string::npos;
}
bool is_hotend_heater_candidate(const std::string& o) {
    return o == "extruder" || o.rfind("extruder", 0) == 0 ||
           o.find("hotend") != std::string::npos || o == "e0";
}

// --- guess adapters: reuse the single heuristic implementation in PrinterHardware ---
std::string guess_part_fan(const std::vector<std::string>& fans) {
    return PrinterHardware({}, {}, fans, {}).guess_part_cooling_fan();
}
std::string guess_hotend_fan(const std::vector<std::string>& fans) {
    return PrinterHardware({}, {}, fans, {}).guess_hotend_fan();
}
std::string guess_chamber_fan(const std::vector<std::string>& fans) {
    return PrinterHardware({}, {}, fans, {}).guess_chamber_fan();
}
std::string guess_exhaust_fan(const std::vector<std::string>& fans) {
    return PrinterHardware({}, {}, fans, {}).guess_exhaust_fan();
}
std::string guess_bed_heater(const std::vector<std::string>& heaters) {
    return PrinterHardware(heaters, {}, {}, {}).guess_bed_heater();
}
std::string guess_hotend_heater(const std::vector<std::string>& heaters) {
    return PrinterHardware(heaters, {}, {}, {}).guess_hotend_heater();
}

const std::vector<HardwareRoleDescriptor>& registry() {
    static const std::vector<HardwareRoleDescriptor> REG = {
        {HardwareRoleId::HotendHeater, helix::wizard::HOTEND_HEATER, "extruder",
         HardwareCategory::Heater, helix::wizard::StepId::HeaterSelect, true,
         &is_hotend_heater_candidate, &guess_hotend_heater},
        {HardwareRoleId::BedHeater, helix::wizard::BED_HEATER, "heater_bed",
         HardwareCategory::Heater, helix::wizard::StepId::HeaterSelect, true,
         &is_bed_heater_candidate, &guess_bed_heater},
        {HardwareRoleId::PartFan, helix::wizard::PART_FAN, "fan", HardwareCategory::Fan,
         helix::wizard::StepId::FanSelect, true, &is_part_fan_candidate, &guess_part_fan},
        {HardwareRoleId::HotendFan, helix::wizard::HOTEND_FAN, "", HardwareCategory::Fan,
         helix::wizard::StepId::FanSelect, true, &is_hotend_fan_candidate, &guess_hotend_fan},
        {HardwareRoleId::ChamberFan, helix::wizard::CHAMBER_FAN, "", HardwareCategory::Fan,
         helix::wizard::StepId::FanSelect, true, &is_chamber_fan_candidate, &guess_chamber_fan},
        {HardwareRoleId::ExhaustFan, helix::wizard::EXHAUST_FAN, "", HardwareCategory::Fan,
         helix::wizard::StepId::FanSelect, true, &is_exhaust_fan_candidate, &guess_exhaust_fan},
    };
    return REG;
}

} // namespace

const std::vector<HardwareRoleDescriptor>& hardware_role_registry() {
    return registry();
}

const HardwareRoleDescriptor* role_descriptor(HardwareRoleId id) {
    for (const auto& d : registry()) {
        if (d.id == id)
            return &d;
    }
    return nullptr;
}

RoleResolution resolve_role(const HardwareRoleDescriptor& desc, const std::string& saved_value,
                            const std::vector<std::string>& discovered) {
    auto is_cand = [&](const std::string& o) {
        return desc.is_candidate == nullptr || desc.is_candidate(o);
    };

    // Tier 0: keep a still-valid saved role. User's explicit live choice is always honored —
    // candidacy is NOT checked here so a deliberately assigned wrong-category fan is kept.
    // Intentional trade-off: a hand-edited config naming a heater_fan as the part fan will be
    // kept, which means init_fans will treat it as PART_COOLING. The wizard's candidate filters
    // prevent creating such configs; Tier 0 defers to the user's judgment for everything else.
    if (!saved_value.empty() && contains(discovered, saved_value)) {
        return {RoleResolutionStatus::Resolved, saved_value};
    }

    // Tier 1a: confident canonical default. Must pass candidacy so a wrong-category canonical
    // (edge case) is not silently accepted.
    if (desc.canonical_default && *desc.canonical_default &&
        contains(discovered, desc.canonical_default) && is_cand(desc.canonical_default)) {
        return {RoleResolutionStatus::AutoHealed, desc.canonical_default};
    }

    // Tier 1b: heuristic guess. Must pass candidacy so the fallback (e.g. fans_[0]) is not
    // accepted when it is a controller_fan / heater_fan.
    if (desc.guess) {
        std::string g = desc.guess(discovered);
        if (!g.empty() && contains(discovered, g) && is_cand(g)) {
            return {RoleResolutionStatus::AutoHealed, g};
        }
    }

    // Tier 2: cannot resolve confidently.
    return {RoleResolutionStatus::Unresolved, {}};
}

std::string resolve_role_from_config(HardwareRoleId id, Config* config,
                                     const std::vector<std::string>& discovered,
                                     bool persist_autoheal) {
    const HardwareRoleDescriptor* desc = role_descriptor(id);
    if (!desc || !config) {
        return {};
    }
    const std::string key = config->df() + desc->config_key;
    const std::string default_val =
        desc->canonical_default ? std::string(desc->canonical_default) : std::string();
    std::string saved = config->get<std::string>(key, default_val);

    // Empty saved means the role is intentionally unconfigured — do not invent one.
    if (saved.empty()) {
        return {};
    }

    RoleResolution res = resolve_role(*desc, saved, discovered);
    if (res.status == RoleResolutionStatus::AutoHealed) {
        spdlog::info("[HardwareRole] Auto-healed '{}' from '{}' to '{}' after hardware change",
                     desc->config_key, saved, res.object);
        if (persist_autoheal) {
            config->set<std::string>(key, res.object);
            if (!config->save()) {
                spdlog::warn("[HardwareRole] Failed to persist auto-heal for '{}'",
                             desc->config_key);
            }
        }
    } else if (res.status == RoleResolutionStatus::Unresolved) {
        // Unresolved is an expected state handled by the targeted reconfig wizard, and
        // fires every discovery for bed-less printers via the heater pre-heal. Keep it
        // at debug so it isn't noise.
        spdlog::debug("[HardwareRole] Role '{}' (saved '{}') has no live match — unresolved",
                      desc->config_key, saved);
    }
    return res.object;
}

std::vector<helix::wizard::StepId> unresolved_guided_steps(Config* config,
                                                           const PrinterDiscovery& hw) {
    std::vector<helix::wizard::StepId> out;
    if (!config) {
        return out;
    }
    for (const auto& desc : hardware_role_registry()) {
        if (!desc.guided) {
            continue;
        }
        const std::vector<std::string>* discovered =
            desc.category == HardwareCategory::Fan      ? &hw.fans()
            : desc.category == HardwareCategory::Heater ? &hw.heaters()
                                                        : nullptr;
        if (!discovered) {
            continue;
        }
        const std::string key = config->df() + desc.config_key;
        // Read with an EMPTY default (NOT the canonical default): an absent config
        // key means this role is not configured for THIS printer (e.g. a bed-less
        // printer has no heaters/bed key, a part-fan-less printer has no fans/part
        // key). Absent must NOT be treated as a problem to reconfigure.
        const std::string saved = config->get<std::string>(key, "");
        if (saved.empty()) {
            continue; // role not configured for this printer → nothing to reconfigure
        }
        if (resolve_role(desc, saved, *discovered).status == RoleResolutionStatus::Unresolved) {
            if (std::find(out.begin(), out.end(), desc.wizard_step) == out.end()) {
                out.push_back(desc.wizard_step);
            }
        }
    }
    return out;
}

bool decline_unresolved_guided_roles(Config* config, const PrinterDiscovery& hw) {
    if (!config) {
        return false;
    }
    bool changed = false;
    for (const auto& desc : hardware_role_registry()) {
        if (!desc.guided) {
            continue;
        }
        const std::vector<std::string>* discovered =
            desc.category == HardwareCategory::Fan      ? &hw.fans()
            : desc.category == HardwareCategory::Heater ? &hw.heaters()
                                                        : nullptr;
        if (!discovered) {
            continue;
        }
        const std::string key = config->df() + desc.config_key;
        // Only a present (non-empty) saved key can be "declined". An absent key is
        // already unconfigured — leave it alone (it isn't re-nagged after C1a).
        const std::string saved = config->get<std::string>(key, "");
        if (saved.empty()) {
            continue;
        }
        if (resolve_role(desc, saved, *discovered).status == RoleResolutionStatus::Unresolved) {
            // No confident substitute exists against current hardware. Record the
            // user's cancel as "declined/unconfigured" ("") so this role is skipped
            // on the next connection instead of re-launching the wizard forever.
            config->set<std::string>(key, std::string(""));
            changed = true;
        }
    }
    if (changed && !config->save()) {
        spdlog::warn("[HardwareRole] Failed to persist declined role(s)");
    }
    return changed;
}

} // namespace helix
