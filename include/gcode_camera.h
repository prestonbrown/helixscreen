// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gcode_parser.h" // For AABB

#include <algorithm>
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
 */

namespace helix {
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
     * @param raw_bounds Model bounding box to fit. Normalized internally, so an
     *                   axis that arrived inverted does not discard the axes
     *                   that were valid.
     *
     * Automatically adjusts zoom and pan to frame the model.
     * Preserves current azimuth and elevation angles.
     */
    void fit_to_bounds(const AABB& raw_bounds);

    /// Fraction of viewport height covered by UI along the bottom (0..1). Feeds
    /// the same shape rule the 2D path uses: a squat model is framed above the
    /// strip, an elongated one keeps the full viewport. Takes effect on the next
    /// fit_to_bounds().
    void set_bottom_occlusion(float occlusion) {
        bottom_occlusion_ = std::clamp(occlusion, 0.0f, 1.0f);
    }

    /**
     * @brief Fraction of viewport height the fitted model occupies (0..1).
     *
     * The 3D analogue of AutoFitResult::content_height: multiplied by the
     * viewport height it gives the same "how tall does this model draw"
     * quantity, so both renderers feed one
     * helix::gcode::compute_content_offset_y(). 0 before the first fit.
     */
    float get_content_height_fraction() const {
        if (distance_ <= 0.0f) {
            return 0.0f;
        }
        return std::clamp(content_height_world_ * zoom_level_ / distance_, 0.0f, 1.0f);
    }

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
    // Direct Setters
    // ==============================================

    /**
     * @brief Set azimuth angle directly
     * @param azimuth Horizontal rotation in degrees (0-360)
     */
    void set_azimuth(float azimuth);

    /**
     * @brief Set elevation angle directly
     * @param elevation Vertical rotation in degrees (-90 to 90)
     */
    void set_elevation(float elevation);

    /**
     * @brief Set zoom level directly
     * @param zoom Zoom factor (>0, 1.0 = default)
     */
    void set_zoom_level(float zoom);

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
     * @brief Get current projection type
     * @return Current projection type (ORTHOGRAPHIC or PERSPECTIVE)
     */
    ProjectionType get_projection_type() const {
        return projection_type_;
    }

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

    /**
     * @brief Get camera position in world space
     * @return Camera position
     */
    glm::vec3 get_camera_position() const {
        return compute_camera_position();
    }

    /**
     * @brief Get camera distance from target
     * @return Distance in world units
     */
    float get_distance() const {
        return distance_;
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
    float azimuth_{45.0f};             ///< Horizontal rotation (degrees)
    float elevation_{30.0f};           ///< Vertical rotation (degrees)
    glm::vec3 target_{0, 0, 0};        ///< Look-at point
    float content_height_world_{0.0f}; ///< Fitted model's projected height, world units
    float bottom_occlusion_{0.0f};     ///< Fraction of viewport height covered by UI
    float distance_{100.0f};           ///< Distance from target
    float zoom_level_{1.0f};           ///< Zoom multiplier

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
} // namespace helix
