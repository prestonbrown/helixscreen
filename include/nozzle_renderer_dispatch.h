// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/// @file nozzle_renderer_dispatch.h
/// @brief Toolhead style dispatch — selects renderer based on user settings

#pragma once

#include "nozzle_renderer_a4t.h"
#include "nozzle_renderer_bambu.h"
#include "nozzle_renderer_faceted.h"
#include "settings_manager.h"

/// @brief Draw the nozzle matching the user's effective toolhead style setting
inline void draw_nozzle_for_style(lv_layer_t* layer, int32_t cx, int32_t cy,
                                  lv_color_t filament_color, int32_t scale_unit,
                                  lv_opa_t opa = LV_OPA_COVER) {
    switch (helix::SettingsManager::instance().get_effective_toolhead_style()) {
        case helix::ToolheadStyle::STEALTHBURNER:
            draw_nozzle_faceted(layer, cx, cy, filament_color, scale_unit, opa);
            break;
        case helix::ToolheadStyle::A4T:
            draw_nozzle_a4t(layer, cx, cy, filament_color, scale_unit, opa);
            break;
        default:
            draw_nozzle_bambu(layer, cx, cy, filament_color, scale_unit, opa);
            break;
    }
}
