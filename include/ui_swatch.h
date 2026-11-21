// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filament_variants.h"
#include "lvgl/lvgl.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix {
struct SlotInfo;
}

/**
 * @file ui_swatch.h
 * @brief The app's one colour-swatch painter: every swatch, chip band, colour
 *        dot and spool illustration ring draws its colour through here.
 *
 * Three layers, each built on the one below:
 *  - Pure decisions (no LVGL objects): the light/dark contrast edge, the
 *    multi-colour list, the equal-area diagonal split clipped to the shape,
 *    the finish a filament name announces and the size below which a finish
 *    is too small to read.
 *  - draw_swatch(): paints one swatch into a layer: the temp-graph legend
 *    calls it directly, the object paint below for everything else. Its finish
 *    texture is rendered once into a SwatchCache image and blitted.
 *  - swatch_paint_finish_pixels(): the same texture written straight into a
 *    pixel buffer, for illustrations that draw their own pixels (spool_canvas).
 *  - Object paint: apply_swatch_color() and the <swatch_paint> XML widget hang
 *    a DRAW_MAIN hook on an lv_obj so it paints through draw_swatch() with the
 *    object's own radius and size.
 *
 * Shape is the caller's: a pure colour is a circle (LV_RADIUS_CIRCLE), a
 * labelled chip or a colour area a rounded rect. See docs/devel/COLOR_SWATCHES.md.
 */

namespace helix::ui {

/// How a filament's surface is painted. Derived from its name by
/// swatch_finish_from_variant(); never chosen by a widget.
enum class SwatchFinish : uint8_t { Plain, Matte, Silk, Marble, Sparkle };

/// A swatch smaller than this many pixels square (by area) is too small to read
/// a finish and paints plain colour (tool dots, legend dots).
inline constexpr int32_t kSwatchFinishMinPx = 16;

/// Below this WCAG contrast ratio a swatch blends into its surface and gets an
/// edge: black on a dark navy panel sits near 1.6, a mid gray on a dark dialog
/// near 1.9.
inline constexpr double kSwatchEdgeContrastRatio = 1.75;

// ---------------------------------------------------------------------------
// Pure decisions
// ---------------------------------------------------------------------------

/// Whether a swatch filled with @p rgb blends into @p surface (too little
/// luminance contrast, dark-on-dark or light-on-light) and needs an edge.
bool swatch_needs_edge(uint32_t rgb, uint32_t surface);

/// The surfaces a swatch's edge is judged against: its actual @p backdrop when
/// known, else the current theme's panel and dialog surfaces. Theme colours are
/// cached and re-read only when the theme changes.
std::vector<uint32_t> swatch_edge_surfaces(std::optional<uint32_t> backdrop = std::nullopt);

/// swatch_needs_edge() against swatch_edge_surfaces() with no known backdrop.
bool swatch_needs_edge_here(uint32_t rgb);

/// Whether any of @p colors needs an edge against any of @p surfaces.
bool swatch_colors_need_edge(const std::vector<lv_color_t>& colors,
                             const std::vector<uint32_t>& surfaces);

/// Parse "#RRGGBB,#RRGGBB,..." (each entry optionally '#'-prefixed, blanks
/// trimmed); unparseable entries are skipped.
std::vector<lv_color_t> parse_multi_colors(const std::string& csv);

/// The finish a filament name announces ("PLA Silk", "Galaxy Black").
SwatchFinish swatch_finish_from_variant(std::string_view name);

/// The swatch finish for a filament finish.
SwatchFinish swatch_finish(helix::FilamentFinish finish);

/// The finish of a slot's filament (SlotInfo::finish()).
SwatchFinish swatch_finish_of(const helix::SlotInfo& slot);

/// @p finish, or Plain when a @p w x @p h swatch has less area than a
/// kSwatchFinishMinPx square.
SwatchFinish swatch_effective_finish(SwatchFinish finish, int32_t w, int32_t h);

/// A matte filament's colour: slightly desaturated toward its own grey.
lv_color_t swatch_matte_color(lv_color_t c);

struct SwatchPoint {
    float x;
    float y;
};
using SwatchPolygon = std::vector<SwatchPoint>;

/// How much of the pixel centred on (x, y) a shape covers, 0..1.
using SwatchCoverage = std::function<float(float x, float y)>;

/// The outline of a @p area rounded by @p radius (clamped to half the short
/// side; LV_RADIUS_CIRCLE gives a circle or a stadium), as a convex polygon.
SwatchPolygon swatch_shape_polygon(const lv_area_t& area, int32_t radius);

/// Whether (@p x, @p y) lies inside @p area rounded by @p radius.
bool swatch_shape_contains(const lv_area_t& area, int32_t radius, float x, float y);

/// How much of the pixel centred on (@p x, @p y) the rounded shape covers, 0..1:
/// the anti-aliased rim the texture pixels follow.
float swatch_shape_coverage(const lv_area_t& area, int32_t radius, float x, float y);

/// Where the @p n colour bands of a multi-colour swatch meet: n - 1 values in
/// 0..1 along the top-left to bottom-right diagonal (0 at the top-left corner,
/// 1 at the bottom-right), chosen so every band covers an equal share of the
/// rounded shape. They are the stops of the linear gradient draw_swatch() paints.
std::vector<float> swatch_split_thresholds(const lv_area_t& area, int32_t radius, size_t n);

/// The @p n colour regions of a multi-colour swatch: the bands between
/// swatch_split_thresholds(), each clipped to the rounded shape.
/// Index k is colour k. n < 2 returns the whole shape as one region.
std::vector<SwatchPolygon> swatch_split_polygons(const lv_area_t& area, int32_t radius, size_t n);

/// Shoelace area of a polygon (absolute).
float swatch_polygon_area(const SwatchPolygon& poly);

/// @p count deterministic points inside the rounded shape, for marble speckle
/// and sparkle glints. The same @p seed always gives the same points.
std::vector<SwatchPoint> swatch_texture_points(const lv_area_t& area, int32_t radius, size_t count,
                                               uint32_t seed);

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

/**
 * What a swatch keeps between redraws: its finish texture, rendered once into
 * an image, and its split thresholds. Each is rebuilt only when its key (size,
 * radius, finish; size, radius, colour count) changes. Owned by the object the
 * swatch paints (or by a draw_swatch() caller); freed with it. Main thread only.
 */
class SwatchCache {
  public:
    SwatchCache() = default;
    ~SwatchCache();
    SwatchCache(const SwatchCache&) = delete;
    SwatchCache& operator=(const SwatchCache&) = delete;

