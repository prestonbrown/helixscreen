// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file filament_tube_stroker.h
 * @brief Shared concentric tube stroker for AMS filament-path rendering.
 *
 * A "tube" is rendered as a set of CONCENTRIC stroke passes (no perpendicular
 * offsets) over a pathgeo::FilamentPath. Straight runs are drawn with
 * lv_draw_line (float coords — we build with LV_USE_FLOAT) and corners as a fan
 * of short straight chords sampled from the arc's exact float parametrization
 * (also lv_draw_line, NOT lv_draw_arc, whose integer center / outer radius would
 * round the band ~0.5px out of alignment with the adjoining lines). Opaque
 * passes round-cap every joint, chord to chord included, so angled joints leave
 * no wedge notches.
 *
 * This layer is shared by both AMS path canvases:
 *   - ui_filament_path_canvas (single-unit detail panel)
 *   - ui_system_path_canvas   (multi-unit overview)
 * so both produce identical clean quarter-arc fillet curves. Callers supply
 * lane width / color choices via LaneStyle (the overview's lanes are
 * intentionally thinner/dimmer than the detail panel).
 */

#include "board_identity.h"
#include "filament_path_geometry.h"
#include "lvgl/lvgl.h"

namespace helix {
namespace ui {

namespace pg = pathgeo;

// Centerline fillet radius for orthogonal lane routing (route_orthogonal clamps
// internally, so tight layouts degrade gracefully to jogs / straight runs).
inline constexpr float kFilletRadius = 12.0f;

// Halo around the active route: two opaque bands pre-blended from the background
// toward the wall color, so overlapping chords and bends never double-blend.
inline constexpr int32_t kHaloWidthExtra = 6; // outer band: gauge + 6
inline constexpr int32_t kHaloInnerExtra = 3; // inner band: gauge + 3
inline constexpr float kHaloOuterMix = 0.25f; // wall share, outer band
inline constexpr float kHaloInnerMix = 0.55f; // wall share, inner band

/// Whether a board's row or its memory size calls for reduced effects. Pure: the
/// decision behind reduced_effects(), separate from its cached evaluation.
bool reduced_effects_for(const board::Info& board, bool constrained_memory);

/// Detect low-performance platforms: a board whose row asks for it (K2) or a
/// constrained-memory device (the K1C's 214MB qualifies, the AD5X's 485MB does not).
/// When true, the stroker drops the halo; walls and bore still draw.
bool reduced_effects();

/// Color helpers (delegate to ams_draw::* — kept here so callers and the
/// stroker share one definition).
lv_color_t tube_darken(lv_color_t c, uint8_t amt);
lv_color_t tube_lighten(lv_color_t c, uint8_t amt);
lv_color_t tube_blend(lv_color_t c1, lv_color_t c2, float factor);

// One concentric stroke pass.
struct TubePass {
    lv_color_t color;
    int32_t width;
    lv_opa_t opa;
};

// A PTFE sleeve: 1 px walls around a bore that shows the filament when loaded.
struct LaneStyle {
    lv_color_t wall; // idle wall token, theme accent on the active route, error on error
    lv_color_t bore; // filament color when loaded, background when empty
    lv_color_t bg;   // background the halo bands pre-blend against
    int32_t width;   // outer gauge, walls included
    bool halo;       // active route only; halo color is the wall color
};

// Paint layers of one tube, bottom to top.
enum class TubeLayer : uint8_t { Halo, Wall, Bore };

/// Stroke a path with N concentric passes.
void stroke_path(lv_layer_t* layer, const pg::FilamentPath& path, const TubePass* passes,
                 int n_passes);

/// Build one layer's concentric passes for a LaneStyle. Returns the number of
/// passes written into @p out (at most 2). @p simple drops the halo.
int build_passes(const LaneStyle& style, TubeLayer layer, TubePass* out,
                 bool simple = reduced_effects());

/// Build a LaneStyle from slot state in ONE place. Only a loaded lane on the
/// active route gets accent walls and the halo.
LaneStyle lane_style(bool has_filament, bool active, lv_color_t fill, lv_color_t idle_wall,
                     lv_color_t accent, lv_color_t bg, int32_t gauge);

/// A lane's passes for every layer, in paint order (Halo, Wall, Bore). Returns
/// the count; @p out holds at least 4.
int lane_passes(const LaneStyle& style, TubePass* out, bool simple = reduced_effects());

/// Active-route wall color: the theme's "primary", lightened in light theme so
/// it reads blue rather than navy against the card.
lv_color_t tube_accent();

/// Paint the chevrons chevrons_along() places on @p path: each one two
/// round-capped arms of @p size.stroke in @p color, skipping any that would
/// touch one of the @p n_avoid boxes (labels drawn over the tube). Shows
/// filament moving through a tube on the AMS path canvas and the filament
/// panel's motion glyph.
void draw_flow_chevrons(lv_layer_t* layer, const pg::FilamentPath& path,
                        const pg::ChevronSize& size, float phase, bool reverse, lv_color_t color,
                        lv_opa_t opa, const bool* seg_shown = nullptr,
                        const pg::PathBox* avoid = nullptr, int n_avoid = 0);

} // namespace ui
} // namespace helix
