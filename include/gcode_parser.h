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

#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

/**
 * @file gcode_parser.h
 * @brief Streaming G-code parser for 3D printer toolpath extraction
 *
 * This parser incrementally processes G-code files line-by-line, extracting
 * movement commands (G0/G1) and building a layer-indexed representation of
 * the toolpath. It also parses Klipper EXCLUDE_OBJECT metadata for object
 * exclusion support.
 *
 * Design goals:
 * - Streaming: Process large files without full buffering
 * - Layer-indexed: Fast access to specific Z-height layers
 * - Object-aware: Track segments belonging to named objects
 * - Efficient: Minimal memory overhead per segment (~32 bytes)
 *
 * @see docs/GCODE_VISUALIZATION.md for complete design
 */

namespace gcode {

/**
 * @brief Axis-aligned bounding box for spatial queries
 */
struct AABB {
    glm::vec3 min{0.0f, 0.0f, 0.0f};
    glm::vec3 max{0.0f, 0.0f, 0.0f};

    /**
     * @brief Get center point of bounding box
     * @return Center coordinate
     */
    glm::vec3 center() const {
        return (min + max) * 0.5f;
    }

    /**
     * @brief Get size (dimensions) of bounding box
     * @return Size vector (width, depth, height)
     */
    glm::vec3 size() const {
        return max - min;
    }

    /**
     * @brief Expand bounding box to include a point
     * @param point Point to include
     */
    void expand(const glm::vec3& point) {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    /**
     * @brief Check if bounding box is empty (not initialized)
     * @return true if empty (min == max)
     */
    bool is_empty() const {
        return min == max;
    }
};

/**
 * @brief Single toolpath segment (line segment in 3D space)
 *
 * Represents movement from start to end point. Can be either:
 * - Extrusion move (is_extrusion=true): Plastic is deposited
 * - Travel move (is_extrusion=false): Nozzle moves without extruding
 */
struct ToolpathSegment {
    glm::vec3 start{0.0f, 0.0f, 0.0f}; ///< Start point (X, Y, Z)
    glm::vec3 end{0.0f, 0.0f, 0.0f};   ///< End point (X, Y, Z)
    bool is_extrusion{false};          ///< true if extruding, false if travel move
    std::string object_name;           ///< Object name (from EXCLUDE_OBJECT_START) or
                                       ///< empty
    float extrusion_amount{0.0f};      ///< E-axis delta (for future use)
};

/**
 * @brief Single layer of toolpath (constant Z-height)
 *
 * Contains all segments at a specific Z coordinate. Layers are indexed
 * sequentially from 0 (first layer) to N-1 (top layer).
 */
struct Layer {
    float z_height{0.0f};                  ///< Z coordinate of this layer
    std::vector<ToolpathSegment> segments; ///< All segments in layer
    AABB bounding_box;                     ///< Precomputed spatial bounds
    size_t segment_count_extrusion{0};     ///< Count of extrusion moves
    size_t segment_count_travel{0};        ///< Count of travel moves
};

/**
 * @brief Object metadata from EXCLUDE_OBJECT_DEFINE command
 *
 * Represents a named object in the print (e.g., "part_1", "support_3").
 * Used for Klipper's exclude objects feature.
 */
struct GCodeObject {
    std::string name;               ///< Object identifier
    glm::vec2 center{0.0f, 0.0f};   ///< Center point (X, Y)
    std::vector<glm::vec2> polygon; ///< Boundary polygon points
    AABB bounding_box;              ///< 3D bounding box
    bool is_excluded{false};        ///< User exclusion state (local UI state)
};

/**
 * @brief Parsed G-code file with layer-indexed toolpath data
 *
 * Final output of the parser. Contains all layers, objects, and metadata
 * needed for visualization and analysis.
 */
struct ParsedGCodeFile {
    std::string filename;                       ///< Source filename
    std::vector<Layer> layers;                  ///< Indexed by layer number
    std::map<std::string, GCodeObject> objects; ///< Object metadata (name → object)
    AABB global_bounding_box;                   ///< Bounds of entire model

    // Statistics
    size_t total_segments{0};                 ///< Total segment count
    float estimated_print_time_minutes{0.0f}; ///< From metadata (if available)
    float total_filament_mm{0.0f};            ///< From metadata (if available)

