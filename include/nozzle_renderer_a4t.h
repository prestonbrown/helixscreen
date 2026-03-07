// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/// @file nozzle_renderer_a4t.h
/// @brief Armored Turtle (A4T) toolhead renderer
///
/// Vector-drawn A4T print head using LVGL polygon primitives.
/// Distinctive dark body with green hexagonal honeycomb accents.

#pragma once

#include "lvgl/lvgl.h"

/// @brief Draw Armored Turtle (A4T) print head
///
/// Creates a vector rendering of the A4T toolhead with:
/// - Dark rectangular housing with beveled bottom corners
/// - Extruder section on top with gear detail
/// - Green hexagonal honeycomb accent pattern (A4T signature)
/// - Central fan circle
/// - Tapered nozzle tip
///
/// The toolhead body is always dark with A4T green (#BFBB4B) accents.
/// Nozzle tip shows filament color when loaded.
///
/// @param layer LVGL draw layer
/// @param cx Center X position
/// @param cy Center Y position (center of entire print head)
/// @param filament_color Color of loaded filament (or gray/black for default)
/// @param scale_unit Base scaling unit (typically from theme space_md)
/// @param opa Opacity (default LV_OPA_COVER)
void draw_nozzle_a4t(lv_layer_t* layer, int32_t cx, int32_t cy, lv_color_t filament_color,
                     int32_t scale_unit, lv_opa_t opa = LV_OPA_COVER);
