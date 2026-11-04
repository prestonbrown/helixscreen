// src/printer/chamber_heater_backend_panda_breath.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// VENDOR_OK: stock Panda Breath firmware knowledge lives here and nowhere else.
// Status schema verified live against the U1 rig 2026-09-16 (issue #1290).
#include "chamber_heater_backend.h"
#include "core/text_io.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace helix::chamber {
namespace {

/// work_mode names the control loop holding the heater, mirrored from the
/// device's own field: 1 is the appliance's bed-driven auto cycle, 2 is the
/// target Klipper set, 3 is a filament-drying run. Only 2 is ours, and any
/// value the appliance grows later is something else driving the chamber.
constexpr int kWorkModeKlipperTarget = 2;

/// The binding refuses a drying cycle longer than this.
constexpr int kMaxDryHours = 12;

/// Keys the stock binding publishes that no other object does. `temperature`,
/// `target` and `smoothed_temp` are deliberately absent: every heater carries
/// those, and a backend that accepted them would claim foreign payloads.
constexpr const char* const kStockFields[] = {
    "connected",     "work_mode",      "work_on",           "device_target",
    "auto_enabled",  "auto_target",    "auto_filtertemp",   "auto_hotbedtemp",
    "filament_temp", "filament_timer", "remaining_seconds", "filament_drying_active"};

class PandaBreathBackend : public ChamberHeaterBackend {
  public:
    std::string_view id() const override {
        return "panda_breath";
    }

    int discovery_confidence(const std::string& object_name) const override {
        const std::string lower = helix::text_io::to_lower(object_name);
        if (lower.find("panda_breath") != std::string::npos ||
            lower.find("pandabreath") != std::string::npos) {
            return 95;
        }
        return 0;
    }

    std::string_view diagnostics_object() const override {
        return "panda_breath";
    }
    // The binding exposes no filtration pin: the appliance runs its filter from
    // its own auto settings and publishes no speed.
    std::string_view filter_fan_pin() const override {
        return {};
    }
    // No fault surface to clear — the schema carries no fault or inhibit field.
    std::string_view fault_reset_gcode() const override {
        return {};
    }
    // The binding publishes the chamber temperature only; the PTC element's
    // own temperature never leaves the appliance.
    bool reports_element_temp() const override {
        return false;
    }
    // Fallback only, for a configfile that declares no max_temp; a declared
    // ceiling always wins. 60 C is the chamber temperature the appliance is
    // sold for, and undershooting a ceiling is the safe direction.
    double conservative_max_temp() const override {
        return 60.0;
    }
    // Auto mode closes the chamber loop on the device, driven by bed temperature.
    bool device_autonomous_control() const override {
        return true;
    }

    // The binding's DRY_START takes whole hours between 1 and 12. The target
    // stays under the same 60 C the heater falls back to: the appliance is
    // sold as a chamber heater for that temperature.
    DryerInfo dryer_capabilities() const override {
        DryerInfo d;
        d.supported = true;
        d.allows_during_print = false; // the cycle takes the chamber from the print
        d.min_temp_c = 35.0f;
        d.max_temp_c = static_cast<float>(conservative_max_temp());
        d.max_duration_min = kMaxDryHours * 60;
        d.duration_step_min = 60;
        return d;
    }
    // Minutes round UP to whole hours so a preset never dries for less than it
    // names; the binding truncates a fractional HOURS, so only integers go out.
    std::string dryer_start_gcode(float temp_c, int duration_min) const override {
        const int hours = std::clamp((std::max(duration_min, 0) + 59) / 60, 1, kMaxDryHours);
        return "PANDA_BREATH_DRY_START TEMP=" + std::to_string(std::lround(temp_c)) +
               " HOURS=" + std::to_string(hours);
    }
    std::string_view dryer_stop_gcode() const override {
        return "PANDA_BREATH_DRY_STOP";
    }

    std::optional<ChamberHeaterDiagnostics>
    parse_diagnostics(const nlohmann::json& status) const override {
        if (!status.is_object()) {
            return std::nullopt; // not a panda_breath diagnostics frame
        }
        // A delta frame carries only what changed, so recognition cannot hinge
        // on one field: any stock-specific key marks the frame ours.
        bool known_field = false;
        for (const char* f : kStockFields) {
            if (status.contains(f)) {
                known_field = true;
                break;
            }
        }
        if (!known_field) {
            return std::nullopt; // not a panda_breath diagnostics frame
        }
        ChamberHeaterDiagnostics d;
        // The binding holds a WebSocket to the appliance and reports whether it
        // is up. A value we cannot read is not evidence the appliance is gone,
        // so a malformed slot engages as connected rather than raising the
        // offline banner.
        if (status.contains("connected")) {
            d.device_connected =
                status["connected"].is_boolean() ? status["connected"].get<bool>() : true;
        }
        // Only the Klipper-target mode is our target closing the loop; the auto
        // and drying cycles are the appliance running itself. work_mode latches
        // at its last value once the output stops, so work_on is what makes the
        // answer present-tense. Both halves are needed for an answer, and a
        // value we cannot read fails toward not raising the badge.
        if (status.contains("work_mode") && status.contains("work_on")) {
            int work_mode = kWorkModeKlipperTarget;
            if (status["work_mode"].is_number_integer()) {
                work_mode = status["work_mode"].get<int>();
            }
            bool work_on = false;
            if (status["work_on"].is_boolean()) {
                work_on = status["work_on"].get<bool>();
            }
            d.externally_controlled = work_on && work_mode != kWorkModeKlipperTarget;
        }
        // The drying cycle: filament_temp is its target, filament_timer its
        // length in whole hours, remaining_seconds the countdown. A value we
        // cannot read is no report.
        if (status.contains("filament_drying_active") &&
            status["filament_drying_active"].is_boolean()) {
            d.drying_active = status["filament_drying_active"].get<bool>();
        }
        if (status.contains("remaining_seconds") &&
            status["remaining_seconds"].is_number_integer()) {
            d.drying_remaining_s = std::max(status["remaining_seconds"].get<int>(), 0);
        }
        if (status.contains("filament_temp") && status["filament_temp"].is_number()) {
            d.drying_target_c =
                static_cast<int>(std::lround(status["filament_temp"].get<double>()));
        }
        if (status.contains("filament_timer") && status["filament_timer"].is_number_integer()) {
            d.drying_duration_min = status["filament_timer"].get<int>() * 60;
        }
        return d;
    }
};

const PandaBreathBackend kPandaBreath;

} // namespace

const ChamberHeaterBackend* panda_breath_backend_instance() {
    return &kPandaBreath;
}

} // namespace helix::chamber