    /**
     * @brief Get layer at specific index
     * @param index Layer index (0-based)
     * @return Pointer to layer or nullptr if out of range
     */
    const Layer* get_layer(size_t index) const {
        return (index < layers.size()) ? &layers[index] : nullptr;
    }

    /**
     * @brief Find layer closest to Z height
     * @param z Z coordinate to search for
     * @return Layer index or -1 if no layers
     */
    int find_layer_at_z(float z) const;
};

/**
 * @brief Streaming G-code parser
 *
 * Usage pattern:
 * @code
 *   GCodeParser parser;
 *   std::ifstream file("model.gcode");
 *   std::string line;
 *   while (std::getline(file, line)) {
 *       parser.parse_line(line);
 *   }
 *   ParsedGCodeFile result = parser.finalize();
 * @endcode
 *
 * The parser maintains state across parse_line() calls and accumulates
 * data. Call finalize() once when complete to get the final result.
 */
class GCodeParser {
  public:
    GCodeParser();
    ~GCodeParser() = default;

    /**
     * @brief Parse single line of G-code
     * @param line Raw G-code line (may include comments)
     *
     * Extracts movement commands, coordinate changes, and object metadata.
     * Automatically detects layer changes (Z-axis movement).
     */
    void parse_line(const std::string& line);

    /**
     * @brief Finalize parsing and return complete data structure
     * @return Parsed file with all layers and objects
     *
     * Call this after all lines have been parsed. Clears internal state.
     */
    ParsedGCodeFile finalize();

    /**
     * @brief Reset parser state for new file
     *
     * Clears all accumulated data. Use when parsing multiple files
     * with the same parser instance.
     */
    void reset();

    // Progress tracking

    /**
     * @brief Get number of lines parsed so far
     * @return Line count
     */
    size_t lines_parsed() const {
        return lines_parsed_;
    }

    /**
     * @brief Get current Z coordinate
     * @return Current Z position in mm
     */
    float current_z() const {
        return current_position_.z;
    }

    /**
     * @brief Get current layer index
     * @return Layer number (0-based)
     */
    size_t current_layer() const {
        return layers_.size() - 1;
    }

  private:
    // Parsing helpers

    /**
     * @brief Parse movement command (G0, G1)
     * @param line Trimmed G-code line
     * @return true if parsed successfully
     */
    bool parse_movement_command(const std::string& line);

    /**
     * @brief Parse EXCLUDE_OBJECT_* command
     * @param line Trimmed G-code line
     * @return true if parsed successfully
     */
    bool parse_exclude_object_command(const std::string& line);

    /**
     * @brief Extract parameter value from G-code line
     * @param line G-code line
     * @param param Parameter letter (e.g., 'X', 'Y', 'Z')
     * @param out_value Output value
     * @return true if parameter found
     */
    bool extract_param(const std::string& line, char param, float& out_value);

    /**
     * @brief Extract string parameter value
     * @param line G-code line
     * @param param Parameter name (e.g., "NAME")
     * @param out_value Output string
     * @return true if parameter found
     */
    bool extract_string_param(const std::string& line, const std::string& param,
                              std::string& out_value);

    /**
     * @brief Add toolpath segment to current layer
     * @param start Start point
     * @param end End point
     * @param is_extrusion true if extruding
     */
    void add_segment(const glm::vec3& start, const glm::vec3& end, bool is_extrusion);

    /**
     * @brief Start new layer at given Z height
     * @param z Z coordinate
     */
    void start_new_layer(float z);

    /**
     * @brief Trim whitespace and comments from line
     * @param line Raw line
     * @return Trimmed line
     */
    std::string trim_line(const std::string& line);

    // Parser state
    glm::vec3 current_position_{0.0f, 0.0f, 0.0f}; ///< Current XYZ position
    float current_e_{0.0f};                        ///< Current E (extruder) position
    std::string current_object_;         ///< Current object name (from EXCLUDE_OBJECT_START)
    bool is_absolute_positioning_{true}; ///< G90 (absolute) vs G91 (relative)
    bool is_absolute_extrusion_{true};   ///< M82 (absolute E) vs M83 (relative E)

    // Accumulated data
    std::vector<Layer> layers_;                  ///< All parsed layers
    std::map<std::string, GCodeObject> objects_; ///< Object metadata
    AABB global_bounds_;                         ///< Global bounding box

    // Progress tracking
    size_t lines_parsed_{0}; ///< Line counter
};

} // namespace gcode
