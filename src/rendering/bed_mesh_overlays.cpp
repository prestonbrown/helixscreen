// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_BED_MESH_3D

#include "bed_mesh_overlays.h"

#include "bed_grid.h"
#include "bed_mesh_buffer.h"
#include "bed_mesh_coordinate_transform.h"
#include "bed_mesh_internal.h"
#include "bed_mesh_projection.h"
#include "core/noto_sans_fonts.h"
#include "render_theme.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// ============================================================================
// Constants
// ============================================================================

namespace {

// Rendering opacity values
constexpr lv_opa_t kGridLineOpacity = LV_OPA_70;      // Wireframe over the mesh surface
constexpr lv_opa_t kReferenceGridOpacity = LV_OPA_60; // Floor and wall grids

// Visibility margin for partially visible geometry
constexpr int kVisibilityMarginPx = 10;

// Label positioning
constexpr double kZLabelAboveCeiling = 32.0; // Z label offset above ceiling

constexpr double kGridSpacingMm = helix::kBedGridSpacingMm;

// Number of segments for Z-axis grid divisions
constexpr int kZAxisSegmentCount = 5;

// Axis label offset from edge (world units)
constexpr double kAxisLabelOffset = 50.0; // Distance from grid edge to axis letter labels

// Small-canvas handling.
//
// The separation between the numeric tick labels (kTickLabelOutwardOffset, 20
// world units) and the X/Y axis letters (kAxisLabelOffset, 50) is expressed in
// WORLD units, so it shrinks with the canvas — while the labels themselves are
// fixed pixel boxes drawn in a fixed-size font. Below roughly 300px the 30 world
// units between them project to only a few pixels and the axis letter lands on
// top of the tick number: at 232x212 (the bed-mesh canvas at 480x272) "X" is
// drawn over "100" (prestonbrown/helixscreen#1204).
//
// Keyed on canvas size rather than the UI breakpoint on purpose: this canvas is
// flex-sized, so it can be small on a large display too, and render_axis_labels()
// is already handed canvas_width/canvas_height.
constexpr int kSmallCanvasMaxPx = 300;
constexpr double kAxisLabelOffsetSmall = 110.0; // Clears the tick row when projected small
constexpr int kAxisLabelHalfSizeSmall = 5;      // 5px half-size to match the 10px font

/// True when the canvas is too small for world-unit label separation to survive
/// projection. Uses the smaller dimension: the mesh is drawn isometrically, so a
/// short canvas crowds the labels just as much as a narrow one.
static bool is_small_canvas(int canvas_width, int canvas_height) {
    return std::min(canvas_width, canvas_height) < kSmallCanvasMaxPx;
}

// Tick label dimensions (pixels)
constexpr int kTickLabelWidthDecimal = 40; // Wider for decimal values (e.g., "-0.25")
constexpr int kTickLabelWidthInteger = 30; // Narrower for integers (e.g., "100")
constexpr int kTickLabelHeight = 12;

// Axis label dimensions
constexpr int kAxisLabelHalfSize = 7; // 7px half-size = 14px label area

/**
 * Check if point is visible on canvas (with margin for partially visible geometry)
 * @param x Screen X coordinate
 * @param y Screen Y coordinate
 * @param canvas_width Canvas width in pixels
 * @param canvas_height Canvas height in pixels
 * @param margin Pixel margin for partially visible objects
 * @return true if point is visible or partially visible
 */
static inline bool is_point_visible(int x, int y, int canvas_width, int canvas_height,
                                    int margin = kVisibilityMarginPx) {
    return x >= -margin && x < canvas_width + margin && y >= -margin && y < canvas_height + margin;
}

/**
 * Check if line segment is potentially visible on canvas
 * @return true if either endpoint is visible (line may be partially visible)
 */
static inline bool is_line_visible(int x1, int y1, int x2, int y2, int canvas_width,
                                   int canvas_height, int margin = kVisibilityMarginPx) {
    return is_point_visible(x1, y1, canvas_width, canvas_height, margin) ||
           is_point_visible(x2, y2, canvas_width, canvas_height, margin);
}

/**
 * Draw a single axis line from 3D start to 3D end point into a pixel buffer.
 * Projects coordinates to 2D screen space and renders the line.
 * No LVGL calls — safe for background threads.
 */
static void draw_axis_line_to_buffer(helix::mesh::PixelBuffer& buf, uint8_t r, uint8_t g, uint8_t b,
                                     uint8_t a, double start_x, double start_y, double start_z,
                                     double end_x, double end_y, double end_z, int canvas_width,
                                     int canvas_height, const bed_mesh_view_state_t* view_state) {
    bed_mesh_point_3d_t start = bed_mesh_projection_project_3d_to_2d(
        start_x, start_y, start_z, canvas_width, canvas_height, view_state);
    bed_mesh_point_3d_t end = bed_mesh_projection_project_3d_to_2d(
        end_x, end_y, end_z, canvas_width, canvas_height, view_state);

    buf.draw_line(start.screen_x, start.screen_y, end.screen_x, end.screen_y, r, g, b, a);
}

} // anonymous namespace

