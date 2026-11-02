// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tune_controller.h"

#include "ui_error_reporting.h"

#include "filament_database.h"
#include "format_utils.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "pa_tuning.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace helix::tune {

int clamp_speed_percent(int pct) {
    return std::clamp(pct, kSpeedMinPct, kSpeedMaxPct);
}

int clamp_flow_percent(int pct) {
    return std::clamp(pct, kFlowMinPct, kFlowMaxPct);
}

void set_speed_percent(IMoonrakerAPI* api, int pct) {
    if (!api) {
        return;
    }
    const int value = clamp_speed_percent(pct);
    api->execute_gcode(
        "M220 S" + std::to_string(value),
        [value]() { spdlog::debug("[TuneController] Speed set to {}%", value); },
        [](const MoonrakerError& err) {
            spdlog::error("[TuneController] Failed to set speed: {}", err.message);
            helix::ui::notify_error_tr(TR_NOOP("Failed to set print speed: {}"), err);
        });
}

void set_flow_percent(IMoonrakerAPI* api, int pct) {
    if (!api) {
        return;
    }
    const int value = clamp_flow_percent(pct);
    api->execute_gcode(
        "M221 S" + std::to_string(value),
        [value]() { spdlog::debug("[TuneController] Flow set to {}%", value); },
        [](const MoonrakerError& err) {
            spdlog::error("[TuneController] Failed to set flow: {}", err.message);
            helix::ui::notify_error_tr(TR_NOOP("Failed to set flow rate: {}"), err);
        });
}

void set_pressure_advance(IMoonrakerAPI* api, const std::string& extruder, float k) {
    if (!api || extruder.empty()) {
        return;
    }
    const float value = helix::pa::clamp_k(k);
    api->execute_gcode(
        helix::pa::set_pa_gcode(extruder, value),
        [extruder, value]() {
            spdlog::debug("[TuneController] Pressure advance on {} set to {:.4f}", extruder, value);
        },
        [](const MoonrakerError& err) {
            spdlog::error("[TuneController] Failed to set pressure advance: {}", err.message);
            helix::ui::notify_error_tr(TR_NOOP("Failed to set pressure advance: {}"), err);
        });
}

std::optional<float> pressure_advance_from_subject(int value) {
    if (value < 0) {
        return std::nullopt;
    }
    return static_cast<float>(value) / 10000.0f;
}

std::string pressure_advance_text(int subject_value) {
    const auto k = pressure_advance_from_subject(subject_value);
    return k ? helix::pa::format_k(*k) : std::string("--");
}

double volumetric_flow_mm3_s(int extruder_velocity_centimm_s, float filament_diameter_mm) {
    // A retract moves the extruder backwards; no filament is flowing then.
    return std::max(0.0, (extruder_velocity_centimm_s / 100.0) *
                             filament::cross_section_mm2(filament_diameter_mm));
}

SpeedFlowText status_speed_flow_text(bool physical_units, int speed_pct, int flow_pct,
                                     int live_velocity_mm_s, int extruder_velocity_centimm_s,
                                     float filament_diameter_mm) {
    char speed[32];
    char flow[32];
    if (physical_units) {
        std::snprintf(speed, sizeof(speed), "%s", live_speed_text(live_velocity_mm_s).c_str());
        std::snprintf(
            flow, sizeof(flow), "%s",
            live_flow_text(volumetric_flow_mm3_s(extruder_velocity_centimm_s, filament_diameter_mm))
                .c_str());
    } else {
        helix::format::format_percent(speed_pct, speed, sizeof(speed));
        helix::format::format_percent(flow_pct, flow, sizeof(flow));
    }
    return {speed, flow};
}

std::string live_speed_text(int live_velocity_mm_s) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d mm/s", live_velocity_mm_s);
    return buf;
}

std::string live_flow_text(double flow_mm3_s) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.1f mm\xC2\xB3/s", flow_mm3_s);
    return buf;
}

std::string live_rates_text(int live_velocity_mm_s, double flow_mm3_s) {
    return live_speed_text(live_velocity_mm_s) + "\n" + live_flow_text(flow_mm3_s);
}

} // namespace helix::tune
