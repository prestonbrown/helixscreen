// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "plate_glow.h"

/**
 * @file ui_plate_glow.h
 * @brief <plate_glow>: the heat glow over a printer picture's bed plate.
 *
 * An image that draws an 8-bit coverage mask (plate_glow.h) in its
 * `style_image_recolor` color, so the XML picks the color from a theme token
 * and its strength with `style_opa`:
 *
 *   <plate_glow name="callout_bed_glow" style_image_recolor="#danger"/>
 *
 * The owner hands it a mask with plate_glow_set_mask(), which moves and sizes
 * the widget to the mask; the widget owns the pixels and frees them with itself.
 */
namespace helix::ui {

void register_plate_glow_widget();

/// Show `mask`, placed at its x/y in the parent's coordinates. An empty mask
/// leaves the widget drawing nothing.
void plate_glow_set_mask(lv_obj_t* obj, const PlateGlowMask& mask);

} // namespace helix::ui
