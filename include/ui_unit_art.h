// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_unit_art.h
 * @brief <unit_art tier="full|thumb">: a baked drawing of a filament unit with live lane colours.
 *
 * Layers, bottom to top: the shell's back, each filled lane's tinted region and its shading,
 * then the shell's front. A unit with no matching art hides the widget.
 */

#include "lvgl/lvgl.h"

#include <cstdint>

namespace helix {
struct AmsSystemInfo;
struct SlotInfo;
} // namespace helix

namespace helix::ui {

enum class UnitArtTier { Full, Thumb };

/// How the art shows that its unit is heating.
enum class UnitArtGlow { Off, Steady, Pulsing };

/// Off unless the unit is drying and the board can afford the effect; it only moves where
/// animations are on.
UnitArtGlow unit_art_glow_for(bool drying, bool animations_enabled, bool reduced_effects);

/// Strength of the glow at one pixel of a @p w by @p h drawing whose own alpha there is
/// @p art_alpha. Zero wherever the drawing is transparent, so it takes the unit's outline.
uint8_t unit_art_glow_alpha(uint8_t art_alpha, int x, int y, int w, int h);

struct UnitArtLane {
    bool filled = false;
    uint32_t color = 0;
};

/// What one bay shows.
UnitArtLane unit_art_lane(const SlotInfo& slot);

/// Whether the unit has art for exactly the bays it reports.
bool unit_art_available(const AmsSystemInfo& info, int unit_index, bool has_dryer = false);

void register_unit_art_widget();
lv_obj_t* unit_art_create(lv_obj_t* parent, UnitArtTier tier);
void unit_art_set_unit(lv_obj_t* art, const AmsSystemInfo& info, int unit_index,
                       bool has_dryer = false);

/// Whether the widget currently shows its single flattened image.
bool unit_art_is_flattened(lv_obj_t* art);

/// Show @p glow inside the drawing. A widget without a drawing yet shows none.
void unit_art_set_glow(lv_obj_t* art, UnitArtGlow glow);

/// The glow image, or nullptr when no glow has been shown.
lv_obj_t* unit_art_glow_obj(lv_obj_t* art);

/// The tinted image of @p lane, or nullptr before the widget has art.
lv_obj_t* unit_art_lane_color_obj(lv_obj_t* art, int lane);

} // namespace helix::ui
