// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_BED_MESH_3D

#include "bed_mesh_geometry.h"

#include "bed_mesh_coordinate_transform.h"
#include "bed_mesh_gradient.h"
#include "bed_mesh_internal.h"
#include "memory_monitor.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace helix {
namespace mesh {

void generate_mesh_quads(bed_mesh_renderer_t* renderer) {
    if (!renderer || !renderer->has_mesh_data) {
        return;
    }

    renderer->quads.clear();

    // Pre-allocate capacity to avoid reallocations during generation
    // Number of quads = (rows-1) × (cols-1)
    int expected_quads = (renderer->rows - 1) * (renderer->cols - 1);
    renderer->quads.reserve(static_cast<size_t>(expected_quads));
    helix::MemoryMonitor::log_now("bed_mesh_quads_reserved");

    // Use cached z_center (computed once in compute_mesh_bounds)

    // Generate quads for each mesh cell
    for (int row = 0; row < renderer->rows - 1; row++) {
        for (int col = 0; col < renderer->cols - 1; col++) {
            bed_mesh_quad_3d_t quad;

            // Compute base X,Y positions
            double base_x_0, base_x_1, base_y_0, base_y_1;

            if (renderer->geometry_computed) {
                // Mainsail-style: Position mesh within bed using mesh_area bounds
                // Interpolate printer coordinates from mesh indices
                double cols_minus_1 = static_cast<double>(renderer->cols - 1);
                double rows_minus_1 = static_cast<double>(renderer->rows - 1);

                double printer_x0 =
                    renderer->mesh_area_min_x +
                    col / cols_minus_1 * (renderer->mesh_area_max_x - renderer->mesh_area_min_x);
                double printer_x1 = renderer->mesh_area_min_x +
                                    (col + 1) / cols_minus_1 *
                                        (renderer->mesh_area_max_x - renderer->mesh_area_min_x);
                double printer_y0 =
                    renderer->mesh_area_min_y +
                    row / rows_minus_1 * (renderer->mesh_area_max_y - renderer->mesh_area_min_y);
                double printer_y1 = renderer->mesh_area_min_y +
                                    (row + 1) / rows_minus_1 *
                                        (renderer->mesh_area_max_y - renderer->mesh_area_min_y);

                // Convert printer coordinates to world space
                base_x_0 = helix::mesh::printer_x_to_world_x(printer_x0, renderer->bed_center_x,
                                                             renderer->coord_scale);
                base_x_1 = helix::mesh::printer_x_to_world_x(printer_x1, renderer->bed_center_x,
                                                             renderer->coord_scale);
                base_y_0 = helix::mesh::printer_y_to_world_y(printer_y0, renderer->bed_center_y,
                                                             renderer->coord_scale);
                base_y_1 = helix::mesh::printer_y_to_world_y(printer_y1, renderer->bed_center_y,
                                                             renderer->coord_scale);
            } else {
                // Legacy: Index-based coordinates (centered around origin)
                // Note: Y is inverted because mesh[0] = front edge
                base_x_0 = helix::mesh::mesh_col_to_world_x(col, renderer->cols, BED_MESH_SCALE);
                base_x_1 =
                    helix::mesh::mesh_col_to_world_x(col + 1, renderer->cols, BED_MESH_SCALE);
                base_y_0 = helix::mesh::mesh_row_to_world_y(row, renderer->rows, BED_MESH_SCALE);
                base_y_1 =
                    helix::mesh::mesh_row_to_world_y(row + 1, renderer->rows, BED_MESH_SCALE);
            }

            /**
             * Quad vertex layout (view from above, looking down -Z axis):
             *
             *   mesh[row][col]         mesh[row][col+1]
             *        [2]TL ──────────────── [3]TR
             *         │                      │
             *         │                      │
             *         │       QUAD           │     ← One mesh cell
             *         │     (row,col)        │
             *         │                      │
             *        [0]BL ──────────────── [1]BR
             *   mesh[row+1][col]       mesh[row+1][col+1]
             *
             * Vertex indices: [0]=BL, [1]=BR, [2]=TL, [3]=TR
             * Mesh mapping:   [0]=mesh[row+1][col], [1]=mesh[row+1][col+1],
             *                 [2]=mesh[row][col],    [3]=mesh[row][col+1]
             *
             * Split into triangles for rasterization:
             *   Triangle 1: [0]→[1]→[2] (BL→BR→TL, lower-right triangle)
             *   Triangle 2: [1]→[3]→[2] (BR→TR→TL, upper-left triangle)
             *
             * Winding order: Counter-clockwise (CCW) for front-facing
             */
            quad.vertices[0].x = base_x_0;
            quad.vertices[0].y = base_y_1;
            quad.vertices[0].z = helix::mesh::mesh_z_to_world_z(
                renderer->mesh[static_cast<size_t>(row + 1)][static_cast<size_t>(col)],
                renderer->cached_z_center, renderer->view_state.z_scale);
            quad.vertices[0].color = bed_mesh_gradient_height_to_color(
                renderer->mesh[static_cast<size_t>(row + 1)][static_cast<size_t>(col)],
                renderer->color_min_z, renderer->color_max_z);

            quad.vertices[1].x = base_x_1;
            quad.vertices[1].y = base_y_1;
            quad.vertices[1].z = helix::mesh::mesh_z_to_world_z(
                renderer->mesh[static_cast<size_t>(row + 1)][static_cast<size_t>(col + 1)],
                renderer->cached_z_center, renderer->view_state.z_scale);
            quad.vertices[1].color = bed_mesh_gradient_height_to_color(
                renderer->mesh[static_cast<size_t>(row + 1)][static_cast<size_t>(col + 1)],
                renderer->color_min_z, renderer->color_max_z);

            quad.vertices[2].x = base_x_0;
            quad.vertices[2].y = base_y_0;
            quad.vertices[2].z = helix::mesh::mesh_z_to_world_z(
                renderer->mesh[static_cast<size_t>(row)][static_cast<size_t>(col)],
                renderer->cached_z_center, renderer->view_state.z_scale);
            quad.vertices[2].color = bed_mesh_gradient_height_to_color(
                renderer->mesh[static_cast<size_t>(row)][static_cast<size_t>(col)],
                renderer->color_min_z, renderer->color_max_z);

            quad.vertices[3].x = base_x_1;
            quad.vertices[3].y = base_y_0;
            quad.vertices[3].z = helix::mesh::mesh_z_to_world_z(
                renderer->mesh[static_cast<size_t>(row)][static_cast<size_t>(col + 1)],
                renderer->cached_z_center, renderer->view_state.z_scale);
            quad.vertices[3].color = bed_mesh_gradient_height_to_color(
                renderer->mesh[static_cast<size_t>(row)][static_cast<size_t>(col + 1)],
                renderer->color_min_z, renderer->color_max_z);

            // Compute center color for fast rendering
            bed_mesh_rgb_t avg_color = {
                static_cast<uint8_t>((quad.vertices[0].color.red + quad.vertices[1].color.red +
                                      quad.vertices[2].color.red + quad.vertices[3].color.red) /
                                     4),
                static_cast<uint8_t>((quad.vertices[0].color.green + quad.vertices[1].color.green +
                                      quad.vertices[2].color.green + quad.vertices[3].color.green) /
                                     4),
                static_cast<uint8_t>((quad.vertices[0].color.blue + quad.vertices[1].color.blue +
                                      quad.vertices[2].color.blue + quad.vertices[3].color.blue) /
                                     4)};
            quad.center_color = lv_color_make(avg_color.r, avg_color.g, avg_color.b);

            quad.avg_depth = 0.0;        // Will be computed during projection
            quad.opacity = LV_OPA_COVER; // Mesh quads are fully opaque

            renderer->quads.push_back(quad);
        }
    }

    size_t mesh_quad_count = renderer->quads.size();

    // DEBUG: Log quad generation with z_scale used
    spdlog::debug("[QUAD_GEN] Generated {} mesh quads, z_scale={:.2f}, z_center={:.4f}",
                  mesh_quad_count, renderer->view_state.z_scale, renderer->cached_z_center);
    // Log a sample quad to verify Z values
    if (!renderer->quads.empty()) {
        int center_row = (renderer->rows - 1) / 2;
        int center_col = (renderer->cols - 1) / 2;
        size_t center_quad_idx =
            static_cast<size_t>(center_row * (renderer->cols - 1) + center_col);
        if (center_quad_idx < renderer->quads.size()) {
            const auto& q = renderer->quads[center_quad_idx];
            spdlog::debug(
                "[QUAD_GEN] Center quad[{}] TL world_z={:.2f}, from mesh_z={:.4f}", center_quad_idx,
                q.vertices[2].z,
                renderer->mesh[static_cast<size_t>(center_row)][static_cast<size_t>(center_col)]);
        }
    }

    // ========== Generate Zero Plane Quads ==========
    // Translucent reference plane at Z=0 (or Z-offset) showing where nozzle touches bed
    // The plane covers the FULL BED area (not just the mesh probe area)
    if (renderer->show_zero_plane) {
        // Calculate world Z coordinate for the zero plane
        // zero_plane_z_offset is in mesh coordinates (mm), convert to world Z
        double plane_world_z = helix::mesh::mesh_z_to_world_z(
            renderer->zero_plane_z_offset, renderer->cached_z_center, renderer->view_state.z_scale);

        // Zero plane color: subtle neutral gray
        lv_color_t plane_color = lv_color_make(160, 160, 170);

        // Determine plane bounds and grid spacing
        double plane_min_x, plane_max_x, plane_min_y, plane_max_y;
        double grid_spacing_x, grid_spacing_y;
        int plane_cols, plane_rows;

        if (renderer->geometry_computed && renderer->has_bed_bounds) {
            // Use FULL BED bounds for the zero plane
            plane_min_x = renderer->bed_min_x;
            plane_max_x = renderer->bed_max_x;
            plane_min_y = renderer->bed_min_y;
            plane_max_y = renderer->bed_max_y;

            // Use similar grid density as the mesh for good depth interleaving
            // Calculate approximate cell size from mesh, then apply to bed
            double mesh_cell_x =
                (renderer->mesh_area_max_x - renderer->mesh_area_min_x) / (renderer->cols - 1);
            double mesh_cell_y =
                (renderer->mesh_area_max_y - renderer->mesh_area_min_y) / (renderer->rows - 1);

            // Number of cells needed to cover the bed
            plane_cols = static_cast<int>(std::ceil((plane_max_x - plane_min_x) / mesh_cell_x)) + 1;
            plane_rows = static_cast<int>(std::ceil((plane_max_y - plane_min_y) / mesh_cell_y)) + 1;

            // Clamp to reasonable limits
            plane_cols = std::max(2, std::min(plane_cols, 30));
            plane_rows = std::max(2, std::min(plane_rows, 30));

            grid_spacing_x = (plane_max_x - plane_min_x) / (plane_cols - 1);
            grid_spacing_y = (plane_max_y - plane_min_y) / (plane_rows - 1);
        } else {
            // Fallback: use mesh bounds
            plane_min_x = 0.0;
            plane_max_x = (renderer->cols - 1) * BED_MESH_SCALE;
            plane_min_y = 0.0;
            plane_max_y = (renderer->rows - 1) * BED_MESH_SCALE;
            plane_cols = renderer->cols;
            plane_rows = renderer->rows;
            grid_spacing_x = BED_MESH_SCALE;
            grid_spacing_y = BED_MESH_SCALE;
        }

        // Generate plane quads covering the full bed
        for (int row = 0; row < plane_rows - 1; row++) {
            for (int col = 0; col < plane_cols - 1; col++) {
                bed_mesh_quad_3d_t plane_quad;

                // Compute printer coordinates for this cell
                double printer_x0 = plane_min_x + col * grid_spacing_x;
                double printer_x1 = plane_min_x + (col + 1) * grid_spacing_x;
                double printer_y0 = plane_min_y + row * grid_spacing_y;
                double printer_y1 = plane_min_y + (row + 1) * grid_spacing_y;

                // Convert to world coordinates
                double base_x_0, base_x_1, base_y_0, base_y_1;
                if (renderer->geometry_computed) {
                    base_x_0 = helix::mesh::printer_x_to_world_x(printer_x0, renderer->bed_center_x,
                                                                 renderer->coord_scale);
                    base_x_1 = helix::mesh::printer_x_to_world_x(printer_x1, renderer->bed_center_x,
                                                                 renderer->coord_scale);
                    base_y_0 = helix::mesh::printer_y_to_world_y(printer_y0, renderer->bed_center_y,
                                                                 renderer->coord_scale);
                    base_y_1 = helix::mesh::printer_y_to_world_y(printer_y1, renderer->bed_center_y,
                                                                 renderer->coord_scale);
                } else {
                    // Legacy fallback
                    base_x_0 = printer_x0 - plane_max_x / 2.0;
                    base_x_1 = printer_x1 - plane_max_x / 2.0;
                    base_y_0 = -(printer_y0 - plane_max_y / 2.0);
                    base_y_1 = -(printer_y1 - plane_max_y / 2.0);
                }

                // All vertices at same Z (flat plane)
                // Vertex layout matches mesh quads: [0]=BL, [1]=BR, [2]=TL, [3]=TR
                plane_quad.vertices[0] = {base_x_0, base_y_1, plane_world_z, plane_color};
                plane_quad.vertices[1] = {base_x_1, base_y_1, plane_world_z, plane_color};
                plane_quad.vertices[2] = {base_x_0, base_y_0, plane_world_z, plane_color};
                plane_quad.vertices[3] = {base_x_1, base_y_0, plane_world_z, plane_color};

                plane_quad.center_color = plane_color;
                plane_quad.avg_depth = 0.0; // Will be computed during projection
                plane_quad.opacity = renderer->zero_plane_opacity; // Translucent

                renderer->quads.push_back(plane_quad);
            }
        }

        spdlog::debug("[QUAD_GEN] Generated {} zero plane quads ({}x{} grid) covering full bed "
                      "[{:.0f},{:.0f}]x[{:.0f},{:.0f}] at world_z={:.2f}",
                      renderer->quads.size() - mesh_quad_count, plane_cols - 1, plane_rows - 1,
                      plane_min_x, plane_max_x, plane_min_y, plane_max_y, plane_world_z);
    }

    spdlog::trace(
        "[Bed Mesh Geometry] Generated {} total quads ({} mesh + {} plane) from {}x{} mesh",
        renderer->quads.size(), mesh_quad_count, renderer->quads.size() - mesh_quad_count,
        renderer->rows, renderer->cols);
}

void sort_quads_by_depth(std::vector<bed_mesh_quad_3d_t>& quads) {
    std::sort(quads.begin(), quads.end(),
              [](const bed_mesh_quad_3d_t& a, const bed_mesh_quad_3d_t& b) {
                  // Descending order: furthest (largest depth) first
                  return a.avg_depth > b.avg_depth;
              });
}

} // namespace mesh
} // namespace helix

#endif // HELIX_HAS_BED_MESH_3D
