// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wizard_step.h" // helix::wizard::StepId

#include <string>
#include <vector>

namespace helix {

class Config;
class PrinterDiscovery;

enum class HardwareRoleId {
    HotendHeater,
    BedHeater,
    PartFan,
    HotendFan,
    ChamberFan,
    ExhaustFan,
    AuxFan,
};

enum class HardwareCategory { Fan, Heater, Led, FilamentSensor };

enum class RoleResolutionStatus {
    Resolved,   // saved value is live and valid -- keep it
    AutoHealed, // saved value invalid; a confident replacement was chosen
    Unresolved, // no confident replacement -- needs guided reconfig
};

struct RoleResolution {
    RoleResolutionStatus status;
    std::string object; // resolved object name; empty when Unresolved
};

/// One row of authoritative knowledge about a configurable hardware role.
/// is_candidate / guess may be nullptr (then: membership in `discovered` is the
/// only candidacy test, and no heuristic fallback is attempted, respectively).
struct HardwareRoleDescriptor {
    HardwareRoleId id;
    const char* config_key;        // relative to Config::df(), e.g. "fans/part"
    const char* canonical_default; // e.g. "fan", "heater_bed", "extruder", ""
    HardwareCategory category;
    helix::wizard::StepId wizard_step;
    bool guided;
    bool (*is_candidate)(const std::string& obj);
    std::string (*guess)(const std::vector<std::string>& objs);
};

/// The single source of truth. Stable order; safe to iterate.
const std::vector<HardwareRoleDescriptor>& hardware_role_registry();

/// Look up a descriptor by id. Returns nullptr if absent.
const HardwareRoleDescriptor* role_descriptor(HardwareRoleId id);

/// Pure tiered resolution -- no config, no persistence, fully unit-testable.
RoleResolution resolve_role(const HardwareRoleDescriptor& desc, const std::string& saved_value,
                            const std::vector<std::string>& discovered);

/// Read the saved role from config (default = canonical_default), resolve it
/// against `discovered`, optionally persist an AutoHealed value back to config.
/// Returns the resolved object name (empty if the role is unconfigured or
/// Unresolved). An empty saved value is treated as "unconfigured": returns ""
/// without inventing a role via heuristics.
std::string resolve_role_from_config(HardwareRoleId id, Config* config,
                                     const std::vector<std::string>& discovered,
                                     bool persist_autoheal);

/// Collect the deduped, registry-ordered set of wizard steps whose guided
/// hardware roles cannot be confidently resolved against discovered hardware.
///
/// For each guided registry descriptor: the discovered list is chosen by category
/// (Fan -> hw.fans(), Heater -> hw.heaters(); other categories are skipped). The
/// saved role is read from config (default = canonical_default). An empty saved
/// value is treated as unconfigured and skipped. When resolve_role() returns
/// Unresolved, the descriptor's wizard_step is added (once). Returns empty when
/// config is null or every guided role resolves.
std::vector<helix::wizard::StepId> unresolved_guided_steps(Config* config,
                                                           const PrinterDiscovery& hw);

/// Mark every guided role that is STILL Unresolved against `hw` AND has a present
/// (non-empty) saved key as declined by writing "" to its config key, then save()
/// once if anything changed. Mirrors unresolved_guided_steps() but records the
/// user's "cancel" so an unsatisfiable role (no candidate hardware exists) is not
/// re-offered on every reconnect. Absent keys and resolvable roles are left
/// untouched. Returns true if any key was changed.
bool decline_unresolved_guided_roles(Config* config, const PrinterDiscovery& hw);

/// End of a targeted reconfig session, shared by its Finish and Cancel paths:
/// decline every guided role still Unresolved against `hw`. A session step can
/// only resolve the roles it has a control for — the fan step has no aux
/// dropdown — so a preset-saved role with no live match (e.g. fans/aux naming
/// a fan the printer does not have) can never be satisfied inside the session;
/// skipping this on Finish relaunches the wizard on every boot. Returns true
/// if any key was changed.
bool settle_targeted_reconfig(Config* config, const PrinterDiscovery& hw);

} // namespace helix
