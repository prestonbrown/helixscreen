// src/printer/chamber_heater_backend_dragonbreath.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// VENDOR_OK: DragonBreath-firmware knowledge lives here and nowhere else.
// Status schema verified live against the U1 rig 2026-08-19 (issue #1290).
#include "chamber_heater_backend.h"

#include <spdlog/spdlog.h>

#include <cctype>

#include "hv/json.hpp"

namespace helix::chamber {
namespace {

/// Vendor fault code -> generic classification (substring heuristics over the
/// lowercased code). This file owns the mapping; generic code never sees a
/// vendor string. The raw code stays in fault_reason for logs.
FaultReason classify_fault_reason(const std::string& raw) {
    if (raw.empty()) {
        return FaultReason::None;
    }
    std::string lower;
    lower.reserve(raw.size());
    for (char c : raw) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    auto has = [&lower](const char* needle) { return lower.find(needle) != std::string::npos; };
    if (has("overtemp") || has("overheat")) {
        return FaultReason::Overtemp;
    }
    if (has("sensor") || has("short") || has("open")) {
        return FaultReason::SensorFault;
    }
    if (has("comms") || has("timeout") || has("watchdog") || has("disconnect")) {
        return FaultReason::CommsLoss;
    }
    return FaultReason::Other;
}

/// Vendor fan reason -> generic driver. The vocabulary is closed (off /
/// requested / heater / thermal_purge); any OTHER non-empty value is still
/// the device acting on its own, so it maps to Device — a reason we cannot
/// classify is never "we control it". Absent/null stays Unknown: no report
/// is not a report that the fan is stopped.
FilterFanDriver classify_filter_fan_driver(const std::string& raw) {
    if (raw.empty()) {
        return FilterFanDriver::Unknown;
    }
    if (raw == "off") {
        return FilterFanDriver::Off;
    }
    if (raw == "requested") {
        return FilterFanDriver::Requested;
    }
    return FilterFanDriver::Device;
}

/// Vendor rejection code -> generic kind and, for a refusal, its cause, by the
/// words the code is built from. The glue's stable codes (countermanded,
/// fault_latched, inhibited, auth_failed, conflict_retries_exhausted,
/// invalid_command, missing_lease_id, lease_lost, heater_busy) and any raw
/// firmware code it passes through all land this way. A code we cannot place
/// is still the heater declining: Refused.
struct ClassifiedRejection {
    CommandRejection kind = CommandRejection::None;
    RefusalCause cause = RefusalCause::Unspecified;
};

ClassifiedRejection classify_rejection(const std::string& raw) {
    if (raw.empty()) {
        return {};
    }
    std::string lower;
    lower.reserve(raw.size());
    for (char c : raw) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    auto has = [&lower](const char* needle) { return lower.find(needle) != std::string::npos; };
    if (has("conflict") || has("revision") || has("stale")) {
        return {CommandRejection::Conflict, RefusalCause::Unspecified};
    }
    if (has("auth") || has("token") || has("forbidden") || has("password") || has("credential")) {
        return {CommandRejection::Auth, RefusalCause::Unspecified};
    }
    if (has("inhibit") || has("cooldown")) {
        return {CommandRejection::Refused, RefusalCause::Inhibited};
    }
    if (has("fault")) {
        return {CommandRejection::Refused, RefusalCause::Fault};
    }
    // A device that did not answer is not one that was switched off.
    if (has("offline") || has("unreachable") || has("timeout")) {
        return {CommandRejection::Refused, RefusalCause::Unspecified};
    }
    if (has("countermand") || has("off") || has("disabled") || has("manual") || has("standby")) {
        return {CommandRejection::Refused, RefusalCause::TurnedOff};
    }
    return {CommandRejection::Refused, RefusalCause::Unspecified};
}

class DragonbreathBackend : public ChamberHeaterBackend {
  public:
    std::string_view id() const override {
        return "dragonbreath";
    }

    int discovery_confidence(const std::string& object_name) const override {
        std::string lower;
        lower.reserve(object_name.size());
        for (char c : object_name) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return lower.find("dragonbreath") != std::string::npos ? 95 : 0;
    }

    std::string_view diagnostics_object() const override {
        return "dragonbreath";
    }
    std::string_view filter_fan_pin() const override {
        return "output_pin dragonbreath_filter";
    }
    std::string_view fault_reset_gcode() const override {
        return "DRAGONBREATH_RESET";
    }
    // The glue's dragonbreath/set_target webhook, which Moonraker proxies as a
    // printer endpoint. It runs outside the gcode mutex, so it reaches the
    // heater while a TEMPERATURE_WAIT on it holds the queue.
    std::string_view set_target_method() const override {
        return "printer.dragonbreath.set_target";
    }
    // ptc_temp rides in every status frame.
    bool reports_element_temp() const override {
        return true;
    }
    // Firmware hard-caps the target at 70 C; configfile usually says 75. If we
    // ever have NO ceiling data, assume the stock chamber cap.
    double conservative_max_temp() const override {
        return 60.0;
    }
    // Lease semantics arbitrate: the device invalidates our lease when another
    // controller takes over and we mirror authoritative state.
    bool device_autonomous_control() const override {
        return false;
    }

