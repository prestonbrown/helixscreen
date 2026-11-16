// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/// @file nozzle_renderer_dispatch.h
/// @brief Toolhead style dispatch — selects renderer based on user settings

#pragma once

#include "nozzle_renderer_a4t.h"
#include "nozzle_renderer_anthead.h"
#include "nozzle_renderer_bambu.h"
#include "nozzle_renderer_creality_k1.h"
#include "nozzle_renderer_creality_k2.h"
#include "nozzle_renderer_jabberwocky.h"
#include "nozzle_renderer_stealthburner.h"
#include "settings_manager.h"
#include "toolhead_art.h"

/// @brief Draw the nozzle matching the user's effective toolhead style setting
/// @param filament Loaded filament color, or nullopt when no filament is at the nozzle
inline void draw_nozzle_for_style(lv_layer_t* layer, int32_t cx, int32_t cy,
                                  std::optional<lv_color_t> filament, int32_t scale_unit,
                                  lv_opa_t opa = LV_OPA_COVER) {
    const helix::ToolheadStyle style =
        helix::SettingsManager::instance().get_effective_toolhead_style();
    if (helix::ui::draw_toolhead_art(layer, style, cx, cy, filament, scale_unit, opa))
        return;
    switch (style) {
    case helix::ToolheadStyle::A4T:
        draw_nozzle_a4t(layer, cx, cy, filament, scale_unit, opa);
        break;
    case helix::ToolheadStyle::ANTHEAD:
        draw_nozzle_anthead(layer, cx, cy, filament, scale_unit, opa);
        break;
    case helix::ToolheadStyle::JABBERWOCKY:
        draw_nozzle_jabberwocky(layer, cx, cy, filament, scale_unit, opa);
        break;
    case helix::ToolheadStyle::STEALTHBURNER:
        draw_nozzle_stealthburner(layer, cx, cy, filament, scale_unit, opa);
        break;
    case helix::ToolheadStyle::CREALITY_K1:
        draw_nozzle_creality_k1(layer, cx, cy, filament, scale_unit, opa);
        break;
    case helix::ToolheadStyle::CREALITY_K2:
        draw_nozzle_creality_k2(layer, cx, cy, filament, scale_unit, opa);
        break;
    default:
        draw_nozzle_bambu(layer, cx, cy, filament, scale_unit, opa);
        break;
    }
}
