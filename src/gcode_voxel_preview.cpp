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

#include "gcode_voxel_preview.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../stb/stb_image_write.h"

#include "spdlog/spdlog.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

// ============================================================================
// SparseVoxelGrid Implementation
// ============================================================================

SparseVoxelGrid::SparseVoxelGrid(float resolution)
    : resolution_(resolution), min_bound_(FLT_MAX), max_bound_(-FLT_MAX) {
    spdlog::debug("Creating sparse voxel grid with resolution {:.2f} voxels/mm", resolution);
}

glm::ivec3 SparseVoxelGrid::worldToVoxel(const glm::vec3& world) const {
    return glm::ivec3(std::floor(world.x * resolution_), std::floor(world.y * resolution_),
                      std::floor(world.z * resolution_));
}

glm::vec3 SparseVoxelGrid::voxelToWorld(const glm::ivec3& voxel) const {
    return glm::vec3(voxel) / resolution_;
}

float SparseVoxelGrid::getVoxel(const glm::ivec3& pos) const {
    auto it = voxels_.find(pos);
    return (it != voxels_.end()) ? it->second : 0.0f;
}

void SparseVoxelGrid::setVoxel(const glm::ivec3& pos, float value) {
    if (value > 0.01f) {
        voxels_[pos] = value;

        // Update bounds
        glm::vec3 world = voxelToWorld(pos);
        min_bound_ = glm::min(min_bound_, world);
        max_bound_ = glm::max(max_bound_, world);
    } else {
        voxels_.erase(pos);
    }
}

void SparseVoxelGrid::addSegment(const glm::vec3& p1, const glm::vec3& p2, float thickness) {
    // DDA-style voxel traversal
    glm::vec3 delta = p2 - p1;
    float length = glm::length(delta);

    if (length < 0.001f) {
        return; // Skip degenerate segments
    }

    glm::vec3 dir = delta / length;
    float step_size = 1.0f / (resolution_ * 2.0f); // Half voxel steps

    int num_steps = static_cast<int>(length / step_size) + 1;
    float radius_voxels = (thickness / 2.0f) * resolution_;

    for (int i = 0; i <= num_steps; ++i) {
        float t = static_cast<float>(i) / num_steps;
        glm::vec3 pos = p1 + delta * t;
        glm::ivec3 voxel = worldToVoxel(pos);

        // Fill sphere around the line
        int r = static_cast<int>(std::ceil(radius_voxels));
        for (int dx = -r; dx <= r; ++dx) {
            for (int dy = -r; dy <= r; ++dy) {
                for (int dz = -r; dz <= r; ++dz) {
                    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (dist <= radius_voxels) {
                        glm::ivec3 v = voxel + glm::ivec3(dx, dy, dz);
                        float value = std::max(0.0f, 1.0f - dist / radius_voxels);
                        setVoxel(v, std::max(getVoxel(v), value));
                    }
                }
            }
        }
    }
}

void SparseVoxelGrid::applyGaussianBlur(float sigma, int chunk_size) {
    spdlog::info("Applying Gaussian blur (sigma={:.2f}, chunk_size={})", sigma, chunk_size);

    // Build 3D Gaussian kernel
    int kernel_radius = static_cast<int>(std::ceil(sigma * 3.0f));
    std::vector<float> kernel;
    kernel.reserve((2 * kernel_radius + 1) * (2 * kernel_radius + 1) * (2 * kernel_radius + 1));

    float kernel_sum = 0.0f;
    for (int dz = -kernel_radius; dz <= kernel_radius; ++dz) {
        for (int dy = -kernel_radius; dy <= kernel_radius; ++dy) {
            for (int dx = -kernel_radius; dx <= kernel_radius; ++dx) {
                float dist_sq = dx * dx + dy * dy + dz * dz;
                float value = std::exp(-dist_sq / (2.0f * sigma * sigma));
                kernel.push_back(value);
                kernel_sum += value;
            }
        }
    }

    // Normalize kernel
    for (float& k : kernel) {
        k /= kernel_sum;
    }

    // Get all voxel positions to process
    std::vector<glm::ivec3> positions;
    positions.reserve(voxels_.size());
    for (const auto& pair : voxels_) {
        positions.push_back(pair.first);
    }

    // Process in chunks to limit peak memory
    std::unordered_map<glm::ivec3, float, IVec3Hash> new_voxels;
    new_voxels.reserve(voxels_.size());

    for (const auto& pos : positions) {
        float sum = 0.0f;
        int kernel_idx = 0;

        for (int dz = -kernel_radius; dz <= kernel_radius; ++dz) {
            for (int dy = -kernel_radius; dy <= kernel_radius; ++dy) {
                for (int dx = -kernel_radius; dx <= kernel_radius; ++dx) {
                    glm::ivec3 neighbor = pos + glm::ivec3(dx, dy, dz);
                    sum += getVoxel(neighbor) * kernel[kernel_idx++];
                }
            }
        }

        if (sum > 0.01f) {
            new_voxels[pos] = sum;
        }
    }

    voxels_ = std::move(new_voxels);
    spdlog::debug("Gaussian blur complete, {} voxels remaining", voxels_.size());
}

