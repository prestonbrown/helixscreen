// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_camera.h"

#include "gcode_projection.h"

#include <algorithm>
#define GLM_ENABLE_EXPERIMENTAL
#include <spdlog/spdlog.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtx/transform.hpp>

namespace helix {
namespace gcode {

GCodeCamera::GCodeCamera() {
    reset();
}

void GCodeCamera::reset() {
    // Default isometric view matching OrcaSlicer thumbnail camera:
    // OrcaSlicer uses zenit=45°, phi=45° (Camera.cpp line 682-691)
    azimuth_ = -45.0f;  // Front-left view (OrcaSlicer phi=45° maps to our -45°)
    elevation_ = 45.0f; // Match OrcaSlicer zenit=45°
    target_ = glm::vec3(0, 0, 0);
    distance_ = 100.0f;
    zoom_level_ = 1.4f; // Fit model with margin (OrcaSlicer uses ~2.5% margin)
    projection_type_ = ProjectionType::ORTHOGRAPHIC;

    update_matrices();
}

void GCodeCamera::rotate(float delta_azimuth, float delta_elevation) {
    azimuth_ += delta_azimuth;
    elevation_ += delta_elevation;

    // Wrap azimuth to [0, 360)
    while (azimuth_ >= 360.0f)
        azimuth_ -= 360.0f;
    while (azimuth_ < 0.0f)
        azimuth_ += 360.0f;

    // Clamp elevation to [-89, 89] to avoid gimbal lock at poles
    elevation_ = std::clamp(elevation_, -89.0f, 89.0f);

    update_matrices();
}

void GCodeCamera::pan(float delta_x, float delta_y) {
    // Convert screen-space pan to world-space movement
    // Pan perpendicular to view direction
    glm::vec3 camera_pos = compute_camera_position();
    glm::vec3 view_dir = glm::normalize(target_ - camera_pos);

    // Right vector (perpendicular to view and up)
    glm::vec3 up(0, 0, 1);
    glm::vec3 right = glm::normalize(glm::cross(view_dir, up));

    // Up vector in camera space (perpendicular to view and right)
    glm::vec3 camera_up = glm::normalize(glm::cross(right, view_dir));

    // Apply pan
    target_ += right * delta_x;
    target_ += camera_up * delta_y;

    update_matrices();
}

void GCodeCamera::zoom(float factor) {
    zoom_level_ *= factor;

    // Clamp zoom to reasonable range (increased max for close inspection)
    zoom_level_ = std::clamp(zoom_level_, 0.1f, 100.0f);

    update_matrices();
}

void GCodeCamera::fit_to_bounds(const AABB& raw_bounds) {
    // Order any inverted axis before the emptiness test — one bad axis must not
    // discard the others. Shared with compute_auto_fit() so the 3D and 2D paths
    // agree on which boxes are framable.
    const AABB bounds = raw_bounds.normalized();

    if (bounds.is_empty()) {
        spdlog::warn("[GCode Camera] Cannot fit camera to empty bounding box");
        return;
    }

    // Set target to center of bounding box
    target_ = bounds.center();

    // Set distance far enough for near/far clipping planes
    glm::vec3 size = bounds.size();
    float max_dimension = std::max({size.x, size.y, size.z});
    distance_ = max_dimension * 2.0f;

    // Compute the screen-space projected extent of the AABB at current
    // camera angles.  The renderer applies a -90° Z rotation (model matrix),
    // then the view matrix from our azimuth/elevation.  We need to know how
    // big the model actually looks on screen to set proper zoom.
    glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0, 0, 1));
    glm::vec3 camera_pos = compute_camera_position();
    glm::vec3 up(0, 0, 1);
    glm::mat4 view = glm::lookAt(camera_pos, target_, up);
    glm::mat4 mv = view * model;

    // Project all 8 AABB corners into camera space
    float proj_min_x = 1e30f, proj_max_x = -1e30f;
    float proj_min_y = 1e30f, proj_max_y = -1e30f;
    for (int i = 0; i < 8; i++) {
        glm::vec3 corner((i & 1) ? bounds.max.x : bounds.min.x,
                         (i & 2) ? bounds.max.y : bounds.min.y,
                         (i & 4) ? bounds.max.z : bounds.min.z);
        glm::vec4 cs = mv * glm::vec4(corner, 1.0f);
        proj_min_x = std::min(proj_min_x, cs.x);
        proj_max_x = std::max(proj_max_x, cs.x);
        proj_min_y = std::min(proj_min_y, cs.y);
        proj_max_y = std::max(proj_max_y, cs.y);
    }

    float proj_width = proj_max_x - proj_min_x;
    float proj_height = proj_max_y - proj_min_y;

    // Guard against zero viewport (widget not yet sized)
    if (viewport_width_ <= 0 || viewport_height_ <= 0 || proj_width <= 0.0f ||
        proj_height <= 0.0f) {
        zoom_level_ = 1.4f;
        content_height_world_ = 0.0f;
        update_matrices();
        spdlog::debug("[GCode Camera] Fit to bounds: fallback (viewport {}x{})", viewport_width_,
                      viewport_height_);
        return;
    }

    float aspect = static_cast<float>(viewport_width_) / static_cast<float>(viewport_height_);

    // Compute zoom so the projected model fits with ~5% margin on each side
    // ortho_size = distance / (2 * zoom), visible = 2 * ortho_size (vertical)
    // We need: proj_height <= 2 * ortho_size * margin, and
    //          proj_width  <= 2 * ortho_size * aspect * margin
    constexpr float MARGIN = 0.80f; // ~10% margin on each side

