// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chamber_heater_assignment.h"

#include "chamber_heater_backend.h"
#include "printer_capabilities_state.h"
#include "printer_discovery.h"
#include "printer_temperature_state.h"
#include "temperature_controller.h"
#include "temperature_sensor_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::chamber {

namespace {

/// One chamber role's assignment against discovery's pick for that role. Blind to
/// the object's type: whatever discovery picked for the role is the fallback.
std::string resolve(const std::string& assignment, const std::string& discovered,
                    const PrinterDiscovery& discovery, const char* role) {
    if (assignment == "auto") {
        return discovered;
    }
    if (assignment == "none") {
        return "";
    }

    const auto& reported = discovery.printer_objects();
    if (std::find(reported.begin(), reported.end(), assignment) != reported.end()) {
        return assignment;
    }

    // Every discovery pass (each klippy ready and reconnect) lands here while the saved
    // name stays stale, so it is not news each time.
    spdlog::debug("[ChamberAssignment] Assigned chamber {} '{}' is not a Klipper object on this "
                  "printer; using the discovered one ('{}')",
                  role, assignment, discovered);
    return discovered;
}

} // namespace

std::string resolve_heater(const std::string& assignment, const PrinterDiscovery& discovery) {
    return resolve(assignment, discovery.chamber_heater_name(), discovery, "heater");
}

std::string resolve_sensor(const std::string& assignment, const PrinterDiscovery& discovery) {
    return resolve(assignment, discovery.chamber_sensor_name(), discovery, "sensor");
}

void apply_resolution(const PrinterDiscovery& discovery, const std::string& sensor_assignment,
                      const std::string& heater_assignment, PrinterTemperatureState& temps,
                      PrinterCapabilitiesState& caps, TemperatureController* tc) {
    const std::string chamber_sensor = resolve_sensor(sensor_assignment, discovery);
    const std::string chamber_heater = resolve_heater(heater_assignment, discovery);

    spdlog::debug("[Chamber] Resolved: sensor='{}' heater='{}'", chamber_sensor, chamber_heater);
    temps.set_chamber_sensor_name(chamber_sensor);
    temps.set_chamber_heater_name(chamber_heater);
    // Cooling-fan name has no manual override — it's read straight from discovery.
    // In COOLING mode the K2 M141 macro parks the setpoint on this fan's target.
    temps.set_chamber_cooling_fan_name(discovery.chamber_cooling_fan_name());
    // Cooling fan's configured resting/off target (from configfile.settings). M141
    // S0 returns the fan here, so the chamber mode treats this value as Off rather
    // than a deliberate "Maintaining" set.
    temps.set_chamber_fan_resting(discovery.chamber_fan_resting_deci());

    // Chamber-heater diagnostics backend (issue #1290). The backend matched the
    // DISCOVERED chamber heater during parse_objects; its diagnostics surfaces
    // only apply while the RESOLVED heater is that same discovery pick — a
    // manual override to another heater (or "none") detaches them and clears
    // the capabilities. See include/chamber_heater_backend.h.
    const bool chamber_diagnostics_apply =
        !chamber_heater.empty() && chamber_heater == discovery.chamber_heater_name();
    if (chamber_diagnostics_apply) {
        temps.set_chamber_diagnostics_source(discovery.chamber_heater_backend_id(),
                                             discovery.chamber_diagnostics_object(),
                                             discovery.chamber_filter_fan_pin());
    } else {
        temps.set_chamber_diagnostics_source("", "", "");
    }

    // Backend action surface (issue #1290): fault-reset gcode, filter-fan pin
    // and the conservative ceiling come from the matched backend — same gate
    // as the diagnostics source above, so a manual override to another heater
    // (or "none") clears them and the actions revert to no-ops.
    if (tc) {
        if (chamber_diagnostics_apply) {
            const auto* backend = backend_by_id(discovery.chamber_heater_backend_id());
            tc->set_chamber_actions(backend ? std::string(backend->fault_reset_gcode())
                                            : std::string(),
                                    discovery.chamber_filter_fan_pin(),
                                    backend ? backend->conservative_max_temp() : 0.0);
            tc->set_chamber_dryer(backend, discovery.has_heater_bed());
            // Read the ceiling now, after set_chamber_actions() stored the
            // backend's fallback, so a label built from it on first open is right.
            tc->ensure_limits(helix::HeaterType::Chamber);
        } else {
            tc->set_chamber_actions(std::string(), std::string(), 0.0);
            tc->set_chamber_dryer(nullptr);
        }
    }

    // Capability flags follow the resolved assignments: discovery's own flags
    // miss manual overrides.
    caps.set_has_chamber_sensor(!chamber_sensor.empty());
    caps.set_has_chamber_heater(!chamber_heater.empty());
    caps.set_has_chamber_heater_diagnostics(chamber_diagnostics_apply &&
                                            !discovery.chamber_diagnostics_object().empty());
    caps.set_has_chamber_filter_fan(chamber_diagnostics_apply &&
                                    !discovery.chamber_filter_fan_pin().empty());
    const auto* chamber_backend =
        chamber_diagnostics_apply ? backend_by_id(discovery.chamber_heater_backend_id()) : nullptr;
    caps.set_has_chamber_element_temp(chamber_backend && chamber_backend->reports_element_temp());
    caps.set_has_chamber_dryer(chamber_backend && chamber_backend->dryer_capabilities().supported);

    // Promote the resolved chamber sensor and heater to CHAMBER role in the
    // sensor manager. Required for vendors whose chamber object names don't
    // match the "chamber" substring used by the manager's auto-categorizer
    // (Snapmaker uses "cavity", Elegoo "enclosure"). Without this promotion,
    // the temp graph would add the chamber twice — once as "Chamber" (from
    // PrinterTemperatureState) and once under its raw display name (because
    // the AUXILIARY role isn't filtered out).
    helix::sensors::TemperatureSensorManager::instance().apply_chamber_sensor_override(
        chamber_sensor, chamber_heater);
}

} // namespace helix::chamber