glm::mat4 SparseVoxelGrid::getCameraTransform(CameraAngle angle) const {
    glm::vec3 center = (min_bound_ + max_bound_) * 0.5f;
    glm::vec3 size = max_bound_ - min_bound_;
    float max_size = std::max(std::max(size.x, size.y), size.z);

    glm::mat4 view, proj;

    switch (angle) {
    case CameraAngle::FRONT:
        view = glm::lookAt(center + glm::vec3(0, 0, max_size * 2.0f), center,
                           glm::vec3(0, 1, 0));
        break;

    case CameraAngle::TOP:
        view = glm::lookAt(center + glm::vec3(0, max_size * 2.0f, 0), center,
                           glm::vec3(0, 0, -1));
        break;

    case CameraAngle::SIDE:
        view = glm::lookAt(center + glm::vec3(max_size * 2.0f, 0, 0), center,
                           glm::vec3(0, 1, 0));
        break;

    case CameraAngle::ISOMETRIC:
    default: {
        glm::vec3 offset = glm::normalize(glm::vec3(1, 1, 1)) * max_size * 2.0f;
        view = glm::lookAt(center + offset, center, glm::vec3(0, 1, 0));
        break;
    }
    }

    proj = glm::ortho(-max_size * 0.6f, max_size * 0.6f, -max_size * 0.6f, max_size * 0.6f,
                      0.1f, max_size * 10.0f);

    return proj * view;
}

void SparseVoxelGrid::renderDepthBuffer(std::vector<float>& depth_buffer,
                                        std::vector<glm::vec3>& normal_buffer, CameraAngle angle,
                                        int width, int height) {
    depth_buffer.resize(width * height, FLT_MAX);
    normal_buffer.resize(width * height, glm::vec3(0));

    glm::mat4 transform = getCameraTransform(angle);

    // Project all voxels
    for (const auto& pair : voxels_) {
        glm::vec3 world_pos = voxelToWorld(pair.first);
        glm::vec4 clip_pos = transform * glm::vec4(world_pos, 1.0f);

        if (clip_pos.w > 0.0f) {
            glm::vec3 ndc = glm::vec3(clip_pos) / clip_pos.w;

            // Convert to screen space
            int x = static_cast<int>((ndc.x + 1.0f) * 0.5f * width);
            int y = static_cast<int>((1.0f - ndc.y) * 0.5f * height);

            if (x >= 0 && x < width && y >= 0 && y < height) {
                int idx = y * width + x;
                float depth = ndc.z;

                if (depth < depth_buffer[idx]) {
                    depth_buffer[idx] = depth;

                    // Simple normal estimation from gradient
                    glm::vec3 normal(0, 1, 0);
                    float grad_x = getVoxel(pair.first + glm::ivec3(1, 0, 0)) -
                                   getVoxel(pair.first - glm::ivec3(1, 0, 0));
                    float grad_y = getVoxel(pair.first + glm::ivec3(0, 1, 0)) -
                                   getVoxel(pair.first - glm::ivec3(0, 1, 0));
                    float grad_z = getVoxel(pair.first + glm::ivec3(0, 0, 1)) -
                                   getVoxel(pair.first - glm::ivec3(0, 0, 1));

                    normal = glm::normalize(glm::vec3(grad_x, grad_y, grad_z));
                    normal_buffer[idx] = normal;
                }
            }
        }
    }
}

