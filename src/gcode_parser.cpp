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

#include "gcode_parser.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace gcode {

// ============================================================================
// ParsedGCodeFile Methods
// ============================================================================

int ParsedGCodeFile::find_layer_at_z(float z) const {
    if (layers.empty()) {
        return -1;
    }

    // Binary search for closest Z height
    int left = 0;
    int right = static_cast<int>(layers.size()) - 1;
    int closest = 0;
    float min_diff = std::abs(layers[0].z_height - z);

    while (left <= right) {
        int mid = left + (right - left) / 2;
        float diff = std::abs(layers[mid].z_height - z);

        if (diff < min_diff) {
            min_diff = diff;
            closest = mid;
        }

        if (layers[mid].z_height < z) {
            left = mid + 1;
        } else if (layers[mid].z_height > z) {
            right = mid - 1;
        } else {
            return mid; // Exact match
        }
    }

    return closest;
}

// ============================================================================
// GCodeParser Implementation
// ============================================================================

GCodeParser::GCodeParser() {
    reset();
}

void GCodeParser::reset() {
    current_position_ = glm::vec3(0.0f, 0.0f, 0.0f);
    current_e_ = 0.0f;
    current_object_.clear();
    is_absolute_positioning_ = true;
    is_absolute_extrusion_ = true;
    layers_.clear();
    objects_.clear();
    global_bounds_ = AABB();
    lines_parsed_ = 0;

    // Start with first layer at Z=0
    start_new_layer(0.0f);
}

void GCodeParser::parse_line(const std::string& line) {
    lines_parsed_++;

    std::string trimmed = trim_line(line);
    if (trimmed.empty()) {
        return;
    }

    // Check for EXCLUDE_OBJECT commands first
    if (trimmed.find("EXCLUDE_OBJECT") == 0) {
        parse_exclude_object_command(trimmed);
        return;
    }

    // Parse positioning mode commands
    if (trimmed == "G90") {
        is_absolute_positioning_ = true;
        return;
    } else if (trimmed == "G91") {
        is_absolute_positioning_ = false;
        return;
    } else if (trimmed == "M82") {
        is_absolute_extrusion_ = true;
        return;
    } else if (trimmed == "M83") {
        is_absolute_extrusion_ = false;
        return;
    }

    // Parse movement commands (G0, G1)
    if (trimmed[0] == 'G' && (trimmed.find("G0 ") == 0 || trimmed.find("G1 ") == 0 ||
                              trimmed == "G0" || trimmed == "G1")) {
        parse_movement_command(trimmed);
    }
}

bool GCodeParser::parse_movement_command(const std::string& line) {
    glm::vec3 new_position = current_position_;
    float new_e = current_e_;
    bool has_movement = false;
    bool has_extrusion = false;

    // Extract X, Y, Z parameters
    float value;
    if (extract_param(line, 'X', value)) {
        new_position.x = is_absolute_positioning_ ? value : current_position_.x + value;
        has_movement = true;
    }
    if (extract_param(line, 'Y', value)) {
        new_position.y = is_absolute_positioning_ ? value : current_position_.y + value;
        has_movement = true;
    }
    if (extract_param(line, 'Z', value)) {
        new_position.z = is_absolute_positioning_ ? value : current_position_.z + value;
        has_movement = true;

        // Layer change detected
        if (std::abs(new_position.z - current_position_.z) > 0.001f) {
            start_new_layer(new_position.z);
        }
    }

    // Extract E (extrusion) parameter
    if (extract_param(line, 'E', value)) {
        new_e = is_absolute_extrusion_ ? value : current_e_ + value;
        has_extrusion = true;
    }

    // Add segment if there's XY movement
    if (has_movement &&
        (new_position.x != current_position_.x || new_position.y != current_position_.y)) {
        // Determine if this is an extrusion move
        bool is_extruding = false;
        if (has_extrusion) {
            float e_delta = new_e - current_e_;
            is_extruding = (e_delta > 0.00001f); // Small threshold for floating point
        }

        add_segment(current_position_, new_position, is_extruding);
    }

    // Update state
    current_position_ = new_position;
    if (has_extrusion) {
        current_e_ = new_e;
    }

    return has_movement;
}