    /// The finish texture for a @p w x @p h shape, rendered on a key change.
    const lv_draw_buf_t* texture(int32_t w, int32_t h, int32_t radius, SwatchFinish finish);
    /// The split thresholds for @p area, recomputed on a key change.
    const std::vector<float>& thresholds(const lv_area_t& area, int32_t radius, size_t n);
    /// How many times a texture has been rendered (tests).
    int texture_builds() const {
        return texture_builds_;
    }

  private:
    void drop_texture();
    lv_draw_buf_t* texture_ = nullptr;
    int32_t tex_w_ = 0, tex_h_ = 0, tex_radius_ = 0;
    SwatchFinish tex_finish_ = SwatchFinish::Plain;
    int texture_builds_ = 0;
    std::vector<float> thresholds_;
    int32_t thr_w_ = -1, thr_h_ = -1, thr_radius_ = 0;
    size_t thr_n_ = 0;
};

struct SwatchPaint {
    std::vector<lv_color_t> colors; ///< 1..N; N >= 2 draws the diagonal split
    /// N >= 2 colors blend left to right (a gradient filament) instead of
    /// meeting in hard bands.
    bool blend = false;
    const lv_grad_dsc_t* gradient = nullptr; ///< fills the shape instead of colors
    int32_t radius = LV_RADIUS_CIRCLE;
    bool edge = true; ///< apply the contrast edge rule
    /// The colour the swatch sits on, when known; else the theme surfaces.
    std::optional<uint32_t> backdrop;
    SwatchFinish finish = SwatchFinish::Plain;
    lv_opa_t opa = LV_OPA_COVER;
    /// Holds the finish texture between redraws. Without one a silk, marble or
    /// sparkle finish is not drawn (the colour and split still are).
    SwatchCache* cache = nullptr;
};

/// Paint @p paint over @p area into @p layer: one fill (or split), one blit of
/// the cached finish texture, one edge. Main (UI) thread only.
void draw_swatch(lv_layer_t* layer, const lv_area_t& area, const SwatchPaint& paint);

/// Blend @p finish's texture (silk highlight, marble flecks, sparkle glints)
/// over the pixels of @p area, rounded by @p radius, in the ARGB8888 @p buf,
/// with plain pixel writes. Nothing for matte or plain, or below the
/// kSwatchFinishMinPx area. The texture SwatchCache renders, and the one
/// spool_canvas lays over the filament it draws.
void swatch_paint_finish_pixels(lv_draw_buf_t* buf, const lv_area_t& area, int32_t radius,
                                SwatchFinish finish);

/// The same over any shape inside @p area: @p cover says which pixels are in
/// it, so a texture can follow an outline no rounded rect describes. Marble
/// keeps one density at every size here, so a large drawing is as flecked as a
/// small one.
void swatch_paint_finish_pixels(lv_draw_buf_t* buf, const lv_area_t& area,
                                const SwatchCoverage& cover, SwatchFinish finish);

/// The paint for a spool colour: its multi-colour list when it has two or more
/// parseable entries, else @p rgb.
SwatchPaint swatch_paint_for(uint32_t rgb, const std::string& multi_color_hexes,
                             SwatchFinish finish = SwatchFinish::Plain);

/// swatch_paint_for() a slot: its colour, multi-colour list and finish.
SwatchPaint swatch_paint_for(const helix::SlotInfo& slot);

// ---------------------------------------------------------------------------
// Object paint
// ---------------------------------------------------------------------------

/**
 * Paint an object (or a <color_swatch>'s disc) as a swatch of @p primary_rgb, or of @p
 * multi_color_hexes when that lists two or more colours, with @p finish.
 *
 * The object keeps its own size and radius; its background and border are
 * drawn by the swatch painter instead of the style. Safe to call repeatedly
 * (e.g. from an observer). Main (UI) thread only.
 */
void apply_swatch_color(lv_obj_t* swatch, uint32_t primary_rgb,
                        const std::string& multi_color_hexes,
                        SwatchFinish finish = SwatchFinish::Plain);

/// apply_swatch_color() from a slot's colour, multi-colour list and finish.
void apply_swatch_color(lv_obj_t* swatch, const helix::SlotInfo& slot);

/// Hang the swatch painter on @p obj with no fixed colour: it paints the
/// object's own bg_color (or bg_grad), so a subject-bound colour style keeps
/// working. Idempotent.
void attach_swatch_paint(lv_obj_t* obj);

/// Paint nothing on @p swatch (or a <color_swatch>'s disc) until it is given a
/// colour again: an empty slot. Its style border, if any, still draws.
void clear_swatch_color(lv_obj_t* swatch);

/// Whether @p obj currently paints through the swatch painter.
bool has_swatch_paint(const lv_obj_t* obj);

/// The colours an object's swatch paint holds (empty when it paints its
/// bg_color, or has no paint). For tests and click handlers.
std::vector<lv_color_t> swatch_paint_colors(const lv_obj_t* obj);

/// The finish an object's swatch paint holds.
SwatchFinish swatch_paint_finish(const lv_obj_t* obj);

/// The colour a swatch shows: @p obj's own paint, or the disc of a
/// <color_swatch> holder. Click handlers read a tapped swatch's colour here.
lv_color_t swatch_color_of(lv_obj_t* obj);

/// Register the <swatch_paint> XML widget. Attributes, all optional and
/// ignored when empty: color, bind_color (color subject), colors and
/// bind_colors (CSV / string subject), finish and bind_finish (finish name /
/// string subject holding a filament name), edge ("off" disables the edge),
/// blend ("on" blends several colors left to right instead of banding them).
void register_swatch_paint_widget();

/// Drop all retained swatch paint state. Normally unnecessary (state is
/// released when each object is deleted); exposed for test teardown.
void reset_swatch_state();

} // namespace helix::ui