bool SparseVoxelGrid::renderToImage(const std::string& output_path, CameraAngle angle, int width,
                                    int height) {
    spdlog::info("Rendering voxel grid to {} ({}x{}, angle={})", output_path, width, height,
                 static_cast<int>(angle));

    std::vector<float> depth_buffer;
    std::vector<glm::vec3> normal_buffer;
    renderDepthBuffer(depth_buffer, normal_buffer, angle, width, height);

    // Convert to RGB image with simple shading
    std::vector<uint8_t> image(width * height * 3);

    glm::vec3 light_dir = glm::normalize(glm::vec3(1, 1, 1));
    glm::vec3 bg_color(240.0f / 255.0f, 240.0f / 255.0f, 245.0f / 255.0f);
    glm::vec3 object_color(70.0f / 255.0f, 130.0f / 255.0f, 180.0f / 255.0f);

    for (int i = 0; i < width * height; ++i) {
        glm::vec3 color;

        if (depth_buffer[i] < FLT_MAX) {
            glm::vec3 normal = normal_buffer[i];
            float diffuse = std::max(0.0f, glm::dot(normal, light_dir));
            float ambient = 0.3f;
            float lighting = ambient + (1.0f - ambient) * diffuse;

            color = object_color * lighting;
        } else {
            color = bg_color;
        }

        image[i * 3 + 0] = static_cast<uint8_t>(std::clamp(color.r * 255.0f, 0.0f, 255.0f));
        image[i * 3 + 1] = static_cast<uint8_t>(std::clamp(color.g * 255.0f, 0.0f, 255.0f));
        image[i * 3 + 2] = static_cast<uint8_t>(std::clamp(color.b * 255.0f, 0.0f, 255.0f));
    }

    int result = stbi_write_png(output_path.c_str(), width, height, 3, image.data(), width * 3);

    if (result == 0) {
        spdlog::error("Failed to write PNG to {}", output_path);
        return false;
    }

    spdlog::info("Successfully wrote preview to {}", output_path);
    return true;
}

size_t SparseVoxelGrid::getMemoryUsage() const {
    // Approximate memory usage
    size_t voxel_map_size =
        voxels_.size() * (sizeof(glm::ivec3) + sizeof(float) + sizeof(void*) * 2);
    return voxel_map_size;
}

void SparseVoxelGrid::getBounds(glm::vec3& min_bound, glm::vec3& max_bound) const {
    min_bound = min_bound_;
    max_bound = max_bound_;
}

void SparseVoxelGrid::clear() {
    voxels_.clear();
    min_bound_ = glm::vec3(FLT_MAX);
    max_bound_ = glm::vec3(-FLT_MAX);
}

// ============================================================================
// GCodeParser Implementation
// ============================================================================

GCodeParser::GCodeParser() {}

bool GCodeParser::extractParam(const std::string& line, char param, float& value) const {
    size_t pos = line.find(param);
    if (pos == std::string::npos) {
        return false;
    }

    // Skip the parameter letter
    pos++;

    // Parse number
    std::string num_str;
    while (pos < line.size() && (std::isdigit(line[pos]) || line[pos] == '.' || line[pos] == '-' ||
                                 line[pos] == '+')) {
        num_str += line[pos++];
    }

    if (num_str.empty()) {
        return false;
    }

    try {
        value = std::stof(num_str);
        return true;
    } catch (...) {
        return false;
    }
}