    // Same shape rule as compute_auto_fit(): only a model markedly taller than
    // it is wide keeps the full viewport height and runs under the bottom UI
    // strip. Anything squatter is framed to sit above it.
    const bool elongated = proj_height > projection::ELONGATION_LIMIT * proj_width;
    const float usable_height_frac = elongated ? 1.0f : (1.0f - bottom_occlusion_);

    float zoom_for_height = distance_ * usable_height_frac / (proj_height / MARGIN);
    float zoom_for_width = distance_ * aspect / (proj_width / MARGIN);
    zoom_level_ = std::min(zoom_for_height, zoom_for_width);
    zoom_level_ = std::clamp(zoom_level_, 0.1f, 100.0f);

    // Keep the model's projected height in world units. The visible vertical
    // extent is 2 * ortho_size = distance / zoom, so get_content_height_fraction()
    // can turn this into a viewport fraction that also tracks later pinch-zoom.
    content_height_world_ = proj_height;

    // Shift target so the projected center is on screen center.
    // The projected midpoint in camera space may not be at (0,0) because
    // of the model rotation.  Convert the offset back to world space.
    float proj_cx = (proj_min_x + proj_max_x) * 0.5f;
    float proj_cy = (proj_min_y + proj_max_y) * 0.5f;

    // Camera-space X/Y axes in world space (from view matrix rows)
    glm::vec3 cam_right(view[0][0], view[1][0], view[2][0]);
    glm::vec3 cam_up(view[0][1], view[1][1], view[2][1]);
    target_ += cam_right * proj_cx + cam_up * proj_cy;

    update_matrices();

    spdlog::debug("[GCode Camera] Fit to bounds: center=({:.1f},{:.1f},{:.1f}), "
                  "size=({:.1f},{:.1f},{:.1f}), proj=({:.1f}x{:.1f}), zoom={:.2f}",
                  target_.x, target_.y, target_.z, size.x, size.y, size.z, proj_width, proj_height,
                  zoom_level_);
}

void GCodeCamera::set_top_view() {
    azimuth_ = 0.0f;
    elevation_ = 89.0f; // Almost straight down (avoid gimbal lock at 90°)
    update_matrices();
}

void GCodeCamera::set_front_view() {
    azimuth_ = 0.0f;
    elevation_ = 0.0f;
    update_matrices();
}

void GCodeCamera::set_side_view() {
    azimuth_ = 90.0f;
    elevation_ = 0.0f;
    update_matrices();
}

void GCodeCamera::set_isometric_view() {
    azimuth_ = 45.0f;
    elevation_ = 30.0f;
    update_matrices();
}

void GCodeCamera::set_azimuth(float azimuth) {
    azimuth_ = azimuth;
    // Wrap azimuth to [0, 360)
    while (azimuth_ >= 360.0f)
        azimuth_ -= 360.0f;
    while (azimuth_ < 0.0f)
        azimuth_ += 360.0f;
    update_matrices();
}

void GCodeCamera::set_elevation(float elevation) {
    // Clamp elevation to [-89, 89] to avoid gimbal lock at poles
    elevation_ = std::clamp(elevation, -89.0f, 89.0f);
    update_matrices();
}

void GCodeCamera::set_zoom_level(float zoom) {
    // Clamp zoom to reasonable range (increased max for close inspection)
    zoom_level_ = std::clamp(zoom, 0.1f, 100.0f);
    update_matrices();
}

void GCodeCamera::set_projection_type(ProjectionType type) {
    if (type == ProjectionType::PERSPECTIVE) {
        spdlog::warn("[GCode Camera] Perspective projection not fully implemented in Phase 1");
    }

    projection_type_ = type;
    update_matrices();
}

void GCodeCamera::set_viewport_size(int width, int height) {
    viewport_width_ = width;
    viewport_height_ = height;
    update_matrices();
}

glm::vec3 GCodeCamera::compute_camera_position() const {
    // Convert spherical coordinates (azimuth, elevation, distance) to Cartesian
    float azimuth_rad = glm::radians(azimuth_);
    float elevation_rad = glm::radians(elevation_);

    float cos_elev = std::cos(elevation_rad);
    float sin_elev = std::sin(elevation_rad);
    float cos_azim = std::cos(azimuth_rad);
    float sin_azim = std::sin(azimuth_rad);

    // Position relative to target
    glm::vec3 offset(distance_ * cos_elev * sin_azim, // X
                     distance_ * cos_elev * cos_azim, // Y
                     distance_ * sin_elev             // Z
    );

    return target_ + offset;
}

void GCodeCamera::update_matrices() {
    // === View Matrix ===
    glm::vec3 camera_pos = compute_camera_position();
    glm::vec3 up(0, 0, 1); // Z-up world

    view_matrix_ = glm::lookAt(camera_pos, target_, up);

    // === Projection Matrix ===
    float aspect_ratio = static_cast<float>(viewport_width_) / static_cast<float>(viewport_height_);

    if (projection_type_ == ProjectionType::ORTHOGRAPHIC) {
        // Orthographic projection - no perspective distortion
        // Adjust size based on zoom level
        float ortho_size = distance_ / (2.0f * zoom_level_);

        float left = -ortho_size * aspect_ratio;
        float right = ortho_size * aspect_ratio;
        float bottom = -ortho_size;
        float top = ortho_size;

        projection_matrix_ = glm::ortho(left, right, bottom, top, near_plane_, far_plane_);
    } else {
        // Perspective projection (not used in Phase 1)
        float fov = glm::radians(60.0f / zoom_level_);
        projection_matrix_ = glm::perspective(fov, aspect_ratio, near_plane_, far_plane_);
    }

    spdlog::trace("[GCode Camera] Camera updated: azimuth={:.1f}°, elevation={:.1f}°, "
                  "distance={:.1f}, zoom={:.2f}",
                  azimuth_, elevation_, distance_, zoom_level_);
}

} // namespace gcode
} // namespace helix
