// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Chamber air circulation modes.
//
// Some enclosed printers route chamber air one of two ways with a set of fans
// the firmware drives together: recirculating it inside the enclosure (heat
// retention for ABS/ASA-class materials) or exhausting it out of the machine
// (cooling for PLA/TPU), plus an off state. The firmware owns which fans run at
// which speed for each mode; HelixScreen names the mode, sends it, and shows
// what the firmware reports back.
//
// This module is the ONLY place that knows which printers have such modes and
// what each one sends and reports. The fan screen, the settings row, the
// maintenance meters and the notices ask these questions and never name the
// firmware. Adding a firmware means adding one Provider to the table in
// chamber_air_modes.cpp - no call site changes.
//
// Every provider capability beyond "send a mode" is optional:
//  - presence: a detachable accessory that the firmware reports fitted or not.
//    While it is absent nothing may be offered, because the firmware drops the
//    writes. A provider without it is always present.
//  - mode readback: a status field naming the active mode. Without one a mode
//    is an action (a macro that sets several fans once, after which the fans
//    stay individually controllable, so their speeds cannot be read back as a
//    mode) and the UI must not highlight a selection the printer may not hold.
//  - targets: a temperature and fan speeds a mode command carries.
//  - owned fans: fans the firmware keeps inside its own status object rather
//    than as Klipper fan objects, with their own speed command.
//  - a filter counter: how long the filter fan has run, and how to reset it.
//
// Every status reading is optional too: a field missing from a frame (a delta
// frame where it did not change, or an older firmware that never reports it)
// says nothing, and the caller keeps its last answer.

#include "json_fwd.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix {
class PrinterDiscovery;
}

namespace helix::chamber_air {

/// The fixed set of modes a printer may offer.
enum class AirMode {
    Recirculate, ///< Keep chamber air inside the enclosure
    Exhaust,     ///< Vent chamber air out of the machine
    Off,         ///< Every chamber circulation fan off
};

/// What a mode command carries, for providers that take targets. A provider
/// ignores the fields its modes do not use.
struct ModeTargets {
    double target_c = 42.0;   ///< Exhaust: hold the chamber at or below this
    int exhaust_pct = 60;     ///< Exhaust fan speed while exhausting
    int recirculate_pct = 60; ///< Recirculation fan speed while recirculating
};

/// The job of a fan a provider owns.
enum class FanRole {
    Exhaust,     ///< Vents chamber air out of the machine
    Recirculate, ///< Moves chamber air through the filter, inside the enclosure
};

/// A fan the provider owns.
struct OwnedFan {
    FanRole role;
    /// Stable id for UI user_data and logs; never a Klipper object name.
    const char* id;
    /// A Klipper fan object that IS this fan when the printer has one (a firmware
    /// that republishes it as an ordinary fan), or nullptr. When it is present the
    /// UI shows that object's own dial as this fan instead of adding one.
    const char* equivalent_object;
};

/// One status frame's news. Every field nullopt means the frame said nothing.
struct Reading {
    std::optional<bool> present;
    std::optional<AirMode> mode;
    std::optional<double> exhaust_speed;     ///< 0..1
    std::optional<double> recirculate_speed; ///< 0..1
    /// Below these the firmware's own screen asks before lowering a fan; 0..1.
    std::optional<double> exhaust_threshold;
    std::optional<double> recirculate_threshold;
    std::optional<double> critical_temp_c;
    std::optional<double> filter_seconds;
};

/// What the fan screen does with a requested speed (guard_fan_change).
enum class FanGuardAction {
    Apply,   ///< send FanGuard::pct
    Confirm, ///< ask first, then send FanGuard::pct
    Blocked, ///< the active mode keeps this fan off; send nothing
};

struct FanGuard {
    FanGuardAction action;
    int pct;
};

/// Whether this printer offers any chamber air mode.
bool supported(const PrinterDiscovery& hw);

/// The modes this printer offers, in AirMode order. Empty when unsupported.
std::vector<AirMode> modes(const PrinterDiscovery& hw);

/// Whether this printer offers @p mode.
bool offers(const PrinterDiscovery& hw, AirMode mode);

/// Gcode that switches the chamber to @p mode with @p targets, or an empty
/// string when the printer does not offer it.
std::string mode_gcode(const PrinterDiscovery& hw, AirMode mode,
                       const ModeTargets& targets = ModeTargets{});

/// True when presence is a live reading (a detachable accessory); false when
/// the modes are always available.
bool presence_is_live(const PrinterDiscovery& hw);

/// True when the firmware reports the active mode.
bool reports_mode(const PrinterDiscovery& hw);

/// True when this printer's mode commands carry ModeTargets.
bool takes_targets(const PrinterDiscovery& hw);

/// The fans this printer's provider owns. Empty for most printers.
std::vector<OwnedFan> owned_fans(const PrinterDiscovery& hw);

/// Gcode that sets an owned fan to @p pct (0-100), or "" when the printer has no
/// such fan.
std::string fan_speed_gcode(const PrinterDiscovery& hw, FanRole role, int pct);

/// Gcode that resets the firmware's filter counter, or "" when it keeps none.
std::string filter_reset_gcode(const PrinterDiscovery& hw);

/// The lowest speed, in percent, the fan a mode drives may be set to while the
/// mode runs; 0 when the provider has no such floor.
int guard_floor_pct(const PrinterDiscovery& hw);

/// Status objects to subscribe, as a Moonraker `objects` map (object name to the
/// list of fields read). Empty when the provider reads nothing.
nlohmann::json required_status_objects(const PrinterDiscovery& hw);

/// What a Moonraker status frame says. All nullopt for a printer with no
/// provider, a frame without the provider's object, or fields of a type no
/// firmware sends.
Reading read_status(const PrinterDiscovery& hw, const nlohmann::json& status);

/// The owned fan @p mode runs, which the guard floor applies to. nullopt for Off.
std::optional<FanRole> driven_fan(AirMode mode);

/// The owned fan @p mode keeps off. nullopt for Off.
std::optional<FanRole> disabled_fan(AirMode mode);

/// What the fan screen does when the user asks @p role to go from @p current_pct
/// to @p requested_pct while @p mode runs (nullopt: mode unknown). With no
/// mode, or Off, the request applies as asked. The fan the mode keeps off
/// refuses any speed above 0. The fan the mode drives never goes below
/// @p floor_pct, and lowering it below @p threshold_pct asks first.
FanGuard guard_fan_change(std::optional<AirMode> mode, FanRole role, int requested_pct,
                          int current_pct, std::optional<int> threshold_pct, int floor_pct);

/// Stable identifier for @p mode ("recirculate", "exhaust", "off"), as XML
/// event user_data names it.
const char* mode_id(AirMode mode);

/// The mode an identifier names, or nullopt for anything else.
std::optional<AirMode> parse_mode_id(std::string_view id);

/// Human-readable name of the matched firmware, for logging. Empty when none.
std::string provider_name(const PrinterDiscovery& hw);

} // namespace helix::chamber_air
