// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_catalog_inputs.h"

#include "printer_capabilities_state.h"
#include "printer_detector.h"
#include "printer_profile_state.h"
#include "printer_state.h"
#include "probe_sensor_manager.h"

namespace helix::calibration {

namespace {

bool z_probe_contacts_nozzle() {
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
    return pick && sensors::probe_contacts_nozzle(pick->type);
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
    in.nozzle_contact_probe = z_probe_contacts_nozzle();
    if (!profile.printer_type().empty()) {
        in.tier_overrides = tier_overrides_from(
            PrinterDetector::get_calibration_step_tiers(profile.printer_type()));
    }
    return in;
}

std::vector<StepInfo> current_printer_steps(const PrinterState& state) {
    return applicable_steps(catalog_inputs_from(state));
}

} // namespace helix::calibration
