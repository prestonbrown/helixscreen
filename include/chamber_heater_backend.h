// include/chamber_heater_backend.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_drying_types.h"
#include "json_fwd.h"

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::chamber {

/// Generic fault classification. Vendor codes map to one of these at the
/// backend border; the raw vendor string is kept for logging only and never
/// reaches the UI.
enum class FaultReason { None, Overtemp, SensorFault, CommsLoss, Other };

/// Who is driving the filtration fan right now. The filter-fan output_pin is
/// a REQUEST; a device may also run the fan on its own (heater warmup,
/// residual-heat purge) and report why. Vendor reason strings map to one of
/// these at the backend border; the raw string is kept for logging only.
enum class FilterFanDriver { Unknown, Off, Requested, Device };

/// Why the heater turned down a target we asked for. Vendor reason codes map
/// to one of these at the backend border; the raw code is kept for logs only.
/// Conflict: another controller changed the heater between our read and our
/// write. Refused: the heater itself declined (see RefusalCause). Auth: it did
/// not accept the printer's credentials.
enum class CommandRejection { None, Conflict, Refused, Auth };

/// What a Refused rejection is about, for the words the user reads.
enum class RefusalCause { Unspecified, TurnedOff, Fault, Inhibited };

/// Generic chamber-heater diagnostics — the ONLY shape subjects/UI ever see.
/// Vendor JSON schemas are translated to this at the backend border.
///
/// Field-level delta semantics: Moonraker status frames carry only changed
/// fields, so an unengaged optional means "this frame did not mention the
/// field" and the consumer must keep the last known value. An ENGAGED optional
/// is the device answering — it may carry an unknown value (empty string,
/// negative percent, NAN temp), which is a report, not an absence.
struct ChamberHeaterDiagnostics {
    std::optional<bool> fault;
    std::optional<bool> inhibited;
    /// Another controller (device web UI, physical button) is currently driving
    /// the heater, not us. Display-only annotation in v1.
    std::optional<bool> externally_controlled;
    /// Raw vendor fault code, empty when none. Logs only — the UI binds the
    /// translated chamber_heater_fault_reason_text derived from fault_reason_kind.
    std::optional<std::string> fault_reason;
    std::optional<FaultReason> fault_reason_kind; ///< classified kind for UI
    /// Device reachable on its own radio link. Engaged false = the device
    /// itself reports unreachable. Unengaged = this backend reports no link
    /// state, which is unknown, NOT offline.
    std::optional<bool> device_connected;
    /// Raw vendor protocol/link error, empty when none. Logs only — the UI
    /// surface is chamber_heater_offline, derived from device_connected.
    std::optional<std::string> link_error;
    std::optional<double> element_temp_c;             ///< heating-element temp; NAN = unknown
    std::optional<int> filter_fan_percent;            ///< negative = unknown
    std::optional<std::string> filter_fan_reason;     ///< raw vendor reason, logs only
    std::optional<FilterFanDriver> filter_fan_driver; ///< classified driver for UI
    /// A filament-drying cycle is running on the device.
    std::optional<bool> drying_active;
    /// Seconds left in the drying cycle; 0 before the device reports a countdown.
    std::optional<int> drying_remaining_s;
    std::optional<int> drying_target_c;     ///< the cycle's target temperature
    std::optional<int> drying_duration_min; ///< the cycle's full length
    /// The heater's answer to the last target we sent. Engaged None is the
    /// device reporting no rejection outstanding. A backend that does not
    /// report rejections never engages these.
    std::optional<CommandRejection> rejection;
    std::optional<RefusalCause> refusal_cause;
    /// Increments once per rejection, so a repeat of the same reason is
    /// still a new event.
    std::optional<int> rejection_seq;
    /// Target the rejected command asked for, °C; NAN = not reported.
    std::optional<double> rejection_target_c;
    std::optional<std::string> rejection_code; ///< raw vendor code, logs only
    /// The device's own wait on the chamber (an M191 its glue runs) is
    /// holding the queue. A backend that reports none never engages these.
    std::optional<bool> wait_active;
    std::optional<double> wait_target_c; ///< the wait's temperature; NAN = not reported
};

/// One chamber-heater style/brand behind an interface (AMS-backend pattern).
/// Adding a brand = new subclass file + one registry() line. Vendor names live
/// ONLY in chamber_heater_backend_*.cpp.
class ChamberHeaterBackend {
  public:
    virtual ~ChamberHeaterBackend() = default;
    /// Stable backend id (ASCII, logged, never UI-visible). Concrete ids live in the backend .cpp
    /// files.
    virtual std::string_view id() const = 0;
    /// 0 = not mine. Heuristic keyword score for generic; 95 for appliance names.
    virtual int discovery_confidence(const std::string& object_name) const = 0;
    /// Status object carrying diagnostics ("" = this backend has none).
    virtual std::string_view diagnostics_object() const = 0;
    /// Binary filtration-fan output_pin ("" = none).
    virtual std::string_view filter_fan_pin() const = 0;
    /// Gcode clearing a latched fault ("" = none).
    virtual std::string_view fault_reset_gcode() const = 0;
    /// Moonraker JSON-RPC method that sets the heater target outside the gcode
    /// queue, taking {"target": °C} ("" = none). A gcode set waits behind
    /// whatever holds the queue, including a TEMPERATURE_WAIT on this very
    /// heater; this route does not.
    virtual std::string_view set_target_method() const {
        return {};
    }
    /// Backend reports the heating element's own temperature. False hides the
    /// element readout: a permanently blank row tells the user nothing, and
    /// this is a property of the schema, not of any one frame.
    virtual bool reports_element_temp() const = 0;
    /// Conservative °C ceiling when configfile max_temp is unknown. 0 = no clamp.
    virtual double conservative_max_temp() const = 0;
    /// Device may self-drive the heater (stock-firmware Auto mode).
    virtual bool device_autonomous_control() const = 0;
    /// What this backend's filament-drying cycle accepts. supported=false (the
    /// default) = no dryer; the cycle's live state arrives through
    /// parse_diagnostics() like every other surface.
    virtual DryerInfo dryer_capabilities() const {
        return {};
    }
    /// Gcode starting a drying cycle ("" = no dryer). Takes a request already
    /// clamped by dryer_capabilities(); the backend still bounds what it sends.
    virtual std::string dryer_start_gcode(float /*temp_c*/, int /*duration_min*/) const {
        return {};
    }
    /// Gcode ending a drying cycle ("" = no dryer).
    virtual std::string_view dryer_stop_gcode() const {
        return {};
    }
    /// Parse this backend's status JSON. nullopt = payload is not mine/unusable.
    virtual std::optional<ChamberHeaterDiagnostics>
    parse_diagnostics(const nlohmann::json& status) const = 0;
};

struct MatchResult {
    const ChamberHeaterBackend* backend = nullptr;
    int confidence = 0;
};

/// Fixed-priority registry. match() picks the highest confidence; ties resolve by registry order
/// (first wins).
const std::vector<const ChamberHeaterBackend*>& registry();

/// Best backend for an object name, or nullptr.
MatchResult match(const std::string& object_name);

/// Keyword-only confidence for a chamber/enclosure/cavity/box object name:
/// CHAMBER 100 > ENCLOSURE 90 > CAVITY 85 > standalone-token BOX 60, minus 1
/// when the keyword is compound, minus 40 for an air-quality token
/// (TVOC/VOC/CO2/GAS/HUMIDITY/IAQ/AQI/PM25/PM10/PARTICULATE/PRESSURE). 0 = no
/// chamber keyword. Sensor and cooling-fan discovery score names with this
/// directly — appliance backends score their names only in match(), so an
/// appliance heater never claims the sensor or fan slot.
int keyword_confidence(const std::string& object_name);

/// Lookup by id (nullptr if unknown).
const ChamberHeaterBackend* backend_by_id(std::string_view id);

/// Objects to subscribe for the matched backend's surfaces (diagnostics +
/// filter pin). Inputs come from PrinterDiscovery; empty vector = nothing.
inline std::vector<std::string> required_status_objects(std::string_view diagnostics_object,
                                                        std::string_view filter_fan_pin) {
    std::vector<std::string> out;
    if (!diagnostics_object.empty())
        out.emplace_back(diagnostics_object);
    if (!filter_fan_pin.empty())
        out.emplace_back(filter_fan_pin);
    return out;
}

} // namespace helix::chamber
