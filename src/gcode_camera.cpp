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

#include "gcode_camera.h"

#include <algorithm>
#define GLM_ENABLE_EXPERIMENTAL
#include <spdlog/spdlog.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtx/transform.hpp>

namespace gcode {

GCodeCamera::GCodeCamera() {
    reset();
}

void GCodeCamera::reset() {
    azimuth_ = 45.0f;
    elevation_ = 30.0f;
    target_ = glm::vec3(0, 0, 0);
    distance_ = 100.0f;
    zoom_level_ = 1.0f;
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

    // Clamp zoom to reasonable range
    zoom_level_ = std::clamp(zoom_level_, 0.1f, 10.0f);

    update_matrices();
}

void GCodeCamera::fit_to_bounds(const AABB& bounds) {
    if (bounds.is_empty()) {
        spdlog::warn("Cannot fit camera to empty bounding box");
        return;
    }

    // Set target to center of bounding box
    target_ = bounds.center();

    // Calculate distance to fit entire model in view
    glm::vec3 size = bounds.size();
    float max_dimension = std::max({size.x, size.y, size.z});

    // For orthographic projection, we adjust zoom instead of distance
    // Distance affects near/far planes, zoom affects the orthographic scale
    distance_ = max_dimension * 2.0f; // Far enough for near/far planes
    zoom_level_ = 1.0f;               // Start at 1.0, will be adjusted by projection

    update_matrices();

    spdlog::debug(
        "Fit camera to bounds: center=({:.1f},{:.1f},{:.1f}), size=({:.1f},{:.1f},{:.1f})",
        target_.x, target_.y, target_.z, size.x, size.y, size.z);
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

void GCodeCamera::set_projection_type(ProjectionType type) {
    if (type == ProjectionType::PERSPECTIVE) {
        spdlog::warn("Perspective projection not fully implemented in Phase 1");
    }

    projection_type_ = type;
    update_matrices();
}

void GCodeCamera::set_viewport_size(int width, int height) {
    viewport_width_ = width;
    viewport_height_ = height;
    update_matrices();
}

glm::vec3 GCodeCamera::screen_to_world_ray(const glm::vec2& screen_pos) const {
    // Convert screen coordinates to normalized device coordinates [-1, 1]
    float x = (2.0f * screen_pos.x) / viewport_width_ - 1.0f;
    float y = 1.0f - (2.0f * screen_pos.y) / viewport_height_; // Flip Y

    // For orthographic projection, ray direction is constant (parallel)
    // Ray direction = inverse view direction
    glm::vec3 camera_pos = compute_camera_position();
    glm::vec3 ray_dir = glm::normalize(target_ - camera_pos);

    return ray_dir;
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

    spdlog::trace("Camera updated: azimuth={:.1f}°, elevation={:.1f}°, "
                  "distance={:.1f}, zoom={:.2f}",
                  azimuth_, elevation_, distance_, zoom_level_);
}

} // namespace gcode
