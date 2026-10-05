// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tune_controller.h"

#include "ui_error_reporting.h"

#include "filament_database.h"
#include "format_utils.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
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

double volumetric_flow_mm3_s(int extruder_velocity_centimm_s, float filament_diameter_mm) {
    return (extruder_velocity_centimm_s / 100.0) *
           filament::cross_section_mm2(filament_diameter_mm);
}

SpeedFlowText status_speed_flow_text(bool physical_units, int speed_pct, int flow_pct,
                                     int live_velocity_mm_s, int extruder_velocity_centimm_s,
                                     float filament_diameter_mm) {
    char speed[32];
    char flow[32];
    if (physical_units) {
        std::snprintf(speed, sizeof(speed), "%d mm/s", live_velocity_mm_s);
        std::snprintf(flow, sizeof(flow), "%.1f mm\xC2\xB3/s",
                      volumetric_flow_mm3_s(extruder_velocity_centimm_s, filament_diameter_mm));
    } else {
        helix::format::format_percent(speed_pct, speed, sizeof(speed));
        helix::format::format_percent(flow_pct, flow, sizeof(flow));
    }
    return {speed, flow};
}

} // namespace helix::tune
