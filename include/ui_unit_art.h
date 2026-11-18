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
#include <string>
#include <vector>

namespace helix {
struct AmsSystemInfo;
struct SlotInfo;
} // namespace helix

namespace helix::ui::tray {
struct UnitShell;
}

namespace helix::ui {

enum class UnitArtTier { Full, Thumb };

/// How the art shows that its unit is heating.
enum class UnitArtGlow { Off, Steady, Pulsing };

/// Off unless the unit is drying and the board can afford the effect; it only moves where
/// animations are on.
UnitArtGlow unit_art_glow_for(bool drying, bool animations_enabled, bool reduced_effects);

/// Where the chamber lights up, one byte per pixel of a @p w by @p h drawing: the union of the
/// lane regions, softened and spread a little around them, then shadowed by whatever the front
/// layer draws over it. Solid case and frame pieces stay dark; glass lets the light through.
std::vector<uint8_t> unit_art_chamber_mask(const std::vector<uint8_t>& lane_alpha,
                                           const std::vector<uint8_t>& front_alpha, int w, int h);

/// Strength of the glow at one pixel, from the chamber mask and the drawing's own alpha.
uint8_t unit_art_glow_alpha(uint8_t chamber, uint8_t art_alpha);

struct UnitArtLane {
    bool filled = false;
    uint32_t color = 0;
    float fill = 1.0f; ///< how full its spool is drawn, 0..1
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

/// What a picture shows: the one unit, or the whole device it is part of. They differ only for
/// a device a backend reports as several units (tray::device_shell_for).
enum class UnitArtScope { Unit, Device };

/// Whether the unit, or its device, has art for exactly the bays it reports.
bool unit_art_available(const AmsSystemInfo& info, int unit_index, bool has_dryer = false,
                        UnitArtScope scope = UnitArtScope::Unit);

void register_unit_art_widget();
lv_obj_t* unit_art_create(lv_obj_t* parent, UnitArtTier tier);
void unit_art_set_unit(lv_obj_t* art, const AmsSystemInfo& info, int unit_index,
                       bool has_dryer = false, UnitArtScope scope = UnitArtScope::Unit);

/// Whether the widget currently shows its single flattened image.
bool unit_art_is_flattened(lv_obj_t* art);

/// Show @p glow inside the drawing. A widget without a drawing yet shows none.
void unit_art_set_glow(lv_obj_t* art, UnitArtGlow glow);

/// The glow image, or nullptr when no glow has been shown.
lv_obj_t* unit_art_glow_obj(lv_obj_t* art);

/// The tinted image of @p lane, or nullptr before the widget has art.
lv_obj_t* unit_art_lane_color_obj(lv_obj_t* art, int lane);

/// The drawing's width over its height, or 0 when it has no baked lane geometry.
float unit_art_aspect(lv_obj_t* art);

/// Where lane @p lane's spool (its flanges' bounds) and outlet stand in @p art at its current
/// size, relative to the widget, from the bake's lane geometry. False without geometry.
bool unit_art_lane_place(lv_obj_t* art, int lane, lv_area_t& spool, lv_point_t& outlet);

/// The two ends of the unit's bottom edge as drawn at @p art's current size, relative to the
/// widget, from the bake. False without geometry.
bool unit_art_bottom_edge(lv_obj_t* art, lv_point_t& a, lv_point_t& b);

/// What the drawing covers at @p art's current size, relative to the widget.
bool unit_art_drawn_area(lv_obj_t* art, lv_area_t& out);

/// How one lane of the drawing is marked.
struct UnitArtLaneMark {
    bool glow = false; ///< soft light just behind the spool, in @p color
    lv_color_t color{};
    bool dim = false; ///< the spool faded into the card, as a bay that is not there
};

/// Marks each lane of @p art, in its own place in the drawing's depth: a glow just behind
/// the lane's spool and a dim just in front of it, so whatever stands nearer covers both.
/// Lanes past @p marks keep none.
void unit_art_set_lane_marks(lv_obj_t* art, const std::vector<UnitArtLaneMark>& marks);

/// The mark lane @p lane shows.
UnitArtLaneMark unit_art_lane_mark(lv_obj_t* art, int lane);

/// The drawing's layers in the order they are composed, back to front: "back", each lane's
/// stack left to right ("lane<N>_glow", "lane<N>_body", "lane<N>_tint", "lane<N>_shade",
/// "lane<N>_dim", as the drawing has them), then "front".
std::vector<std::string> unit_art_compose_order(lv_obj_t* art);

/// Whether lane @p lane's filament is masked to a part-wound spool, and to what percent.
int unit_art_lane_fill_pct(lv_obj_t* art, int lane);

/// The image currently showing the sliding selector, or nullptr for a unit without one.
lv_obj_t* unit_art_carriage_obj(lv_obj_t* art);

} // namespace helix::ui