// ============================================================================
// Public API Implementation
// ============================================================================

namespace helix {
namespace mesh {

BedExtent compute_bed_extent(const bed_mesh_renderer_t* renderer) {
    BedExtent ext{};
    // geometry_computed is what makes bed_center/coord_scale meaningful; it is set
    // together with the bed bounds by bed_mesh_renderer_set_bounds().
    if (renderer->geometry_computed) {
        ext.x_min_mm = renderer->bed_min_x;
        ext.x_max_mm = renderer->bed_max_x;
        ext.y_min_mm = renderer->bed_min_y;
        ext.y_max_mm = renderer->bed_max_y;
        ext.center_x = renderer->bed_center_x;
        ext.center_y = renderer->bed_center_y;
        ext.coord_scale = renderer->coord_scale;
    } else {
        ext.x_min_mm = 0.0;
        ext.x_max_mm = (renderer->cols - 1) * BED_MESH_SCALE;
        ext.y_min_mm = 0.0;
        ext.y_max_mm = (renderer->rows - 1) * BED_MESH_SCALE;
        ext.center_x = ext.x_max_mm / 2.0;
        ext.center_y = ext.y_max_mm / 2.0;
        ext.coord_scale = 1.0;
    }
    ext.half_width = (ext.x_max_mm - ext.x_min_mm) / 2.0 * ext.coord_scale;
    ext.half_height = (ext.y_max_mm - ext.y_min_mm) / 2.0 * ext.coord_scale;

    double z_min_world = mesh_z_to_world_z(renderer->mesh_min_z, renderer->cached_z_center,
                                           renderer->view_state.z_scale);
    double z_max_world = mesh_z_to_world_z(renderer->mesh_max_z, renderer->cached_z_center,
                                           renderer->view_state.z_scale);
    ext.walls = compute_wall_bounds(z_min_world, z_max_world, ext.half_width, ext.half_height);
    ext.circular = renderer->circular_bed;
    return ext;
}

ReferenceGridFrame reference_grid_frame(const BedExtent& ext) {
    ReferenceGridFrame f{};
    f.line_x_first = std::ceil(ext.x_min_mm / kGridSpacingMm) * kGridSpacingMm;
    f.line_x_last = std::floor(ext.x_max_mm / kGridSpacingMm) * kGridSpacingMm;
    f.line_y_first = std::ceil(ext.y_min_mm / kGridSpacingMm) * kGridSpacingMm;
    f.line_y_last = std::floor(ext.y_max_mm / kGridSpacingMm) * kGridSpacingMm;
    if (ext.circular) {
        f.edge_x_min = ext.x_min_mm;
        f.edge_x_max = ext.x_max_mm;
        f.edge_y_min = ext.y_min_mm;
        f.edge_y_max = ext.y_max_mm;
    } else {
        f.edge_x_min = f.line_x_first;
        f.edge_x_max = f.line_x_last;
        f.edge_y_min = f.line_y_first;
        f.edge_y_max = f.line_y_last;
    }
    return f;
}

HeatmapLayout compute_heatmap_layout(int rows, int cols, int canvas_width, int canvas_height) {
    constexpr int PADDING = 8;
    HeatmapLayout l{};
    l.cells_x = cols - 1;
    l.cells_y = rows - 1;
    if (l.cells_x <= 0 || l.cells_y <= 0) {
        return l;
    }
    int grid_width = canvas_width - 2 * PADDING;
    int grid_height = canvas_height - 2 * PADDING;
    l.cell_w = std::max(1, grid_width / l.cells_x);
    l.cell_h = std::max(1, grid_height / l.cells_y);
    l.grid_x = PADDING + (grid_width - l.cell_w * l.cells_x) / 2;
    l.grid_y = PADDING + (grid_height - l.cell_h * l.cells_y) / 2;
    l.valid = true;
    return l;
}

void render_heatmap_overlay(lv_layer_t* layer, const bed_mesh_renderer_t* renderer,
                            const HeatmapLayout& l, int offset_x, int offset_y) {
    if (!renderer || !l.valid) {
        return;
    }
    const int grid_x = offset_x + l.grid_x;
    const int grid_y = offset_y + l.grid_y;

    // Subtle border around the entire grid
    lv_draw_rect_dsc_t border_dsc;
    lv_draw_rect_dsc_init(&border_dsc);
    border_dsc.bg_opa = LV_OPA_TRANSP;
    border_dsc.border_color = helix::render_theme::color("elevated_bg");
    border_dsc.border_width = 1;
    border_dsc.border_opa = LV_OPA_60;
    border_dsc.radius = 2;

    lv_area_t border_area;
    border_area.x1 = grid_x - 1;
    border_area.y1 = grid_y - 1;
    border_area.x2 = grid_x + l.cells_x * l.cell_w + 1;
    border_area.y2 = grid_y + l.cells_y * l.cell_h + 1;
    lv_draw_rect(layer, &border_dsc, &border_area);

    if (!renderer->touch_valid) {
        return;
    }

    // Highlight the touched mesh cell
    lv_draw_rect_dsc_t highlight_dsc;
    lv_draw_rect_dsc_init(&highlight_dsc);
    highlight_dsc.bg_opa = LV_OPA_20;
    highlight_dsc.bg_color = lv_color_white();
    highlight_dsc.border_color = lv_color_white();
    highlight_dsc.border_width = 2;
    highlight_dsc.border_opa = LV_OPA_COVER;
    highlight_dsc.radius = 2;

    lv_area_t highlight_area;
    highlight_area.x1 = grid_x + renderer->touched_col * l.cell_w;
    highlight_area.y1 = grid_y + renderer->touched_row * l.cell_h;
    highlight_area.x2 = highlight_area.x1 + l.cell_w - 1;
    highlight_area.y2 = highlight_area.y1 + l.cell_h - 1;
    lv_draw_rect(layer, &highlight_dsc, &highlight_area);

    // Z value tooltip, with the display offset added back to show the original probe height
    char z_text[32];
    snprintf(z_text, sizeof(z_text), "%.3f mm",
             static_cast<double>(renderer->touched_z) + renderer->z_display_offset);

    // Above the cell, or below it when the cell is near the top
    int tooltip_x = highlight_area.x1 + l.cell_w / 2 - 30;
    int tooltip_y = highlight_area.y1 - 24;
    if (tooltip_y < offset_y + 5) {
        tooltip_y = highlight_area.y2 + 5;
    }

    lv_draw_rect_dsc_t tooltip_bg;
    lv_draw_rect_dsc_init(&tooltip_bg);
    tooltip_bg.bg_color = helix::render_theme::color("card_bg");
    tooltip_bg.bg_opa = LV_OPA_90;
    tooltip_bg.radius = 6;
    tooltip_bg.border_color = helix::render_theme::color("elevated_bg");
    tooltip_bg.border_width = 1;
    tooltip_bg.border_opa = LV_OPA_60;

    lv_area_t tooltip_area = {tooltip_x - 8, tooltip_y - 4, tooltip_x + 68, tooltip_y + 18};
    lv_draw_rect(layer, &tooltip_bg, &tooltip_area);

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = helix::render_theme::color("text");
    label_dsc.font = &noto_sans_14;
    label_dsc.text = z_text;
    label_dsc.text_local = 1; // z_text is a stack buffer; the draw is deferred
    label_dsc.align = LV_TEXT_ALIGN_CENTER;

    // Full box width: a negative value plus " mm" is wider than the box's inner 60px
    lv_area_t label_area = {tooltip_area.x1, tooltip_y, tooltip_area.x2, tooltip_y + 14};
    lv_draw_label(layer, &label_dsc, &label_area);
}

void render_axis_labels(lv_layer_t* layer, const bed_mesh_renderer_t* renderer, int canvas_width,
                        int canvas_height) {
    if (!renderer || !renderer->has_mesh_data) {
        return;
    }

    const BedExtent ext = compute_bed_extent(renderer);
    const ReferenceGridFrame frame = reference_grid_frame(ext);

    // Wall positions in world coordinates
    double x_min_world = printer_x_to_world_x(frame.edge_x_min, ext.center_x, ext.coord_scale);
    double x_max_world = printer_x_to_world_x(frame.edge_x_max, ext.center_x, ext.coord_scale);
    double y_max_world =
        printer_y_to_world_y(frame.edge_y_min, ext.center_y, ext.coord_scale); // Y inverted
    double floor_z = ext.walls.floor_z;

    // Configure label drawing style. On a small canvas the letters drop to the
    // same 10px font the tick numbers use and are pushed further out, so the
    // projected gap between the two rows stays larger than the glyphs.
    const bool small_canvas = is_small_canvas(canvas_width, canvas_height);
    const double axis_label_offset = small_canvas ? kAxisLabelOffsetSmall : kAxisLabelOffset;
    const int axis_label_half = small_canvas ? kAxisLabelHalfSizeSmall : kAxisLabelHalfSize;

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = helix::render_theme::color("text");
    label_dsc.font = small_canvas ? &noto_sans_10 : &noto_sans_14;
    label_dsc.opa = LV_OPA_90;
    label_dsc.align = LV_TEXT_ALIGN_CENTER;

    // X label: At the CENTER of the front edge, pushed OUTWARD (away from grid)
    // Positioned beyond the tick labels in the Y direction
    double x_label_x = 0.0;                             // Center of X axis
    double x_label_y = y_max_world + axis_label_offset; // Pushed outward from front edge
    double x_label_z = floor_z;                         // At floor grid level (base of walls)
    bed_mesh_point_3d_t x_pos = bed_mesh_projection_project_3d_to_2d(
        x_label_x, x_label_y, x_label_z, canvas_width, canvas_height, &renderer->view_state);

    // X label - let LVGL handle clipping
    {
        label_dsc.text = "X";
        lv_area_t x_area;
        x_area.x1 = x_pos.screen_x - axis_label_half;
        x_area.y1 = x_pos.screen_y - axis_label_half;
        x_area.x2 = x_area.x1 + 2 * axis_label_half;
        x_area.y2 = x_area.y1 + 2 * axis_label_half;
        lv_draw_label(layer, &label_dsc, &x_area);
    }

    // Y label: At the CENTER of the right edge, pushed OUTWARD (away from grid)
    // Positioned beyond the tick labels in the X direction
    double y_label_x = x_max_world + axis_label_offset; // Pushed outward from right edge
    double y_label_y = 0.0;                             // Center of Y axis
    double y_label_z = floor_z;                         // At floor grid level (base of walls)
    bed_mesh_point_3d_t y_pos = bed_mesh_projection_project_3d_to_2d(
        y_label_x, y_label_y, y_label_z, canvas_width, canvas_height, &renderer->view_state);

    // Y label - let LVGL handle clipping
    {
        label_dsc.text = "Y";
        lv_area_t y_area;
        y_area.x1 = y_pos.screen_x - axis_label_half;
        y_area.y1 = y_pos.screen_y - axis_label_half;
        y_area.x2 = y_area.x1 + 2 * axis_label_half;
        y_area.y2 = y_area.y1 + 2 * axis_label_half;
        lv_draw_label(layer, &label_dsc, &y_area);
    }

    // Z label: At the top of Z axis, ABOVE the wall ceiling where tick labels end
    // Position at front-left corner (grid-aligned)
    double z_axis_top =
        ext.walls.ceiling_z + kZLabelAboveCeiling; // Position above the highest tick
    bed_mesh_point_3d_t z_pos = bed_mesh_projection_project_3d_to_2d(
        x_min_world, y_max_world, z_axis_top, canvas_width, canvas_height, &renderer->view_state);

    // Z label - let LVGL handle clipping
    {
        label_dsc.text = "Z";
        lv_area_t z_area;
        z_area.x1 = z_pos.screen_x - axis_label_half - 5; // Offset left of the axis
        z_area.y1 = z_pos.screen_y - axis_label_half;
        z_area.x2 = z_area.x1 + 2 * axis_label_half;
        z_area.y2 = z_area.y1 + 2 * axis_label_half;
        lv_draw_label(layer, &label_dsc, &z_area);
    }
}

void draw_axis_tick_label(lv_layer_t* layer, lv_draw_label_dsc_t* label_dsc, int screen_x,
                          int screen_y, int offset_x, int offset_y, double value,
                          [[maybe_unused]] int canvas_width, [[maybe_unused]] int canvas_height,
                          bool use_decimals) {
    // Let LVGL handle clipping via the layer's clip area
    // (screen coordinates include layer_offset so manual bounds check would be wrong)

    // Format label text (use decimal format for Z-axis heights)
    // Handle -0 by treating small negative values as 0
    double display_value = (std::fabs(value) < 0.005) ? 0.0 : value;
    char label_text[12];
    if (use_decimals) {
        snprintf(label_text, sizeof(label_text), "%.2f", display_value);
    } else {
        snprintf(label_text, sizeof(label_text), "%.0f", display_value);
    }
    label_dsc->text = label_text;
    label_dsc->text_length = static_cast<uint32_t>(strlen(label_text));

    // Calculate label area with offsets (wider for decimal values)
    lv_area_t label_area;
    label_area.x1 = screen_x + offset_x;
    label_area.y1 = screen_y + offset_y;
    label_area.x2 =
        label_area.x1 + (use_decimals ? kTickLabelWidthDecimal : kTickLabelWidthInteger);
    label_area.y2 = label_area.y1 + kTickLabelHeight;

    // Let LVGL handle clipping via the layer's clip area
    lv_draw_label(layer, label_dsc, &label_area);
}

void render_numeric_axis_ticks(lv_layer_t* layer, const bed_mesh_renderer_t* renderer,
                               int canvas_width, int canvas_height) {
    if (!renderer || !renderer->has_mesh_data) {
        return;
    }

    const BedExtent ext = compute_bed_extent(renderer);
    const double bed_center_x = ext.center_x;
    const double bed_center_y = ext.center_y;
    const double coord_scale = ext.coord_scale;
    const WallBounds& bounds = ext.walls;

    const ReferenceGridFrame frame = reference_grid_frame(ext);
    const double x_grid_start = frame.line_x_first;
    const double x_grid_end = frame.line_x_last;
    const double y_grid_start = frame.line_y_first;
    const double y_grid_end = frame.line_y_last;

    // Wall positions in world coordinates
    double x_min_world =
        helix::mesh::printer_x_to_world_x(frame.edge_x_min, bed_center_x, coord_scale);
    double x_max_world =
        helix::mesh::printer_x_to_world_x(frame.edge_x_max, bed_center_x, coord_scale);
    double y_max_world = helix::mesh::printer_y_to_world_y(frame.edge_y_min, bed_center_y,
                                                           coord_scale); // Y inverted
    double floor_z = bounds.floor_z;

    // Configure label drawing style (smaller font than axis letters)
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = helix::render_theme::color("text");
    label_dsc.font = &noto_sans_10; // Smaller font for numeric labels
    label_dsc.opa = LV_OPA_80;      // Slightly more transparent than axis letters
    label_dsc.align = LV_TEXT_ALIGN_CENTER;
    label_dsc.text_local = 1; // Tell LVGL to copy text (we use stack buffers)

    // Use same spacing as grid lines (50mm) for tick labels
    double tick_spacing = kGridSpacingMm;

    // World-space offset to push tick labels OUTWARD from grid edges (prevents overlap)
    constexpr double kTickLabelOutwardOffset = 20.0; // World units away from grid edge

    // X-axis tick label offsets: at front edge (at floor level, centered beneath tick)
    constexpr int kXLabelOffsetX = -15; // Center label beneath tick
    constexpr int kXLabelOffsetY = 0;   // At floor level
    // Y-axis tick label offsets: at right edge (same pattern as X)
    constexpr int kYLabelOffsetX = -15; // Center label beneath tick
    constexpr int kYLabelOffsetY = 0;   // At floor level
    // Z-axis tick label offsets: to the left of the axis line (closer to wall)
    constexpr int kZLabelOffsetX = -38;
    constexpr int kZLabelOffsetY = -6;

    // Draw X-axis tick labels along FRONT edge of the grid, pushed outward
    // Only label every other tick (0, 100, 200... not 50, 150, 250) to reduce crowding
    int x_tick_index = 0;
    for (double x_mm = x_grid_start; x_mm <= x_grid_end + 0.001; x_mm += tick_spacing) {
        if (x_tick_index % 2 == 0) {
            double x_world = helix::mesh::printer_x_to_world_x(x_mm, bed_center_x, coord_scale);
            // Push outward from front edge (+Y direction in world space)
            bed_mesh_point_3d_t tick = bed_mesh_projection_project_3d_to_2d(
                x_world, y_max_world + kTickLabelOutwardOffset, floor_z, canvas_width,
                canvas_height, &renderer->view_state);
            draw_axis_tick_label(layer, &label_dsc, tick.screen_x, tick.screen_y, kXLabelOffsetX,
                                 kXLabelOffsetY, x_mm, canvas_width, canvas_height);
        }
        x_tick_index++;
    }

    // Draw Y-axis tick labels along RIGHT edge of the grid, pushed outward
    // Only label every other tick to reduce crowding
    int y_tick_index = 0;
    for (double y_mm = y_grid_start; y_mm <= y_grid_end + 0.001; y_mm += tick_spacing) {
        if (y_tick_index % 2 == 0) {
            double y_world = helix::mesh::printer_y_to_world_y(y_mm, bed_center_y, coord_scale);
            // Push outward from right edge (+X direction in world space)
            bed_mesh_point_3d_t tick = bed_mesh_projection_project_3d_to_2d(
                x_max_world + kTickLabelOutwardOffset, y_world, floor_z, canvas_width,
                canvas_height, &renderer->view_state);
            draw_axis_tick_label(layer, &label_dsc, tick.screen_x, tick.screen_y, kYLabelOffsetX,
                                 kYLabelOffsetY, y_mm, canvas_width, canvas_height);
        }
        y_tick_index++;
    }

    // Draw Z-axis tick labels on the LEFT WALL at front-left corner
    // Use x_min_world and y_max_world (grid-aligned positions)

    // Generate 3 evenly-spaced Z labels along the wall (reduced from 5 to avoid crowding)
    constexpr int kNumZLabels = 3;
    for (int i = 0; i < kNumZLabels; i++) {
        double t = static_cast<double>(i) / (kNumZLabels - 1);
        double z_world = bounds.floor_z + t * (bounds.ceiling_z - bounds.floor_z);

        // Convert world Z back to mesh Z for display, adding back the normalization
        // offset so labels show original probe heights (not mean-subtracted values)
        double z_mm = helix::mesh::world_z_to_mesh_z(z_world, renderer->cached_z_center,
                                                     renderer->view_state.z_scale) +
                      renderer->z_display_offset;

        bed_mesh_point_3d_t tick = bed_mesh_projection_project_3d_to_2d(
            x_min_world, y_max_world, z_world, canvas_width, canvas_height, &renderer->view_state);
        draw_axis_tick_label(layer, &label_dsc, tick.screen_x, tick.screen_y, kZLabelOffsetX,
                             kZLabelOffsetY, z_mm, canvas_width, canvas_height, true);
    }
}

// ============================================================================
// Pixel-buffer drawing (no LVGL calls - runs on the render thread)
// ============================================================================

void render_grid_lines(PixelBuffer& buf, const bed_mesh_renderer_t* renderer, int canvas_width,
                       int canvas_height, uint8_t line_r, uint8_t line_g, uint8_t line_b) {
    if (!renderer || !renderer->has_mesh_data) {
        return;
    }

    constexpr uint8_t grid_alpha = kGridLineOpacity;

    // Use cached projected screen coordinates (SOA arrays)
    const auto& screen_x = renderer->projected_screen_x;
    const auto& screen_y = renderer->projected_screen_y;

    // A round bed's surface stops at the circle inscribed in the mesh area, so
    // its wireframe does too.
    const bool round = renderer->circular_bed && renderer->geometry_computed;
    const RoundBed circle =
        inscribed_round_bed(renderer->mesh_area_min_x, renderer->mesh_area_max_x,
                            renderer->mesh_area_min_y, renderer->mesh_area_max_y);
    auto printer_xy = [&](int row, int col) {
        return BedPoint{renderer->mesh_area_min_x +
                            col * (renderer->mesh_area_max_x - renderer->mesh_area_min_x) /
                                (renderer->cols - 1),
                        renderer->mesh_area_min_y +
                            row * (renderer->mesh_area_max_y - renderer->mesh_area_min_y) /
                                (renderer->rows - 1)};
    };

    auto draw_segment = [&](int r1, int c1, int r2, int c2) {
        int p1_x = screen_x[static_cast<size_t>(r1)][static_cast<size_t>(c1)];
        int p1_y = screen_y[static_cast<size_t>(r1)][static_cast<size_t>(c1)];
        int p2_x = screen_x[static_cast<size_t>(r2)][static_cast<size_t>(c2)];
        int p2_y = screen_y[static_cast<size_t>(r2)][static_cast<size_t>(c2)];
        if (round) {
            const BedPoint a = printer_xy(r1, c1);
            const BedPoint b = printer_xy(r2, c2);
            const auto t = segment_in_round_bed(a, b, circle);
            if (!t) {
                return;
            }
            if (t->first > 0.0 || t->second < 1.0) {
                const double za = renderer->mesh[static_cast<size_t>(r1)][static_cast<size_t>(c1)];
                const double zb = renderer->mesh[static_cast<size_t>(r2)][static_cast<size_t>(c2)];
                auto project_at = [&](double s) {
                    return bed_mesh_projection_project_3d_to_2d(
                        printer_x_to_world_x(a.x + s * (b.x - a.x), renderer->bed_center_x,
                                             renderer->coord_scale),
                        printer_y_to_world_y(a.y + s * (b.y - a.y), renderer->bed_center_y,
                                             renderer->coord_scale),
                        mesh_z_to_world_z(za + s * (zb - za), renderer->cached_z_center,
                                          renderer->view_state.z_scale),
                        canvas_width, canvas_height, &renderer->view_state);
                };
                const auto q1 = project_at(t->first);
                const auto q2 = project_at(t->second);
                p1_x = q1.screen_x;
                p1_y = q1.screen_y;
                p2_x = q2.screen_x;
                p2_y = q2.screen_y;
            }
        }
        if (is_line_visible(p1_x, p1_y, p2_x, p2_y, canvas_width, canvas_height)) {
            buf.draw_line(p1_x, p1_y, p2_x, p2_y, line_r, line_g, line_b, grid_alpha);
        }
    };

    // Horizontal grid lines (connect points in same row)
    for (int row = 0; row < renderer->rows; row++) {
        for (int col = 0; col < renderer->cols - 1; col++) {
            draw_segment(row, col, row, col + 1);
        }
    }

    // Vertical grid lines (connect points in same column)
    for (int col = 0; col < renderer->cols; col++) {
        for (int row = 0; row < renderer->rows - 1; row++) {
            draw_segment(row, col, row + 1, col);
        }
    }
}

void render_reference_grids(PixelBuffer& buf, const bed_mesh_renderer_t* renderer, int canvas_width,
                            int canvas_height, uint8_t line_r, uint8_t line_g, uint8_t line_b) {
    if (!renderer || !renderer->has_mesh_data) {
        return;
    }

    constexpr uint8_t ref_alpha = kReferenceGridOpacity;

    // Printer-bed extent, not mesh extent: the mesh floats inside the walls
    const BedExtent ext = compute_bed_extent(renderer);
    const ReferenceGridFrame frame = reference_grid_frame(ext);
    const double bed_center_x = ext.center_x;
    const double bed_center_y = ext.center_y;
    const double coord_scale = ext.coord_scale;
    const double x_grid_start = frame.line_x_first;
    const double x_grid_end = frame.line_x_last;
    const double y_grid_start = frame.line_y_first;
    const double y_grid_end = frame.line_y_last;

    // Wall positions in world coordinates
    double x_min = printer_x_to_world_x(frame.edge_x_min, bed_center_x, coord_scale);
    double x_max = printer_x_to_world_x(frame.edge_x_max, bed_center_x, coord_scale);
    double y_min = printer_y_to_world_y(frame.edge_y_max, bed_center_y, coord_scale); // Y inverted
    double y_max = printer_y_to_world_y(frame.edge_y_min, bed_center_y, coord_scale); // Y inverted
    double z_floor = ext.walls.floor_z;
    double z_ceiling = ext.walls.ceiling_z;

    auto floor_line = [&](double x1_mm, double y1_mm, double x2_mm, double y2_mm) {
        draw_axis_line_to_buffer(buf, line_r, line_g, line_b, ref_alpha,
                                 printer_x_to_world_x(x1_mm, bed_center_x, coord_scale),
                                 printer_y_to_world_y(y1_mm, bed_center_y, coord_scale), z_floor,
                                 printer_x_to_world_x(x2_mm, bed_center_x, coord_scale),
                                 printer_y_to_world_y(y2_mm, bed_center_y, coord_scale), z_floor,
                                 canvas_width, canvas_height, &renderer->view_state);
    };

    // ========== 1. BOTTOM GRID (XY plane at Z=z_floor) ==========
    // Outline: the floor rectangle, or the circle inscribed in the bed.
    for (const auto& seg :
         helix::bed_outline_segments(frame.edge_x_min, frame.edge_x_max, frame.edge_y_min,
                                     frame.edge_y_max, ext.circular)) {
        floor_line(seg.x1, seg.y1, seg.x2, seg.y2);
    }
    for (const auto& seg :
         helix::bed_grid_lines(frame.edge_x_min, frame.edge_x_max, frame.edge_y_min,
                               frame.edge_y_max, kGridSpacingMm, ext.circular)) {
        floor_line(seg.x1, seg.y1, seg.x2, seg.y2);
    }

    // ========== 2. BACK WALL GRID (XZ plane at Y=y_min) ==========
    for (double x_mm = x_grid_start; x_mm <= x_grid_end + 0.001; x_mm += kGridSpacingMm) {
        double x_world = helix::mesh::printer_x_to_world_x(x_mm, bed_center_x, coord_scale);
        draw_axis_line_to_buffer(buf, line_r, line_g, line_b, ref_alpha, x_world, y_min, z_floor,
                                 x_world, y_min, z_ceiling, canvas_width, canvas_height,
                                 &renderer->view_state);
    }
    double wall_z_range = z_ceiling - z_floor;
    double wall_z_spacing = wall_z_range / kZAxisSegmentCount;
    if (wall_z_spacing < 0.5)
        wall_z_spacing = wall_z_range / 3.0;
    for (double z = z_floor; z <= z_ceiling + 0.01; z += wall_z_spacing) {
        draw_axis_line_to_buffer(buf, line_r, line_g, line_b, ref_alpha, x_min, y_min, z, x_max,
                                 y_min, z, canvas_width, canvas_height, &renderer->view_state);
    }

    // ========== 3. LEFT WALL GRID (YZ plane at X=x_min) ==========
    for (double y_mm = y_grid_start; y_mm <= y_grid_end + 0.001; y_mm += kGridSpacingMm) {
        double y_world = helix::mesh::printer_y_to_world_y(y_mm, bed_center_y, coord_scale);
        draw_axis_line_to_buffer(buf, line_r, line_g, line_b, ref_alpha, x_min, y_world, z_floor,
                                 x_min, y_world, z_ceiling, canvas_width, canvas_height,
                                 &renderer->view_state);
    }
    for (double z = z_floor; z <= z_ceiling + 0.01; z += wall_z_spacing) {
        draw_axis_line_to_buffer(buf, line_r, line_g, line_b, ref_alpha, x_min, y_min, z, x_min,
                                 y_max, z, canvas_width, canvas_height, &renderer->view_state);
    }
}

} // namespace mesh
} // namespace helix

#endif // HELIX_HAS_BED_MESH_3D
