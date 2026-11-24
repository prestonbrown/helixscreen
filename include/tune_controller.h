// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>

namespace helix {
class IMoonrakerAPI;
}

namespace helix::tune {

/// The single agreed clamp for speed and flow overrides.
///
/// Before this existed, PrintTuneOverlay clamped speed to [50,200] and flow to
/// [75,125] while a dead ControlsPanel path used [10,200] and [50,150]. These
/// are the shipped values — the ones users have actually been getting.
inline constexpr int kSpeedMinPct = 50;
inline constexpr int kSpeedMaxPct = 200;
inline constexpr int kFlowMinPct = 75;
inline constexpr int kFlowMaxPct = 125;

int clamp_speed_percent(int pct);
int clamp_flow_percent(int pct);

/// Clamp and send M220. No-op when `api` is null. Errors surface via NOTIFY_ERROR.
void set_speed_percent(IMoonrakerAPI* api, int pct);

/// Clamp and send M221. No-op when `api` is null. Errors surface via NOTIFY_ERROR.
void set_flow_percent(IMoonrakerAPI* api, int pct);

/// The Tune overlay's pressure-advance step: one tap is 0.001 of K, two
/// buttons give 0.005.
inline constexpr float kPaTuneStep = 0.001f;

/// Whether `extruder` runs a nonlinear pressure-advance model. A bare
/// SET_PRESSURE_ADVANCE ADVANCE= there replaces only linear_advance and keeps the
/// tuned offset, a mix nobody tuned, so no classic-K path may send one.
bool pressure_advance_is_nonlinear(const std::string& extruder);

/// Clamp and send SET_PRESSURE_ADVANCE for `extruder`. No-op when `api` is
/// null, or when `extruder` runs a nonlinear model (see above). The printer's reported
/// pressure_advance is the readback: callers display that, not the value they asked for. Errors
/// surface via NOTIFY_ERROR.
void set_pressure_advance(IMoonrakerAPI* api, const std::string& extruder, float k);

/// K from the active-extruder pressure-advance subject (K x 10000), nullopt
/// while the extruder has not reported one.
std::optional<float> pressure_advance_from_subject(int value);

/// The readout for that subject: four decimals, or a dash before any report.
std::string pressure_advance_text(int subject_value);

/// Volumetric flow for a live extruder velocity given in centi-mm/s, the unit
/// PrinterState publishes it in, through filament of the given diameter. Never
/// below 0: a retract reads as no flow.
double volumetric_flow_mm3_s(int extruder_velocity_centimm_s, float filament_diameter_mm);

struct SpeedFlowText {
    std::string speed;
    std::string flow;
};

/// A Speed/Flow readout pair: the override percentages, or the measured
/// toolhead speed and live volumetric flow. Measured rather than commanded
/// speed, because the commanded feed rate holds its last value while the
/// toolhead sits still.
SpeedFlowText status_speed_flow_text(bool physical_units, int speed_pct, int flow_pct,
                                     int live_velocity_mm_s, int extruder_velocity_centimm_s,
                                     float filament_diameter_mm);

/// The measured toolhead speed, the one format every readout shows: "187 mm/s".
std::string live_speed_text(int live_velocity_mm_s);

/// A volumetric flow, the one format every readout shows: "12.0 mm³/s".
std::string live_flow_text(double flow_mm3_s);

/// Both live rates, speed over flow: "187 mm/s\n12.0 mm³/s".
std::string live_rates_text(int live_velocity_mm_s, double flow_mm3_s);

} // namespace helix::tune