void GCodeParser::parseLine(const std::string& line,
                            std::vector<std::pair<glm::vec3, glm::vec3>>& segments) {
    // Remove comments
    std::string clean_line = line;
    size_t comment_pos = clean_line.find(';');
    if (comment_pos != std::string::npos) {
        clean_line = clean_line.substr(0, comment_pos);
    }

    // Trim whitespace
    clean_line.erase(0, clean_line.find_first_not_of(" \t\r\n"));
    clean_line.erase(clean_line.find_last_not_of(" \t\r\n") + 1);

    if (clean_line.empty()) {
        return;
    }

    // Check for mode changes
    if (clean_line.find("G90") == 0) {
        state_.absolute_mode = true;
        return;
    } else if (clean_line.find("G91") == 0) {
        state_.absolute_mode = false;
        return;
    } else if (clean_line.find("M82") == 0) {
        state_.absolute_e_mode = true;
        return;
    } else if (clean_line.find("M83") == 0) {
        state_.absolute_e_mode = false;
        return;
    }

    // Parse G0/G1 moves
    if (clean_line.find("G0") == 0 || clean_line.find("G1") == 0 || clean_line.find("G2") == 0 ||
        clean_line.find("G3") == 0) {

        glm::vec3 new_pos = state_.position;
        float new_e = state_.e_position;

        float x, y, z, e;
        bool has_x = extractParam(clean_line, 'X', x);
        bool has_y = extractParam(clean_line, 'Y', y);
        bool has_z = extractParam(clean_line, 'Z', z);
        bool has_e = extractParam(clean_line, 'E', e);

        if (state_.absolute_mode) {
            if (has_x)
                new_pos.x = x;
            if (has_y)
                new_pos.y = y;
            if (has_z)
                new_pos.z = z;
        } else {
            if (has_x)
                new_pos.x += x;
            if (has_y)
                new_pos.y += y;
            if (has_z)
                new_pos.z += z;
        }

        if (has_e) {
            if (state_.absolute_e_mode) {
                new_e = e;
            } else {
                new_e += e;
            }
        }

        // Check if this is an extrusion move (E is increasing)
        float e_delta = new_e - state_.e_position;

        if (e_delta > 0.001f) { // Positive extrusion (not retraction)
            segments.push_back({state_.position, new_pos});
            extrusion_lines_++;
            total_length_ += glm::length(new_pos - state_.position);
        }

        state_.position = new_pos;
        state_.e_position = new_e;
    }
}

std::vector<std::pair<glm::vec3, glm::vec3>> GCodeParser::parseExtrusions(
    const std::string& filepath) {
    spdlog::info("Parsing G-code file: {}", filepath);

    std::vector<std::pair<glm::vec3, glm::vec3>> segments;
    std::ifstream file(filepath);

    if (!file.is_open()) {
        spdlog::error("Failed to open G-code file: {}", filepath);
        return segments;
    }

    // Reset state
    state_ = ParserState();
    total_lines_ = 0;
    extrusion_lines_ = 0;
    total_length_ = 0.0f;

    std::string line;
    while (std::getline(file, line)) {
        total_lines_++;
        parseLine(line, segments);

        // Progress logging every 10000 lines
        if (total_lines_ % 10000 == 0) {
            spdlog::debug("Parsed {} lines, {} extrusion segments", total_lines_, segments.size());
        }
    }

    spdlog::info("Parsing complete: {} total lines, {} extrusion segments, {:.2f}mm total length",
                 total_lines_, extrusion_lines_, total_length_);

    return segments;
}

void GCodeParser::getStats(size_t& total_lines, size_t& extrusion_lines,
                           float& total_length) const {
    total_lines = total_lines_;
    extrusion_lines = extrusion_lines_;
    total_length = total_length_;
}

// ============================================================================
// High-Level API
// ============================================================================

bool generateGCodePreview(const std::string& gcode_path, const std::string& output_path,
                          const VoxelPreviewConfig& config) {
    spdlog::info("Generating G-code preview: {} -> {}", gcode_path, output_path);

    // Parse G-code
    GCodeParser parser;
    auto segments = parser.parseExtrusions(gcode_path);

    if (segments.empty()) {
        spdlog::error("No extrusion segments found in G-code file");
        return false;
    }

    size_t total_lines, extrusion_lines;
    float total_length;
    parser.getStats(total_lines, extrusion_lines, total_length);

    // Create voxel grid
    SparseVoxelGrid grid(config.resolution);

    spdlog::info("Voxelizing {} segments...", segments.size());
    for (const auto& seg : segments) {
        grid.addSegment(seg.first, seg.second, 0.4f);
    }

    size_t voxel_count = grid.getVoxelCount();
    size_t memory_usage = grid.getMemoryUsage();
    spdlog::info("Voxelization complete: {} voxels, {:.2f} MB", voxel_count,
                 memory_usage / (1024.0f * 1024.0f));

    // Apply Gaussian blur
    grid.applyGaussianBlur(config.blur_sigma, config.chunk_size);

    // Render to image
    bool success = grid.renderToImage(output_path, CameraAngle::ISOMETRIC, config.output_width,
                                      config.output_height);

    if (success) {
        spdlog::info("Preview generation successful");
    } else {
        spdlog::error("Preview generation failed");
    }

    return success;
}
