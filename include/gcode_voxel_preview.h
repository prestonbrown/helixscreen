// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef GCODE_VOXEL_PREVIEW_H
#define GCODE_VOXEL_PREVIEW_H

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief Camera angle for rendering G-code preview
 */
enum class CameraAngle {
    FRONT,     // Front view (looking along -Z)
    TOP,       // Top view (looking along -Y)
    ISOMETRIC, // Isometric view (45 degrees)
    SIDE       // Side view (looking along -X)
};

/**
 * @brief Configuration for voxel preview generation
 */
struct VoxelPreviewConfig {
    float resolution = 2.0f;         // Voxels per millimeter
    float blur_sigma = 2.0f;         // Gaussian blur sigma
    int chunk_size = 32;             // Chunk size for processing
    int output_width = 512;          // Output image width
    int output_height = 512;         // Output image height
    float iso_threshold = 0.3f;      // Threshold for surface detection
    bool adaptive_resolution = true; // Use adaptive resolution
};

/**
 * @brief Hash function for glm::ivec3 to use in unordered_map
 */
struct IVec3Hash {
    std::size_t operator()(const glm::ivec3& v) const {
        std::size_t h1 = std::hash<int>()(v.x);
        std::size_t h2 = std::hash<int>()(v.y);
        std::size_t h3 = std::hash<int>()(v.z);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

/**
 * @brief Sparse voxel grid for memory-efficient G-code preview
 *
 * Uses sparse storage to minimize memory usage on embedded systems.
 * Only stores non-zero voxels, dramatically reducing RAM requirements.
 */
class SparseVoxelGrid {
  public:
    /**
     * @brief Construct a new sparse voxel grid
     *
     * @param resolution Voxels per millimeter
     */
    explicit SparseVoxelGrid(float resolution = 2.0f);

    /**
     * @brief Add a line segment to the voxel grid
     *
     * @param p1 Start point in millimeters
     * @param p2 End point in millimeters
     * @param thickness Line thickness in millimeters (e.g., nozzle diameter)
     */
    void addSegment(const glm::vec3& p1, const glm::vec3& p2, float thickness = 0.4f);

    /**
     * @brief Apply Gaussian blur to voxels in chunks to reduce memory
     *
     * @param sigma Gaussian blur sigma (larger = more blur)
     */
    void applyGaussianBlur(float sigma = 2.0f, int chunk_size = 32);

    /**
     * @brief Render voxel grid to PNG image
     *
     * @param output_path Path to save PNG file
     * @param angle Camera angle for rendering
     * @param width Output image width
     * @param height Output image height
     * @return true if successful
     */
    bool renderToImage(const std::string& output_path, CameraAngle angle = CameraAngle::ISOMETRIC,
                       int width = 512, int height = 512);

    /**
     * @brief Get memory usage in bytes
     *
     * @return Approximate memory usage of voxel data
     */
    size_t getMemoryUsage() const;

    /**
     * @brief Get bounding box of voxel grid
     *
     * @param min_bound Output minimum bound
     * @param max_bound Output maximum bound
     */
    void getBounds(glm::vec3& min_bound, glm::vec3& max_bound) const;

    /**
     * @brief Clear all voxel data
     */
    void clear();

    /**
     * @brief Get number of voxels
     *
     * @return Number of non-zero voxels
     */
    size_t getVoxelCount() const { return voxels_.size(); }

  private:
    std::unordered_map<glm::ivec3, float, IVec3Hash> voxels_;
    float resolution_; // voxels per mm
    glm::vec3 min_bound_;
    glm::vec3 max_bound_;

    /**
     * @brief Convert world coordinates to voxel coordinates
     */
    glm::ivec3 worldToVoxel(const glm::vec3& world) const;

    /**
     * @brief Convert voxel coordinates to world coordinates
     */
    glm::vec3 voxelToWorld(const glm::ivec3& voxel) const;

    /**
     * @brief Get voxel value at position (0 if not set)
     */
    float getVoxel(const glm::ivec3& pos) const;

    /**
     * @brief Set voxel value at position
     */
    void setVoxel(const glm::ivec3& pos, float value);

    /**
     * @brief Render depth buffer for raycast rendering
     */
    void renderDepthBuffer(std::vector<float>& depth_buffer, std::vector<glm::vec3>& normal_buffer,
                           CameraAngle angle, int width, int height);

    /**
     * @brief Get camera transform matrix for given angle
     */
    glm::mat4 getCameraTransform(CameraAngle angle) const;
};

/**
 * @brief G-code parser for extracting extrusion paths
 *
 * Handles absolute/relative positioning and extrusion tracking
 */
class GCodeParser {
  public:
    GCodeParser();

    /**
     * @brief Parse G-code file and extract extrusion segments
     *
     * Only returns segments where material is actually being extruded (E > 0)
     *
     * @param filepath Path to G-code file
     * @return Vector of line segments (start, end) in millimeters
     */
    std::vector<std::pair<glm::vec3, glm::vec3>> parseExtrusions(const std::string& filepath);

    /**
     * @brief Get parsing statistics
     *
     * @param total_lines Output total lines parsed
     * @param extrusion_lines Output lines with extrusion
     * @param total_length Output total extrusion length in mm
     */
    void getStats(size_t& total_lines, size_t& extrusion_lines, float& total_length) const;

  private:
    struct ParserState {
        glm::vec3 position{0, 0, 0};
        float e_position = 0.0f;
        bool absolute_mode = true;   // G90 vs G91
        bool absolute_e_mode = true; // M82 vs M83
    };

    ParserState state_;
    size_t total_lines_ = 0;
    size_t extrusion_lines_ = 0;
    float total_length_ = 0.0f;

    /**
     * @brief Parse a single G-code line
     */
    void parseLine(const std::string& line, std::vector<std::pair<glm::vec3, glm::vec3>>& segments);

    /**
     * @brief Extract float parameter from G-code line (e.g., "X123.45")
     */
    bool extractParam(const std::string& line, char param, float& value) const;
};

/**
 * @brief Generate G-code preview image from file
 *
 * High-level convenience function that handles the entire pipeline:
 * parse G-code -> voxelize -> blur -> render
 *
 * @param gcode_path Path to G-code file
 * @param output_path Path to save preview PNG
 * @param config Preview configuration
 * @return true if successful
 */
bool generateGCodePreview(const std::string& gcode_path, const std::string& output_path,
                          const VoxelPreviewConfig& config = VoxelPreviewConfig());

#endif // GCODE_VOXEL_PREVIEW_H
