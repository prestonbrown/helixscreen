// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 HelixScreen Contributors
 *
 * This file is part of HelixScreen, which is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * See <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "gcode_parser.h" // For AABB

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

/**
 * @file gcode_camera.h
 * @brief 3D camera system for G-code visualization
 *
 * Manages view transformation (rotation, pan, zoom) and projection
 * (orthographic or perspective). Provides matrices for 3D-to-2D rendering
 * and screen-to-world ray casting for object picking.
 *
 * Coordinate System:
 * - World space: +X right, +Y front, +Z up (print bed at Z=0)
 * - Camera space: Looking down at print bed from angle
 * - Screen space: Origin at top-left, +X right, +Y down
 *
 * @see docs/GCODE_VISUALIZATION.md for complete design
 */

namespace gcode {

/**
 * @brief Projection type for camera
 */
enum class ProjectionType {
    ORTHOGRAPHIC, ///< Parallel projection (no perspective distortion)
    PERSPECTIVE   ///< Realistic perspective (not implemented in Phase 1)
};

/**
 * @brief 3D camera for G-code visualization
 *
 * Usage pattern:
 * @code
 *   GCodeCamera camera;
 *   camera.set_viewport_size(800, 480);
 *   camera.fit_to_bounds(gcode_file.global_bounding_box);
 *   camera.rotate(10.0f, 5.0f);  // Adjust view
 *   glm::mat4 transform = camera.get_view_projection_matrix();
 *   // Use transform to render segments...
 * @endcode
 */
class GCodeCamera {
  public:
    GCodeCamera();
    ~GCodeCamera() = default;

    // ==============================================
    // Camera Controls
    // ==============================================

    /**
     * @brief Rotate camera view
     * @param delta_azimuth Horizontal rotation in degrees (around Z-axis)
     * @param delta_elevation Vertical rotation in degrees (tilt up/down)
     *
     * Azimuth: 0° = front view, 90° = right view, 180° = back, 270° = left
     * Elevation: 0° = side view, 90° = top view, -90° = bottom view
     */
    void rotate(float delta_azimuth, float delta_elevation);

    /**
     * @brief Pan camera (translate view)
     * @param delta_x Horizontal pan in world units
     * @param delta_y Vertical pan in world units
     */
    void pan(float delta_x, float delta_y);

    /**
     * @brief Zoom camera
     * @param factor Zoom factor (>1.0 = zoom in, <1.0 = zoom out)
     *
     * Example: zoom(1.1f) zooms in 10%, zoom(0.9f) zooms out 10%
     */
    void zoom(float factor);

    /**
     * @brief Reset camera to default view
     *
     * Resets azimuth, elevation, pan, and zoom to defaults.
     * Call fit_to_bounds() after reset to frame the model.
     */
    void reset();

    /**
     * @brief Fit camera to view entire bounding box
     * @param bounds Model bounding box to fit
     *
     * Automatically adjusts zoom and pan to frame the model.
     * Preserves current azimuth and elevation angles.
     */
    void fit_to_bounds(const AABB& bounds);

    // ==============================================
    // Preset Views
    // ==============================================

    /**
     * @brief Set top-down view (looking straight down at print bed)
     */
    void set_top_view();

    /**
     * @brief Set front view (looking from front of printer)
     */
    void set_front_view();

    /**
     * @brief Set side view (looking from right side)
     */
    void set_side_view();

    /**
     * @brief Set isometric view (45° azimuth, 30° elevation)
     *
     * Default view - good compromise between visibility and depth perception.
     */
    void set_isometric_view();

    // ==============================================
    // Matrix Access
    // ==============================================

    /**
     * @brief Get view matrix (world-to-camera transform)
     * @return 4x4 view matrix
     */
    glm::mat4 get_view_matrix() const {
        return view_matrix_;
    }

    /**
     * @brief Get projection matrix (camera-to-screen transform)
     * @return 4x4 projection matrix
     */
    glm::mat4 get_projection_matrix() const {
        return projection_matrix_;
    }

    /**
     * @brief Get combined view-projection matrix
     * @return Projection * View matrix
     *
     * Use this for transforming 3D world coordinates to 2D screen space.
     */
    glm::mat4 get_view_projection_matrix() const {
        return projection_matrix_ * view_matrix_;
    }

    // ==============================================
    // Configuration
    // ==============================================

    /**
     * @brief Set projection type
     * @param type ORTHOGRAPHIC or PERSPECTIVE
     *
     * Note: PERSPECTIVE not fully implemented in Phase 1
     */
    void set_projection_type(ProjectionType type);

    /**
     * @brief Set viewport size
     * @param width Viewport width in pixels
     * @param height Viewport height in pixels
     *
     * Call this when screen size changes to update projection matrix.
     */
    void set_viewport_size(int width, int height);

    /**
     * @brief Get current viewport width
     * @return Width in pixels
     */
    int get_viewport_width() const {
        return viewport_width_;
    }

    /**
     * @brief Get current viewport height
     * @return Height in pixels
     */
    int get_viewport_height() const {
        return viewport_height_;
    }

    // ==============================================
    // Ray Casting (for object picking)
    // ==============================================

    /**
     * @brief Convert screen coordinates to world-space ray
     * @param screen_pos Screen coordinates (top-left origin)
     * @return Normalized ray direction in world space
     *
     * Used for touch/click object picking. Cast ray from screen point
     * through camera and test intersection with objects.
     */
    glm::vec3 screen_to_world_ray(const glm::vec2& screen_pos) const;

    // ==============================================
    // State Query
    // ==============================================

    /**
     * @brief Get current azimuth angle
     * @return Azimuth in degrees (0-360)
     */
    float get_azimuth() const {
        return azimuth_;
    }

    /**
     * @brief Get current elevation angle
     * @return Elevation in degrees (-90 to 90)
     */
    float get_elevation() const {
        return elevation_;
    }

    /**
     * @brief Get current zoom level
     * @return Zoom factor (1.0 = default)
     */
    float get_zoom_level() const {
        return zoom_level_;
    }

    /**
     * @brief Get camera target point (look-at point)
     * @return Target position in world space
     */
    glm::vec3 get_target() const {
        return target_;
    }

  private:
    /**
     * @brief Recompute view and projection matrices
     *
     * Called automatically when camera parameters change.
     */
    void update_matrices();

    /**
     * @brief Compute camera position from azimuth, elevation, and distance
     * @return Camera position in world space
     */
    glm::vec3 compute_camera_position() const;

    // Camera parameters
    float azimuth_{45.0f};      ///< Horizontal rotation (degrees)
    float elevation_{30.0f};    ///< Vertical rotation (degrees)
    glm::vec3 target_{0, 0, 0}; ///< Look-at point
    float distance_{100.0f};    ///< Distance from target
    float zoom_level_{1.0f};    ///< Zoom multiplier

    // Projection parameters
    ProjectionType projection_type_{ProjectionType::ORTHOGRAPHIC};
    int viewport_width_{800};
    int viewport_height_{480};
    float near_plane_{0.1f};
    float far_plane_{1000.0f};

    // Computed matrices
    glm::mat4 view_matrix_{1.0f};
    glm::mat4 projection_matrix_{1.0f};
};

} // namespace gcode