bool GCodeParser::parse_exclude_object_command(const std::string& line) {
    // EXCLUDE_OBJECT_DEFINE NAME=... CENTER=... POLYGON=...
    if (line.find("EXCLUDE_OBJECT_DEFINE") == 0) {
        std::string name;
        if (!extract_string_param(line, "NAME", name)) {
            return false;
        }

        GCodeObject obj;
        obj.name = name;

        // Extract CENTER (format: "X,Y")
        std::string center_str;
        if (extract_string_param(line, "CENTER", center_str)) {
            size_t comma = center_str.find(',');
            if (comma != std::string::npos) {
                try {
                    obj.center.x = std::stof(center_str.substr(0, comma));
                    obj.center.y = std::stof(center_str.substr(comma + 1));
                } catch (...) {
                    spdlog::warn("Failed to parse CENTER for object: {}", name);
                }
            }
        }

        // Extract POLYGON (format: "[[x1,y1],[x2,y2],...]")
        // For now, we'll do basic parsing - full JSON parsing would be better
        std::string polygon_str;
        if (extract_string_param(line, "POLYGON", polygon_str)) {
            // Simple extraction of number pairs
            std::istringstream iss(polygon_str);
            char ch;
            float x, y;
            while (iss >> ch) {
                if (ch == '[') {
                    if (iss >> x >> ch && ch == ',' && iss >> y >> ch && ch == ']') {
                        obj.polygon.push_back(glm::vec2(x, y));
                    }
                }
            }
        }

        objects_[name] = obj;
        spdlog::debug("Defined object: {} at ({}, {})", name, obj.center.x, obj.center.y);
        return true;
    }
    // EXCLUDE_OBJECT_START NAME=...
    else if (line.find("EXCLUDE_OBJECT_START") == 0) {
        if (!extract_string_param(line, "NAME", current_object_)) {
            current_object_.clear();
            return false;
        }
        spdlog::debug("Started object: {}", current_object_);
        return true;
    }
    // EXCLUDE_OBJECT_END NAME=...
    else if (line.find("EXCLUDE_OBJECT_END") == 0) {
        std::string name;
        if (extract_string_param(line, "NAME", name) && name == current_object_) {
            spdlog::debug("Ended object: {}", current_object_);
            current_object_.clear();
            return true;
        }
    }

    return false;
}

bool GCodeParser::extract_param(const std::string& line, char param, float& out_value) {
    size_t pos = line.find(param);
    if (pos == std::string::npos) {
        return false;
    }

    // Make sure it's a parameter (preceded by space or at start after command)
    if (pos > 0 && line[pos - 1] != ' ' && line[pos - 1] != '\t') {
        return false;
    }

    // Extract number after parameter letter
    size_t start = pos + 1;
    if (start >= line.length()) {
        return false;
    }

    // Find end of number (space, end of string, or another letter)
    size_t end = start;
    while (end < line.length() &&
           (std::isdigit(line[end]) || line[end] == '.' || line[end] == '-' || line[end] == '+')) {
        end++;
    }

    if (end == start) {
        return false;
    }

    try {
        out_value = std::stof(line.substr(start, end - start));
        return true;
    } catch (...) {
        return false;
    }
}

bool GCodeParser::extract_string_param(const std::string& line, const std::string& param,
                                       std::string& out_value) {
    size_t pos = line.find(param + "=");
    if (pos == std::string::npos) {
        return false;
    }

    size_t start = pos + param.length() + 1; // Skip "PARAM="
    if (start >= line.length()) {
        return false;
    }

    // Find end of value (space or end of line)
    size_t end = line.find(' ', start);
    if (end == std::string::npos) {
        end = line.length();
    }

    out_value = line.substr(start, end - start);
    return true;
}

void GCodeParser::add_segment(const glm::vec3& start, const glm::vec3& end, bool is_extrusion) {
    if (layers_.empty()) {
        start_new_layer(start.z);
    }

    ToolpathSegment segment;
    segment.start = start;
    segment.end = end;
    segment.is_extrusion = is_extrusion;
    segment.object_name = current_object_;
    segment.extrusion_amount = 0.0f; // TODO: Calculate from E delta

    // Update layer data
    Layer& current_layer = layers_.back();
    current_layer.segments.push_back(segment);
    current_layer.bounding_box.expand(start);
    current_layer.bounding_box.expand(end);

    if (is_extrusion) {
        current_layer.segment_count_extrusion++;
    } else {
        current_layer.segment_count_travel++;
    }

    // Update global bounds
    global_bounds_.expand(start);
    global_bounds_.expand(end);

    // Update object bounding box
    if (!current_object_.empty() && objects_.count(current_object_) > 0) {
        objects_[current_object_].bounding_box.expand(start);
        objects_[current_object_].bounding_box.expand(end);
    }
}

void GCodeParser::start_new_layer(float z) {
    // Don't create duplicate layers at same Z
    if (!layers_.empty() && std::abs(layers_.back().z_height - z) < 0.001f) {
        return;
    }

    Layer layer;
    layer.z_height = z;
    layers_.push_back(layer);

    spdlog::debug("Started layer {} at Z={:.3f}", layers_.size() - 1, z);
}

std::string GCodeParser::trim_line(const std::string& line) {
    if (line.empty()) {
        return line;
    }

    // Remove comments (everything after ';')
    size_t comment_pos = line.find(';');
    std::string without_comment =
        (comment_pos != std::string::npos) ? line.substr(0, comment_pos) : line;

    // Trim leading/trailing whitespace
    size_t start = 0;
    while (start < without_comment.length() && std::isspace(without_comment[start])) {
        start++;
    }

    if (start == without_comment.length()) {
        return "";
    }

    size_t end = without_comment.length();
    while (end > start && std::isspace(without_comment[end - 1])) {
        end--;
    }

    return without_comment.substr(start, end - start);
}

ParsedGCodeFile GCodeParser::finalize() {
    ParsedGCodeFile result;
    result.filename = "";
    result.layers = std::move(layers_);
    result.objects = std::move(objects_);
    result.global_bounding_box = global_bounds_;

    // Calculate statistics
    for (const auto& layer : result.layers) {
        result.total_segments += layer.segments.size();
    }

    spdlog::info("Parsed G-code: {} layers, {} segments, {} objects", result.layers.size(),
                 result.total_segments, result.objects.size());

    // Reset state for potential reuse
    reset();

    return result;
}

} // namespace gcode