    std::optional<ChamberHeaterDiagnostics>
    parse_diagnostics(const nlohmann::json& status) const override {
        if (!status.is_object()) {
            return std::nullopt; // not a dragonbreath diagnostics frame
        }
        // A delta frame may carry any subset of the schema, so recognition
        // cannot hinge on one field (ptc_temp alone drops every delta that
        // mentions only the fan). Any dragonbreath-specific key marks the
        // frame ours; generic heater keys (temperature/target) never do.
        static const char* const kDbFields[] = {"ptc_temp",
                                                "fault",
                                                "inhibited",
                                                "fault_reason",
                                                "fan_percent",
                                                "fan_reason",
                                                "mode",
                                                "source",
                                                "lease_owned",
                                                "connected",
                                                "protocol_error",
                                                "protocol_error_code",
                                                "protocol_error_seq",
                                                "protocol_error_target",
                                                "protocol_error_message",
                                                "wait_active",
                                                "wait_target"};
        bool known_field = false;
        for (const char* f : kDbFields) {
            if (status.contains(f)) {
                known_field = true;
                break;
            }
        }
        if (!known_field) {
            return std::nullopt; // not a dragonbreath diagnostics frame
        }
        ChamberHeaterDiagnostics d;
        // Key present -> engaged; null or a wrong type coerces to the type's
        // unknown value. Key absent -> nullopt: the frame made no report.
        if (status.contains("fault")) {
            d.fault = status["fault"].is_boolean() ? status["fault"].get<bool>() : false;
        }
        if (status.contains("inhibited")) {
            d.inhibited =
                status["inhibited"].is_boolean() ? status["inhibited"].get<bool>() : false;
        }
        if (status.contains("fault_reason")) {
            d.fault_reason = status["fault_reason"].is_string()
                                 ? status["fault_reason"].get<std::string>()
                                 : std::string();
            d.fault_reason_kind = classify_fault_reason(*d.fault_reason);
        }
        if (status.contains("ptc_temp")) {
            d.element_temp_c =
                status["ptc_temp"].is_number() ? status["ptc_temp"].get<double>() : NAN;
        }
        if (status.contains("fan_percent")) {
            d.filter_fan_percent =
                status["fan_percent"].is_number() ? status["fan_percent"].get<int>() : -1;
        }
        if (status.contains("fan_reason")) {
            d.filter_fan_reason = status["fan_reason"].is_string()
                                      ? status["fan_reason"].get<std::string>()
                                      : std::string();
            d.filter_fan_driver = classify_filter_fan_driver(*d.filter_fan_reason);
        }
        // The appliance is a mains-powered radio device: it can vanish while
        // Klipper keeps answering for the heater section it owns. Absent means
        // the frame said nothing, which is not a report that the link is up.
        // A value we cannot read is not evidence the device is unreachable, so
        // a malformed slot engages as connected: every coercion here fails
        // toward not raising an alarm, as the fault slot does.
        if (status.contains("connected")) {
            d.device_connected =
                status["connected"].is_boolean() ? status["connected"].get<bool>() : true;
        }
        // Transport-level complaint from the glue. Logs only; the UI speaks
        // through chamber_heater_offline.
        if (status.contains("protocol_error")) {
            d.link_error = status["protocol_error"].is_string()
                               ? status["protocol_error"].get<std::string>()
                               : std::string();
        }
        // The heater's answer to our last target. The glue holds a rejection
        // until the next command intent and clears it to "". The code is the
        // machine-readable reason; a glue too old to send one reports only a
        // free-text protocol_error, which stays a link complaint, except that
        // its clearing to "" is still a report that nothing is outstanding.
        if (status.contains("protocol_error_code")) {
            const auto& code = status["protocol_error_code"];
            d.rejection_code = code.is_string() ? code.get<std::string>() : std::string();
            const ClassifiedRejection c = classify_rejection(*d.rejection_code);
            d.rejection = c.kind;
            d.refusal_cause = c.cause;
        } else if (status.contains("protocol_error") && d.link_error && d.link_error->empty()) {
            d.rejection = CommandRejection::None;
            d.refusal_cause = RefusalCause::Unspecified;
        }
        if (status.contains("protocol_error_seq")) {
            const auto& seq = status["protocol_error_seq"];
            d.rejection_seq = seq.is_number_integer() ? seq.get<int>() : 0;
        }
        if (status.contains("protocol_error_target")) {
            const auto& t = status["protocol_error_target"];
            d.rejection_target_c = t.is_number() ? t.get<double>() : NAN;
        }
        // The glue's M191 wait: set when it starts, following a newer target,
        // cleared on every exit.
        if (status.contains("wait_active")) {
            d.wait_active =
                status["wait_active"].is_boolean() ? status["wait_active"].get<bool>() : false;
        }
        if (status.contains("wait_target")) {
            d.wait_target_c =
                status["wait_target"].is_number() ? status["wait_target"].get<double>() : NAN;
        }
        // Externally driven: heater active, but neither klipper source nor our
        // lease. Computed from three inputs, so it is an answer only when the
        // frame carries all of them — a partial trio is not a confident false.
        // Fields may arrive as null (value() throws on null, not just on missing).
        if (status.contains("mode") && status.contains("source") &&
            status.contains("lease_owned")) {
            std::string mode;
            if (status["mode"].is_string()) {
                mode = status["mode"].get<std::string>();
            }
            std::string source;
            if (status["source"].is_string()) {
                source = status["source"].get<std::string>();
            }
            bool lease_owned = false;
            if (status["lease_owned"].is_boolean()) {
                lease_owned = status["lease_owned"].get<bool>();
            }
            bool heating = mode == "power_on";
            bool ours = lease_owned || source == "klipper";
            d.externally_controlled = heating && !ours;
        }
        return d;
    }
};

const DragonbreathBackend kDragonbreath;

} // namespace

// registry hookup lives in chamber_heater_backend_generic.cpp; expose via friend
// free function so this file owns its instance.
const ChamberHeaterBackend* dragonbreath_backend_instance() {
    return &kDragonbreath;
}

} // namespace helix::chamber
