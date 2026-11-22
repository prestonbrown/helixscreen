// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_catalog_inputs.h"

#include "calibration_config_results.h"
#include "pid_calibrate_tolerance.h"
#include "printer_capabilities_state.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "printer_profile_state.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "probe_sensor_manager.h"
#include "temperature_rules.h"
#include "thermal_rate_model.h"

namespace helix::calibration {

namespace {

/// The Z probe's type, or STANDARD when the printer reports no probe sensor.
sensors::ProbeSensorType z_probe_type() {
    const auto sensors = sensors::ProbeSensorManager::instance().get_sensors();
    const sensors::ProbeSensorConfig* pick = nullptr;
    for (const auto& s : sensors) {
        if (s.enabled && s.role == sensors::ProbeSensorRole::Z_PROBE) {
            pick = &s;
            break;
        }
    }
    if (!pick && !sensors.empty()) {
        pick = &sensors.front();
    }
    return pick ? pick->type : sensors::ProbeSensorType::STANDARD;
}

} // namespace

CatalogInputs catalog_inputs_from(const PrinterState& state) {
    const auto& caps = state.capabilities_state();
    const auto has = [&caps](Capability c) { return lv_subject_get_int(caps.subject(c)) != 0; };
    const auto& profile = state.profile_state();

    CatalogInputs in;
    in.has_probe = has(Capability::HasProbe);
    in.has_bed_mesh = has(Capability::HasBedMesh);
    in.has_accelerometer = has(Capability::HasAccelerometer);
    in.can_tension_belts = has(Capability::CanTensionBelts);
    in.can_calibrate_cutter = has(Capability::CanCalibrateCutter);
    in.has_pa_cal = has(Capability::HasPaCal);
    in.has_tool_offset_cal = has(Capability::HasToolOffsetCal);
    in.has_screws_tilt = has(Capability::HasScrewsTilt);
    in.has_stepper_resonance_cal = has(Capability::HasStepperResonanceCal);
    in.supports_belt_compare = has(Capability::SupportsBeltCompare);
    in.supports_ripple_check = has(Capability::SupportsRippleCheck);
    in.hide_manual_z_calibration = has(Capability::HideManualZCalibration);
    in.z_offset_reapplied_per_head = profile.z_offset_reapplied_per_head();
    in.z_strategy = profile.z_offset_calibration_strategy();
    const auto probe = z_probe_type();
    in.nozzle_contact_probe = sensors::probe_contacts_nozzle(probe);
    in.probe_sets_nozzle_height = sensors::probe_sets_nozzle_height(probe) ||
                                  sensors::ProbeSensorManager::instance().z_probe_sets_z_by_touch();
    if (!profile.printer_type().empty()) {
        in.tier_overrides = tier_overrides_from(
            PrinterDetector::get_calibration_step_tiers(profile.printer_type()));
    }
    return in;
}

HeaterPricing price_heater(PrinterState& state, const std::string& heater, int target_c) {
    HeaterPricing p;
    p.bed = heater == "heater_bed";
    auto& temps = state.temperature_state();
    lv_subject_t* reading =
        p.bed ? temps.get_bed_temp_subject() : temps.get_extruder_temp_subject(heater);
    const float now_c =
        reading ? temperature_rules::deci_to_degrees_f(lv_subject_get_int(reading)) : 0.0f;
    auto& rates = ThermalRateManager::instance();
    p.heat_seconds = rates.estimate_heating_seconds(heater, now_c, static_cast<float>(target_c));
    p.climb_rate = rates.get_model(heater).climb_rate();
    const auto& hardware = state.get_discovery();
    const auto& controls = hardware.heater_controls_in_config();
    const auto control = controls.find(heater);
    p.mpc = control != controls.end() && control->second == "mpc";
    p.tolerance = helix::pid::calibrate_tolerance(hardware.pid_calibrate_takes_tolerance(), heater)
                      .has_value();
    return p;
}

std::vector<HeaterPricing> heater_pricing(PrinterState& state) {
    std::vector<HeaterPricing> out{
        price_heater(state, state.temperature_state().active_extruder_name(), kRunNozzleTempC)};
    if (state.get_discovery().has_heater_bed()) {
        out.push_back(price_heater(state, "heater_bed", kRunBedTempC));
    }
    return out;
}

std::vector<StepInfo> current_printer_steps(PrinterState& state) {
    auto steps = applicable_steps(catalog_inputs_from(state));
    for (auto& s : steps) {
        if (s.kind == StepKind::Heaters) {
            s.minutes = heaters_step_minutes(heater_pricing(state));
        }
    }
    return steps;
}

} // namespace helix::calibration
