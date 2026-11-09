// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_unit_art.h
 * @brief <unit_art tier="full|thumb">: a baked drawing of a filament unit with live lane colours.
 *
 * Layers, bottom to top: the shell's back, each filled lane's tinted region and its shading,
 * then the shell's front. A unit with a sliding selector has one more layer on top, moved to
 * the active gate. A unit with no matching art hides the widget.
 */

#include "lvgl/lvgl.h"

#include <cstdint>

namespace helix {
struct AmsSystemInfo;
struct SlotInfo;
} // namespace helix

namespace helix::ui {

enum class UnitArtTier { Full, Thumb };

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

/// The tinted image of @p lane, or nullptr before the widget has art.
lv_obj_t* unit_art_lane_color_obj(lv_obj_t* art, int lane);

/// The image currently showing the sliding selector, or nullptr for a unit without one.
lv_obj_t* unit_art_carriage_obj(lv_obj_t* art);

} // namespace helix::ui
