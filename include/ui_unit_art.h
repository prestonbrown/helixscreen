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
#include <string>

namespace helix {
struct AmsSystemInfo;
struct SlotInfo;
} // namespace helix

namespace helix::ui::tray {
struct UnitShell;
}

namespace helix::ui {

enum class UnitArtTier { Full, Thumb };

struct UnitArtLane {
    bool filled = false;
    uint32_t color = 0;
};

/// Which drawing a unit that holds no spools uses: spools behind it, or tube stubs.
enum class FeederLook { Spools, Stubs };

/// The look named by the HELIX_UNIT_ART_FEEDER value @p value; Spools unless it says stubs.
FeederLook parse_feeder_look(const char* value);

/// The asset path, without extension or layer, of a unit's drawing at @p lanes bays.
std::string unit_art_base_stem(const tray::UnitShell& shell, int lanes, UnitArtTier tier,
                               FeederLook look);

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

} // namespace helix::ui
