// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_parser.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace helix {
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

    constexpr float epsilon = 0.0001f; // Tolerance for floating point comparison

    while (left <= right) {
        int mid = left + (right - left) / 2;
        float diff = std::abs(layers[static_cast<size_t>(mid)].z_height - z);

        // Update closest if this is better, or if equal distance but prefer lower Z height
        if (diff < min_diff || (std::abs(diff - min_diff) < epsilon &&
                                layers[static_cast<size_t>(mid)].z_height <
                                    layers[static_cast<size_t>(closest)].z_height)) {
            min_diff = diff;
            closest = mid;
        }

        if (layers[static_cast<size_t>(mid)].z_height < z) {
            left = mid + 1;
        } else if (layers[static_cast<size_t>(mid)].z_height > z) {
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
    out_of_range_width_count_ = 0;

    // Layers will be created on-demand when segments are added
    // (see add_segment() which creates a layer if layers_ is empty)
}

void GCodeParser::parse_line(const std::string& line) {
    lines_parsed_++;

    // Extract and parse metadata comments before trimming
    size_t comment_pos = line.find(';');
    if (comment_pos != std::string::npos) {
        std::string comment = line.substr(comment_pos);
        parse_metadata_comment(comment);
        parse_wipe_tower_marker(comment);
    }

    std::string trimmed = trim_line(line);
    if (trimmed.empty()) {
        return;
    }

    // Check for tool changes (T0, T1, T2, etc.)
    if (!trimmed.empty() && trimmed[0] == 'T') {
        parse_tool_change_command(trimmed);
        // Continue processing - some G-code files have commands after tool changes
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

        // Layer change detection:
        // If we have LAYER_CHANGE markers, only start a new layer when we see one
        // Otherwise fall back to Z-based detection (for older G-code without markers)
        if (std::abs(new_position.z - current_position_.z) > 0.001f) {
            if (use_layer_markers_) {
                // Layer marker mode: only start layer if marker was seen
                if (pending_layer_marker_) {
                    start_new_layer(new_position.z);
                    pending_layer_marker_ = false;
                }
                // Otherwise ignore Z movement (it's a z-hop or adjustment)
            } else {
                // Legacy mode: every Z change is a new layer
                start_new_layer(new_position.z);
            }
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
        float e_delta = 0.0f;
        if (has_extrusion) {
            e_delta = new_e - current_e_;
            is_extruding = (e_delta > 0.00001f); // Small threshold for floating point
        }

        add_segment(current_position_, new_position, is_extruding, e_delta);
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
                    // Internal parsing error - no user notification needed
                    spdlog::debug("[GCode Parser] Failed to parse CENTER for object: {}", name);
                }
            }
        }

        // Extract POLYGON (format: "[[x1,y1],[x2,y2],...]")
        // For now, we'll do basic parsing - full JSON parsing would be better
        std::string polygon_str;
        if (extract_string_param(line, "POLYGON", polygon_str)) {
            // Simple extraction of number pairs
            // Remove all whitespace first for easier parsing
            polygon_str.erase(std::remove_if(polygon_str.begin(), polygon_str.end(), ::isspace),
                              polygon_str.end());

            // Skip outer opening bracket if present
            size_t pos = 0;
            if (!polygon_str.empty() && polygon_str[0] == '[') {
                pos = 1;
            }

            while (pos < polygon_str.length()) {
                // Find opening bracket for this point
                if (polygon_str[pos] == '[') {
                    pos++;
                    // Extract x coordinate (everything until comma)
                    size_t comma = polygon_str.find(',', pos);
                    if (comma != std::string::npos) {
                        try {
                            float x = std::stof(polygon_str.substr(pos, comma - pos));
                            pos = comma + 1;

                            // Extract y coordinate (everything until closing bracket)
                            size_t close = polygon_str.find(']', pos);
                            if (close != std::string::npos) {
                                float y = std::stof(polygon_str.substr(pos, close - pos));
                                obj.polygon.push_back(glm::vec2(x, y));
                                pos = close + 1;
                                spdlog::trace("[GCode Parser] Parsed polygon point: ({}, {})", x,
                                              y);
                            } else {
                                break;
                            }
                        } catch (...) {
                            break;
                        }
                    } else {
                        break;
                    }
                } else {
                    pos++;
                }
            }
        }

        objects_[name] = obj;
        spdlog::trace("[GCode Parser] Defined object: {} at ({}, {})", name, obj.center.x,
                      obj.center.y);
        return true;
    }
    // EXCLUDE_OBJECT_START NAME=...
    else if (line.find("EXCLUDE_OBJECT_START") == 0) {
        if (!extract_string_param(line, "NAME", current_object_)) {
            current_object_.clear();
            return false;
        }
        spdlog::trace("[GCode Parser] Started object: {}", current_object_);
        return true;
    }
    // EXCLUDE_OBJECT_END NAME=...
    else if (line.find("EXCLUDE_OBJECT_END") == 0) {
        std::string name;
        if (extract_string_param(line, "NAME", name) && name == current_object_) {
            spdlog::trace("[GCode Parser] Ended object: {}", current_object_);
            current_object_.clear();
            return true;
        }
    }

    return false;
}

void GCodeParser::parse_metadata_comment(const std::string& line) {
    // OrcaSlicer/PrusaSlicer format: "; key = value"
    // Use fuzzy matching to handle variations across slicers

    if (line.length() < 2 || line[0] != ';') {
        return;
    }

    // Check for layer change markers FIRST (before key=value parsing)
    // Common formats: ";LAYER_CHANGE", ";LAYER:N", "; LAYER_CHANGE"
    std::string content_upper = line.substr(1);
    // Remove leading whitespace for comparison
    size_t ws_start = 0;
    while (ws_start < content_upper.length() && std::isspace(content_upper[ws_start])) {
        ws_start++;
    }
    content_upper = content_upper.substr(ws_start);
    std::transform(content_upper.begin(), content_upper.end(), content_upper.begin(), ::toupper);

    // Detect layer change markers (but not LAYER_COUNT which is metadata)
    if (content_upper.find("LAYER_CHANGE") == 0 || content_upper.find("LAYER:") == 0) {
        // Mark that we found layer markers (prefer this over Z-based detection)
        use_layer_markers_ = true;
        pending_layer_marker_ = true;
        spdlog::trace("[GCode Parser] Layer marker detected: '{}' (use_markers={}, pending={})",
                      line, use_layer_markers_, pending_layer_marker_);
        return; // Don't process as key=value metadata
    }

    // Skip '; ' to get key=value or key: value part
    std::string content = line.substr(1);

    // Trim leading whitespace
    size_t start = 0;
    while (start < content.length() && std::isspace(content[start])) {
        start++;
    }
    content = content.substr(start);

    // Look for '=' or ':' separator (support both OrcaSlicer and PrusaSlicer formats)
    size_t eq_pos = content.find('=');
    size_t colon_pos = content.find(':');
    size_t sep_pos = std::string::npos;

    // Prefer '=' if present and before any ':', otherwise use ':'
    if (eq_pos != std::string::npos && (colon_pos == std::string::npos || eq_pos < colon_pos)) {
        sep_pos = eq_pos;
    } else if (colon_pos != std::string::npos) {
        sep_pos = colon_pos;
    }

    if (sep_pos == std::string::npos) {
        return;
    }

    // Extract key and value
    std::string key = content.substr(0, sep_pos);
    std::string value = content.substr(sep_pos + 1);

    // Trim whitespace from key and value
    auto trim = [](std::string& s) {
        size_t start = 0;
        while (start < s.length() && std::isspace(s[start]))
            start++;
        size_t end = s.length();
        while (end > start && std::isspace(s[end - 1]))
            end--;
        s = s.substr(start, end - start);
    };
    trim(key);
    trim(value);

    // Convert key to lowercase for case-insensitive matching
    std::string key_lower = key;
    std::transform(key_lower.begin(), key_lower.end(), key_lower.begin(), ::tolower);

    // Helper to check if key contains all substrings (fuzzy match)
    auto contains_all = [&key_lower](std::initializer_list<const char*> terms) {
        for (const char* term : terms) {
            if (key_lower.find(term) == std::string::npos) {
                return false;
            }
        }
        return true;
    };

    // Parse specific metadata fields with fuzzy matching
    // Multi-color: Check for extruder_colour first (priority over single filament_colour)
    if (key_lower.find("extruder_colour") != std::string::npos ||
        key_lower.find("extruder_color") != std::string::npos) {
        parse_extruder_color_metadata(line);
    }
    // Fallback: Parse single filament_colour if extruder_colour not yet found
    else if (contains_all({"filament", "col"}) && tool_color_palette_.empty()) {
        // Check if it's a semicolon-separated list (multi-color)
        if (value.find(';') != std::string::npos) {
            parse_extruder_color_metadata(line);
        } else {
            // Single color metadata
            metadata_filament_color_ = value;
            spdlog::trace("[GCode Parser] Parsed single filament color: {}", value);
        }
    } else if (contains_all({"filament", "type"})) {
        metadata_filament_type_ = value;
        spdlog::trace("[GCode Parser] Parsed filament type: {}", value);
    } else if (contains_all({"printer", "model"}) || contains_all({"printer", "name"})) {
        metadata_printer_model_ = value;
        spdlog::trace("[GCode Parser] Parsed printer model: {}", value);
    } else if (contains_all({"nozzle", "diameter"})) {
        try {
            metadata_nozzle_diameter_ = std::stof(value);
            spdlog::trace("[GCode Parser] Parsed nozzle diameter: {}mm", metadata_nozzle_diameter_);
        } catch (...) {
        }
    } else if (contains_all({"filament"}) &&
               (key_lower.find("[mm]") != std::string::npos || contains_all({"length"}))) {
        try {
            metadata_filament_length_ = std::stof(value);
            spdlog::trace("[GCode Parser] Parsed filament length: {}mm", metadata_filament_length_);
        } catch (...) {
        }
    } else if (contains_all({"filament"}) &&
               (key_lower.find("[g]") != std::string::npos || contains_all({"weight"}))) {
        try {
            metadata_filament_weight_ = std::stof(value);
            spdlog::trace("[GCode Parser] Parsed filament weight: {}g", metadata_filament_weight_);
        } catch (...) {
        }
    } else if (contains_all({"filament", "cost"}) || contains_all({"material", "cost"})) {
        try {
            metadata_filament_cost_ = std::stof(value);
            spdlog::trace("[GCode Parser] Parsed filament cost: ${}", metadata_filament_cost_);
        } catch (...) {
        }
    } else if (contains_all({"layer"}) && contains_all({"total"}) &&
               (contains_all({"number"}) || contains_all({"count"}) ||
                key_lower.find("total layer") != std::string::npos)) {
        // Match "total layer number", "total layers count", but NOT "interlocking_beam_layer_count"
        try {
            metadata_layer_count_ = std::stoi(value);
            spdlog::trace("[GCode Parser] Parsed total layer count: {}", metadata_layer_count_);
        } catch (...) {
        }
    } else if ((contains_all({"time"}) &&
                (contains_all({"print"}) || contains_all({"estimated"}))) ||
               contains_all({"print", "time"})) {
        // Parse various time formats: "29m 25s", "1h 23m", "45s", etc.
        float minutes = 0.0f;

        // Try to find hours
        size_t h_pos = value.find('h');
        if (h_pos != std::string::npos) {
            try {
                float hours = std::stof(value.substr(0, h_pos));
                minutes += hours * 60.0f;
            } catch (...) {
            }
        }

        // Try to find minutes
        size_t m_pos = value.find('m');
        if (m_pos != std::string::npos) {
            try {
                size_t start_pos = (h_pos != std::string::npos) ? h_pos + 1 : 0;
                std::string min_str = value.substr(start_pos, m_pos - start_pos);
                // Trim spaces
                size_t min_start = 0;
                while (min_start < min_str.length() && std::isspace(min_str[min_start]))
                    min_start++;
                if (min_start < min_str.length()) {
                    minutes += std::stof(min_str.substr(min_start));
                }
            } catch (...) {
            }
        }

        // Try to find seconds
        size_t s_pos = value.find('s');
        if (s_pos != std::string::npos) {
            try {
                size_t start_pos = (m_pos != std::string::npos)   ? m_pos + 1
                                   : (h_pos != std::string::npos) ? h_pos + 1
                                                                  : 0;
                std::string sec_str = value.substr(start_pos, s_pos - start_pos);
                // Trim spaces
                size_t sec_start = 0;
                while (sec_start < sec_str.length() && std::isspace(sec_str[sec_start]))
                    sec_start++;
                if (sec_start < sec_str.length()) {
                    float seconds = std::stof(sec_str.substr(sec_start));
                    minutes += seconds / 60.0f;
                }
            } catch (...) {
            }
        }

        if (minutes > 0.0f) {
            metadata_print_time_ = minutes;
            spdlog::trace("[GCode Parser] Parsed estimated time: {:.2f} minutes", minutes);
        }
    } else if (contains_all({"generated"}) || contains_all({"slicer"})) {
        metadata_slicer_name_ = value;
        spdlog::trace("[GCode Parser] Parsed slicer: {}", value);
    }
    // Parse layer height metadata (exact key match to avoid max_layer_height etc.)
    // OrcaSlicer/PrusaSlicer: "; layer_height = 0.2"
    // Cura: ";Layer height: 0.12"
    else if (key_lower == "layer_height" || key_lower == "layer height" ||
             key_lower == "first_layer_height" || key_lower == "first layer height") {
        std::string numeric_value = value;
        size_t mm_pos = numeric_value.find("mm");
        if (mm_pos != std::string::npos) {
            numeric_value = numeric_value.substr(0, mm_pos);
        }
        try {
            float h = std::stof(numeric_value);
            if (h > 0.01f && h < 2.0f) {
                if (key_lower.find("first") != std::string::npos) {
                    metadata_first_layer_height_ = h;
                    spdlog::trace("[GCode Parser] Parsed first layer height: {}mm", h);
                } else {
                    metadata_layer_height_ = h;
                    spdlog::trace("[GCode Parser] Parsed layer height: {}mm", h);
                }
            }
        } catch (...) {
        }
    }
    // Parse extrusion width metadata
    // OrcaSlicer/PrusaSlicer/SuperSlicer: "; perimeters extrusion width = 0.45mm"
    // Cura: ";SETTING_3 line_width = 0.4" or ";SETTING_3 wall_line_width_0 = 0.4"
    else if (contains_all({"extrusion", "width"}) ||
             (key_lower.find("line_width") != std::string::npos) ||
             (key_lower.find("linewidth") != std::string::npos)) {
        // Extract numeric value (handle "0.45mm" format and plain "0.4")
        std::string numeric_value = value;

        // Skip percentage values (e.g., "100%", "112.5%") — OrcaSlicer's settings dump
        // at end of file uses percentages of nozzle diameter, not absolute mm values.
        // These would overwrite the correct mm values from the header comments.
        if (numeric_value.find('%') != std::string::npos) {
            return;
        }

        // Remove "mm" suffix if present
        size_t mm_pos = numeric_value.find("mm");
        if (mm_pos != std::string::npos) {
            numeric_value = numeric_value.substr(0, mm_pos);
        }

        try {
            float width = std::stof(numeric_value);

            // Sanity check: extrusion widths should be 0.05mm to 3.0mm
            if (width < 0.05f || width > 3.0f) {
                spdlog::debug("[GCode Parser] Ignoring out-of-range extrusion width: {}mm", width);
                return;
            }

            // Categorize by feature type
            if (contains_all({"first", "layer"}) || contains_all({"initial", "layer"})) {
                metadata_first_layer_extrusion_width_ = width;
                spdlog::trace("[GCode Parser] Parsed first layer extrusion width: {}mm", width);
            } else if (contains_all({"perimeter"}) || key_lower.find("wall") != std::string::npos) {
                // Handles "perimeter" (Prusa/Orca) and "wall" (Cura)
                metadata_perimeter_extrusion_width_ = width;
                spdlog::trace("[GCode Parser] Parsed perimeter/wall extrusion width: {}mm", width);
            } else if (contains_all({"infill"})) {
                metadata_infill_extrusion_width_ = width;
                spdlog::trace("[GCode Parser] Parsed infill extrusion width: {}mm", width);
            } else {
                // General extrusion width (fallback for "line_width", etc.)
                if (metadata_extrusion_width_ == 0.0f) {
                    metadata_extrusion_width_ = width;
                    spdlog::trace("[GCode Parser] Parsed default extrusion width: {}mm", width);
                }
            }
        } catch (...) {
            // Failed to parse width value
        }
    }
}

void GCodeParser::parse_extruder_color_metadata(const std::string& line) {
    // Format: "; extruder_colour = #ED1C24;#00C1AE;#F4E2C1;#000000"
    //     OR: "; filament_colour = ..." (fallback)
    //     OR: ";extruder_colour=#AA0000 ; #00BB00 ;#0000CC" (with variations)

    // Find '=' character (with or without spaces)
    size_t eq_pos = line.find('=');
    if (eq_pos == std::string::npos) {
        return;
    }

    std::string colors_str = line.substr(eq_pos + 1);

    // Trim leading whitespace from colors_str
    size_t start = colors_str.find_first_not_of(" \t\r\n");
    if (start != std::string::npos) {
        colors_str = colors_str.substr(start);
    }

    // Split by semicolons
    std::stringstream ss(colors_str);
    std::string color;
    while (std::getline(ss, color, ';')) {
        // Trim whitespace
        color.erase(0, color.find_first_not_of(" \t\r\n"));
        color.erase(color.find_last_not_of(" \t\r\n") + 1);

        if (!color.empty() && color[0] == '#') {
            tool_color_palette_.push_back(color);
        } else if (!color.empty()) {
            // Non-empty but invalid format - use placeholder
            tool_color_palette_.push_back("");
        }
    }

    // Log color palette (manual join since fmt::join may not be available)
    std::string palette_str;
    for (size_t i = 0; i < tool_color_palette_.size(); ++i) {
        if (i > 0)
            palette_str += ", ";
        palette_str += tool_color_palette_[i];
    }
    spdlog::debug("[GCode Parser] Parsed {} extruder colors from metadata: [{}]",
                  tool_color_palette_.size(), palette_str);

    // Set metadata_filament_color_ to the first valid color (for single-color rendering fallback)
    if (!tool_color_palette_.empty() && !tool_color_palette_[0].empty()) {
        metadata_filament_color_ = tool_color_palette_[0];
    }
}

void GCodeParser::parse_tool_change_command(const std::string& line) {
    // Format: "T0", "T1", "T2", etc. (standalone line)
    if (line.empty() || line[0] != 'T') {
        return;
    }

    // Check if it's JUST "T" + digits (no other commands on line)
    if (line.length() < 2) {
        return;
    }

    // Extract tool number
    size_t i = 1;
    while (i < line.length() && std::isdigit(line[i])) {
        i++;
    }

    if (i == 1) {
        return; // No digits after T
    }
    if (i < line.length() && !std::isspace(line[i])) {
        return; // Not standalone
    }

    std::string tool_str = line.substr(1, i - 1);
    int tool_num = std::stoi(tool_str);

    current_tool_index_ = tool_num;
    spdlog::trace("[GCode Parser] Tool change: T{}", tool_num);
}

void GCodeParser::parse_wipe_tower_marker(const std::string& comment) {
    if (comment.find("WIPE_TOWER_START") != std::string::npos ||
        comment.find("WIPE_TOWER_BRIM_START") != std::string::npos) {
        in_wipe_tower_ = true;
        spdlog::debug("[GCode Parser] Entering wipe tower section");
    } else if (comment.find("WIPE_TOWER_END") != std::string::npos ||
               comment.find("WIPE_TOWER_BRIM_END") != std::string::npos) {
        in_wipe_tower_ = false;
        spdlog::debug("[GCode Parser] Exiting wipe tower section");
    }
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

void GCodeParser::add_segment(const glm::vec3& start, const glm::vec3& end, bool is_extrusion,
                              float e_delta) {
    if (layers_.empty()) {
        start_new_layer(start.z);
    }

    ToolpathSegment segment;
    segment.start = start;
    segment.end = end;
    segment.is_extrusion = is_extrusion;
    segment.object_name = current_object_;
    segment.extrusion_amount = e_delta;

    // Multi-color support: Tag segment with current tool
    segment.tool_index = current_tool_index_;

    // Wipe tower support: Tag wipe tower segments with special object name
    if (in_wipe_tower_) {
        segment.object_name = "__WIPE_TOWER__";
    }

    // Calculate actual extrusion width from E-delta and XY distance
    if (is_extrusion && e_delta > 0.00001f) {
        // Calculate XY distance
        float dx = end.x - start.x;
        float dy = end.y - start.y;
        float xy_distance = std::sqrt(dx * dx + dy * dy);

        if (xy_distance > 0.00001f) {
            // Calculate filament cross-sectional area
            float filament_radius = metadata_filament_diameter_ / 2.0f;
            float filament_area = static_cast<float>(M_PI) * filament_radius * filament_radius;

            // Calculate extruded volume: volume = e_delta * filament_area
            float volume = e_delta * filament_area;

            // Calculate width using Slic3r's oval cross-section formula
            // Extruded plastic forms an oval/rounded shape, not a rectangle
            // Cross-sectional area: A = (w - h) × h + π × (h/2)²
            // Where: A = volume / distance, h = layer_height, w = width
            // Solving for w: w = (A - π × (h/2)²) / h + h
            float h = metadata_layer_height_;
            float cross_section_area = volume / xy_distance;
            float h_radius = h / 2.0f;
            float circular_area = static_cast<float>(M_PI) * h_radius * h_radius;
            segment.width = (cross_section_area - circular_area) / h + h;

            // Sanity check: width should be reasonable (0.1mm to 2.0mm)
            if (segment.width < 0.1f || segment.width > 2.0f) {
                out_of_range_width_count_++;
                segment.width = 0.0f; // Use default
            }
        }
    }

    // Update layer data
    Layer& current_layer = layers_.back();
    current_layer.segments.push_back(segment);

    // For bounding box: skip start position if this is the first segment ever
    // (avoids including implicit (0,0,0) starting position in print bounds)
    bool is_first_segment = (layers_.size() == 1 && current_layer.segments.size() == 1);

    if (!is_first_segment) {
        current_layer.bounding_box.expand(start);
        global_bounds_.expand(start);
    }
    current_layer.bounding_box.expand(end);
    global_bounds_.expand(end);

    if (is_extrusion) {
        current_layer.segment_count_extrusion++;
    } else {
        current_layer.segment_count_travel++;
    }

    // Update object bounding box (only for extrusion moves, not travels)
    if (!current_object_.empty() && objects_.count(current_object_) > 0 && is_extrusion) {
        objects_[current_object_].bounding_box.expand(start);
        objects_[current_object_].bounding_box.expand(end);

        // Debug: Log first few extrusion segments per object
        static std::map<std::string, int> segment_counts;
        segment_counts[current_object_]++;
        if (segment_counts[current_object_] <= 3) {
            spdlog::trace(
                "[GCode Parser] Object '{}' extrusion segment: start=({:.2f},{:.2f},{:.2f}) "
                "end=({:.2f},{:.2f},{:.2f})",
                current_object_, start.x, start.y, start.z, end.x, end.y, end.z);
        }
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

    spdlog::trace("[GCode Parser] Started layer {} at Z={:.3f}", layers_.size() - 1, z);
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

    // Transfer metadata
    result.slicer_name = metadata_slicer_name_;
    result.filament_type = metadata_filament_type_;
    result.filament_color_hex = metadata_filament_color_;
    result.printer_model = metadata_printer_model_;
    result.nozzle_diameter_mm = metadata_nozzle_diameter_;
    result.total_filament_mm = metadata_filament_length_;
    result.filament_weight_g = metadata_filament_weight_;
    result.filament_cost = metadata_filament_cost_;

    // Transfer layer height + extrusion width metadata
    result.layer_height_mm = metadata_layer_height_;
    result.first_layer_height_mm = metadata_first_layer_height_;
    result.extrusion_width_mm = metadata_extrusion_width_;
    result.perimeter_extrusion_width_mm = metadata_perimeter_extrusion_width_;
    result.infill_extrusion_width_mm = metadata_infill_extrusion_width_;
    result.first_layer_extrusion_width_mm = metadata_first_layer_extrusion_width_;
    result.estimated_print_time_minutes = metadata_print_time_;
    result.total_layer_count = metadata_layer_count_;

    spdlog::info("[GCode Parser] Layer height: {}mm, first layer: {}mm, extrusion width: {}mm",
                 result.layer_height_mm,
                 result.first_layer_height_mm > 0 ? result.first_layer_height_mm
                                                  : result.layer_height_mm,
                 result.extrusion_width_mm);

    // Transfer multi-color tool palette
    result.tool_color_palette = tool_color_palette_;

    spdlog::info("[GCode Parser] Parsed G-code: {} layers, {} segments, {} objects",
                 result.layers.size(), result.total_segments, result.objects.size());

    // Log warning summary if any out-of-range width calculations occurred
    if (out_of_range_width_count_ > 0) {
        spdlog::debug("[GCode Parser] {} segments had out-of-range calculated width (used default)",
                      out_of_range_width_count_);
    }

    // Debug: Log object bounding boxes
    for (const auto& [name, obj] : result.objects) {
        spdlog::trace("[GCode Parser] Object '{}' AABB: min=({:.2f},{:.2f},{:.2f}) "
                      "max=({:.2f},{:.2f},{:.2f}) "
                      "center=({:.2f},{:.2f},{:.2f})",
                      name, obj.bounding_box.min.x, obj.bounding_box.min.y, obj.bounding_box.min.z,
                      obj.bounding_box.max.x, obj.bounding_box.max.y, obj.bounding_box.max.z,
                      obj.bounding_box.center().x, obj.bounding_box.center().y,
                      obj.bounding_box.center().z);
    }

    // Reset state for potential reuse
    reset();

    return result;
}

// ============================================================================
// Thumbnail Extraction Implementation
// ============================================================================

// Base64 decoding table
static const unsigned char base64_decode_table[256] = {
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 62,  255, 255, 255, 63,  52,  53,  54,  55,  56,  57,  58,  59,  60,
    61,  255, 255, 255, 255, 255, 255, 255, 0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,
    11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  255, 255, 255, 255,
    255, 255, 26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,
    43,  44,  45,  46,  47,  48,  49,  50,  51,  255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255};

std::vector<uint8_t> base64_decode(const std::string& encoded) {
    std::vector<uint8_t> result;
    result.reserve(encoded.size() * 3 / 4);

    uint32_t buffer = 0;
    int bits_collected = 0;

    for (char c : encoded) {
        if (std::isspace(c) || c == '=') {
            continue; // Skip whitespace and padding
        }

        unsigned char decoded = base64_decode_table[static_cast<unsigned char>(c)];
        if (decoded == 255) {
            continue; // Skip invalid characters
        }

        buffer = (buffer << 6) | decoded;
        bits_collected += 6;

        if (bits_collected >= 8) {
            bits_collected -= 8;
            result.push_back(static_cast<uint8_t>((buffer >> bits_collected) & 0xFF));
        }
    }

    return result;
}

std::vector<GCodeThumbnail> extract_thumbnails(const std::string& filepath) {
    std::vector<GCodeThumbnail> thumbnails;

    std::ifstream file(filepath);
    if (!file.is_open()) {
        spdlog::warn("[GCode Parser] Cannot open G-code file for thumbnail extraction: {}",
                     filepath);
        return thumbnails;
    }

    std::string line;
    GCodeThumbnail current_thumb;
    std::string base64_data;
    bool in_thumbnail_block = false;
    int lines_read = 0;
    constexpr int max_header_lines = 2000; // Thumbnails should be in first ~2000 lines

    while (std::getline(file, line) && lines_read < max_header_lines) {
        lines_read++;

        // Look for thumbnail begin marker
        // Format: "; thumbnail begin WIDTHxHEIGHT SIZE"
        size_t begin_pos = line.find("; thumbnail begin ");
        if (begin_pos != std::string::npos) {
            // Parse dimensions: "WIDTHxHEIGHT SIZE"
            std::string dims = line.substr(begin_pos + 18);
            int w = 0, h = 0, size = 0;
            if (sscanf(dims.c_str(), "%dx%d %d", &w, &h, &size) >= 2) {
                current_thumb = GCodeThumbnail();
                current_thumb.width = w;
                current_thumb.height = h;
                base64_data.clear();
                base64_data.reserve(static_cast<size_t>(size) * 4 / 3 +
                                    100); // Estimate base64 size
                in_thumbnail_block = true;
                spdlog::debug("[GCode Parser] Found thumbnail {}x{} in {}", w, h, filepath);
            }
            continue;
        }

        // Look for thumbnail end marker
        if (in_thumbnail_block && line.find("; thumbnail end") != std::string::npos) {
            // Decode accumulated base64 data
            current_thumb.png_data = base64_decode(base64_data);
            if (!current_thumb.png_data.empty()) {
                thumbnails.push_back(std::move(current_thumb));
            }
            in_thumbnail_block = false;
            continue;
        }

        // Accumulate base64 data (lines start with "; ")
        if (in_thumbnail_block && line.size() > 2 && line[0] == ';' && line[1] == ' ') {
            base64_data += line.substr(2);
        }

        // Stop if we hit actual G-code (not header comments)
        if (!line.empty() && line[0] != ';' && line[0] != '\r' && line[0] != '\n') {
            // Check if it's a G-code command
            if (line[0] == 'G' || line[0] == 'M' || line[0] == 'T') {
                break; // Past header, stop searching
            }
        }
    }

    // Sort by pixel count (largest first)
    std::sort(thumbnails.begin(), thumbnails.end(),
              [](const GCodeThumbnail& a, const GCodeThumbnail& b) {
                  return a.pixel_count() > b.pixel_count();
              });

    spdlog::info("[GCode Parser] Extracted {} thumbnails from {}", thumbnails.size(), filepath);
    return thumbnails;
}

std::vector<GCodeThumbnail> extract_thumbnails_from_content(const std::string& content) {
    std::vector<GCodeThumbnail> thumbnails;

    GCodeThumbnail current_thumb;
    std::string base64_data;
    bool in_thumbnail_block = false;
    int lines_read = 0;
    constexpr int max_header_lines = 2000; // Thumbnails should be in first ~2000 lines

    // Parse content line by line using string stream
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line) && lines_read < max_header_lines) {
        lines_read++;

        // Look for thumbnail begin marker
        // Format: "; thumbnail begin WIDTHxHEIGHT SIZE"
        size_t begin_pos = line.find("; thumbnail begin ");
        if (begin_pos != std::string::npos) {
            // Parse dimensions: "WIDTHxHEIGHT SIZE"
            std::string dims = line.substr(begin_pos + 18);
            int w = 0, h = 0, size = 0;
            if (sscanf(dims.c_str(), "%dx%d %d", &w, &h, &size) >= 2) {
                current_thumb = GCodeThumbnail();
                current_thumb.width = w;
                current_thumb.height = h;
                base64_data.clear();
                base64_data.reserve(static_cast<size_t>(size) * 4 / 3 +
                                    100); // Estimate base64 size
                in_thumbnail_block = true;
                spdlog::debug("[GCode Parser] Found thumbnail {}x{} in content", w, h);
            }
            continue;
        }

        // Look for thumbnail end marker
        if (in_thumbnail_block && line.find("; thumbnail end") != std::string::npos) {
            // Decode accumulated base64 data
            current_thumb.png_data = base64_decode(base64_data);
            if (!current_thumb.png_data.empty()) {
                thumbnails.push_back(std::move(current_thumb));
            }
            in_thumbnail_block = false;
            continue;
        }

        // Accumulate base64 data (lines start with "; ")
        if (in_thumbnail_block && line.size() > 2 && line[0] == ';' && line[1] == ' ') {
            base64_data += line.substr(2);
        }

        // Stop if we hit actual G-code (not header comments)
        if (!line.empty() && line[0] != ';' && line[0] != '\r' && line[0] != '\n') {
            // Check if it's a G-code command
            if (line[0] == 'G' || line[0] == 'M' || line[0] == 'T') {
                break; // Past header, stop searching
            }
        }
    }

    // Sort by pixel count (largest first)
    std::sort(thumbnails.begin(), thumbnails.end(),
              [](const GCodeThumbnail& a, const GCodeThumbnail& b) {
                  return a.pixel_count() > b.pixel_count();
              });

    spdlog::info("[GCode Parser] Extracted {} thumbnails from content ({} lines)",
                 thumbnails.size(), lines_read);
    return thumbnails;
}

GCodeThumbnail get_best_thumbnail(const std::string& filepath) {
    auto thumbnails = extract_thumbnails(filepath);
    if (thumbnails.empty()) {
        return GCodeThumbnail(); // Empty thumbnail
    }
    return std::move(thumbnails[0]); // Largest one (already sorted)
}

bool save_thumbnail_to_file(const std::string& gcode_path, const std::string& output_path) {
    GCodeThumbnail thumb = get_best_thumbnail(gcode_path);
    if (thumb.png_data.empty()) {
        spdlog::debug("[GCode Parser] No thumbnail found in {}", gcode_path);
        return false;
    }

    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open()) {
        spdlog::error("[GCode Parser] Cannot write thumbnail to {}", output_path);
        return false;
    }

    out.write(reinterpret_cast<const char*>(thumb.png_data.data()),
              static_cast<std::streamsize>(thumb.png_data.size()));
    spdlog::debug("[GCode Parser] Saved {}x{} thumbnail to {}", thumb.width, thumb.height,
                  output_path);
    return true;
}

std::string get_cached_thumbnail(const std::string& gcode_path, const std::string& cache_dir) {
    // Track if we've already shown errors (only show once per session)
    static bool cache_dir_error_shown = false;
    static bool write_error_shown = false;

    // Generate cache filename from gcode path
    std::string filename = gcode_path;
    size_t last_slash = filename.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        filename = filename.substr(last_slash + 1);
    }

    // Replace .gcode with .png
    size_t ext_pos = filename.rfind(".gcode");
    if (ext_pos != std::string::npos) {
        filename = filename.substr(0, ext_pos) + ".png";
    } else {
        filename += ".png";
    }

    std::string cache_path = cache_dir + "/" + filename;

    // Check if cache exists and is newer than gcode file
    struct stat gcode_stat, cache_stat;
    if (stat(gcode_path.c_str(), &gcode_stat) == 0 && stat(cache_path.c_str(), &cache_stat) == 0) {
        if (cache_stat.st_mtime >= gcode_stat.st_mtime) {
            spdlog::trace("[GCode Parser] Using cached thumbnail: {}", cache_path);
            return cache_path;
        }
    }

    // Ensure cache directory exists (create on-the-fly)
    struct stat dir_stat;
    if (stat(cache_dir.c_str(), &dir_stat) != 0) {
        if (mkdir(cache_dir.c_str(), 0750) != 0) {
            if (!cache_dir_error_shown) {
                spdlog::error(
                    "Cannot create thumbnail cache directory: {} (further errors suppressed)",
                    cache_dir);
                cache_dir_error_shown = true;
            }
            return ""; // Can't cache, but app continues working
        }
        spdlog::info("[GCode Parser] Created thumbnail cache directory: {}", cache_dir);
    }

    // Extract and save thumbnail
    if (save_thumbnail_to_file(gcode_path, cache_path)) {
        return cache_path;
    }

    // Log write failures only once
    if (!write_error_shown) {
        spdlog::warn(
            "[GCode Parser] Could not cache some thumbnails (further warnings suppressed)");
        write_error_shown = true;
    }

    return ""; // No thumbnail available
}

namespace {

/**
 * @brief Parse a single metadata comment line and update metadata struct
 * @param line Comment line starting with ';'
 * @param metadata Metadata struct to update
 * @return true if line was a valid metadata comment
 */
bool parse_metadata_line(const std::string& line, GCodeHeaderMetadata& metadata) {
    // Skip if not a comment line
    if (line.empty() || line[0] != ';') {
        return false;
    }

    // ====================
    // OrcaSlicer/PrusaSlicer format: "; generated by OrcaSlicer 2.3.1 on..."
    // ====================
    const std::string generated_prefix = "; generated by ";
    if (line.rfind(generated_prefix, 0) == 0) {
        std::string slicer_info = line.substr(generated_prefix.length());
        // Strip " on YYYY-MM-DD..." if present
        size_t on_pos = slicer_info.find(" on ");
        if (on_pos != std::string::npos) {
            slicer_info = slicer_info.substr(0, on_pos);
        }
        metadata.slicer = slicer_info;
        return true;
    }

    // ====================
    // Cura format: ";Generated with Cura_SteamEngine 5.6.0"
    // ====================
    const std::string cura_prefix = ";Generated with ";
    if (line.rfind(cura_prefix, 0) == 0) {
        metadata.slicer = line.substr(cura_prefix.length());
        return true;
    }

    // ====================
    // Cura format: ";TIME:7036" (time in seconds, no space)
    // ====================
    const std::string cura_time = ";TIME:";
    if (line.rfind(cura_time, 0) == 0) {
        try {
            metadata.estimated_time_seconds = std::stod(line.substr(cura_time.length()));
            return true;
        } catch (...) {
        }
    }

    // ====================
    // Cura format: ";Filament used: 1.20047m" (length in meters)
    // ====================
    const std::string cura_filament = ";Filament used: ";
    if (line.rfind(cura_filament, 0) == 0) {
        std::string filament_str = line.substr(cura_filament.length());
        // Parse "1.20047m" format - meters to mm
        double meters = 0;
        if (sscanf(filament_str.c_str(), "%lfm", &meters) == 1) {
            metadata.filament_used_mm = meters * 1000.0; // Convert to mm
            // Estimate grams (assuming PLA: 1.75mm diameter, ~1.24 g/cm³)
            // Volume = π * r² * length, mass = volume * density
            // For 1.75mm filament: π * (0.875)² ≈ 2.405 mm², so 1mm = 2.405mm³
            // At 1.24 g/cm³, 1mm of filament = 2.405 * 1.24 / 1000 ≈ 0.00298g
            metadata.filament_used_g = metadata.filament_used_mm * 0.00298;
        }
        return true;
    }

    // ====================
    // Cura format: ";Layer height: 0.12"
    // ====================
    const std::string cura_layer_height = ";Layer height: ";
    if (line.rfind(cura_layer_height, 0) == 0) {
        try {
            metadata.layer_height = std::stod(line.substr(cura_layer_height.length()));
        } catch (...) {
        }
        return true;
    }

    // ====================
    // Standard key=value or key: value format (OrcaSlicer/PrusaSlicer)
    // ====================
    // Need at least "; k" (3 chars) for a valid comment with key
    if (line.length() < 3) {
        return false;
    }

    // Parse comment metadata
    // OrcaSlicer format: "; key = value" or "; key: value"
    size_t eq_pos = line.find('=');
    size_t colon_pos = line.find(':');
    size_t sep_pos = std::string::npos;

    if (eq_pos != std::string::npos && (colon_pos == std::string::npos || eq_pos < colon_pos)) {
        sep_pos = eq_pos;
    } else if (colon_pos != std::string::npos) {
        sep_pos = colon_pos;
    }

    if (sep_pos == std::string::npos || sep_pos < 2) {
        return false;
    }

    // Extract key and value
    std::string key = line.substr(2, sep_pos - 2);
    std::string value = line.substr(sep_pos + 1);

    // Trim whitespace
    while (!key.empty() && std::isspace(key.back()))
        key.pop_back();
    while (!key.empty() && std::isspace(key.front()))
        key.erase(0, 1);
    while (!value.empty() && std::isspace(value.back()))
        value.pop_back();
    while (!value.empty() && std::isspace(value.front()))
        value.erase(0, 1);

    // Map known keys to metadata fields
    if (key == "generated by" || key == "slicer") {
        metadata.slicer = value;
    } else if (key == "slicer_version") {
        metadata.slicer_version = value;
    } else if (key == "estimated printing time" || key == "estimated printing time (normal mode)") {
        // Parse time string like "2h 30m 15s", "36m 25s", or "45s"
        // Use explicit pattern matching based on what's in the string
        int hours = 0, minutes = 0, seconds = 0;
        bool parsed = false;

        // Check which format we have by looking for unit markers
        bool has_h = (value.find('h') != std::string::npos);
        bool has_m = (value.find('m') != std::string::npos);
        bool has_s = (value.find('s') != std::string::npos);

        if (has_h && has_m && has_s) {
            // Format: "Nh NNm NNs"
            parsed = (sscanf(value.c_str(), "%dh %dm %ds", &hours, &minutes, &seconds) == 3);
        } else if (has_h && has_m) {
            // Format: "Nh NNm"
            parsed = (sscanf(value.c_str(), "%dh %dm", &hours, &minutes) == 2);
        } else if (has_m && has_s) {
            // Format: "NNm NNs"
            parsed = (sscanf(value.c_str(), "%dm %ds", &minutes, &seconds) == 2);
        } else if (has_h) {
            // Format: "Nh"
            parsed = (sscanf(value.c_str(), "%dh", &hours) == 1);
        } else if (has_m) {
            // Format: "NNm"
            parsed = (sscanf(value.c_str(), "%dm", &minutes) == 1);
        } else if (has_s) {
            // Format: "NNs"
            parsed = (sscanf(value.c_str(), "%ds", &seconds) == 1);
        }

        if (parsed) {
            metadata.estimated_time_seconds = hours * 3600.0 + minutes * 60.0 + seconds;
        }
    } else if (key == "total filament used [g]" || key == "filament used [g]" ||
               key == "total filament weight") {
        try {
            metadata.filament_used_g = std::stod(value);
        } catch (...) {
        }
    } else if (key == "filament used [mm]" || key == "total filament used [mm]") {
        try {
            metadata.filament_used_mm = std::stod(value);
        } catch (...) {
        }
    } else if (key == "total layers" || key == "total layer number") {
        try {
            metadata.layer_count = static_cast<uint32_t>(std::stoul(value));
        } catch (...) {
        }
    } else if (key == "first_layer_bed_temperature" || key == "bed_temperature") {
        try {
            metadata.first_layer_bed_temp = std::stod(value);
        } catch (...) {
        }
    } else if (key == "first_layer_temperature" || key == "nozzle_temperature") {
        try {
            metadata.first_layer_nozzle_temp = std::stod(value);
        } catch (...) {
        }
    } else if (key == "layer_height") {
        try {
            metadata.layer_height = std::stod(value);
        } catch (...) {
        }
    } else if (key == "first_layer_height") {
        try {
            metadata.first_layer_height = std::stod(value);
        } catch (...) {
        }
    } else if (key == "max_z_height") {
        try {
            metadata.object_height = std::stod(value);
        } catch (...) {
        }
    } else if (key == "filament_type") {
        // Slicers may output multiple types separated by semicolons (e.g., "PLA;PLA;PLA")
        // Take the first type only
        size_t semicolon = value.find(';');
        metadata.filament_type =
            (semicolon != std::string::npos) ? value.substr(0, semicolon) : value;
    } else if (key == "extruder_colour" || key == "filament_colour") {
        // Parse multi-tool colors: "#ED1C24;#00C1AE;#F4E2C1;#000000"
        // May also have spaces: "#AA0000 ; #00BB00 ; #0000CC"
        metadata.tool_colors.clear();
        std::string color;
        bool in_color = false;

        for (char c : value) {
            if (c == '#') {
                if (!color.empty() && color[0] == '#') {
                    // Save previous color
                    metadata.tool_colors.push_back(color);
                }
                color = "#";
                in_color = true;
            } else if (in_color) {
                if (std::isxdigit(c)) {
                    color += c;
                } else if (c == ';' || c == ' ' || c == ',') {
                    // End of this color
                    if (color.length() >= 4) { // At least #RGB
                        metadata.tool_colors.push_back(color);
                    }
                    color.clear();
                    in_color = false;
                }
            }
        }
        // Don't forget the last color
        if (!color.empty() && color[0] == '#' && color.length() >= 4) {
            metadata.tool_colors.push_back(color);
        }
    }

    return true;
}

/**
 * @brief Read the last N bytes of a file and extract lines
 * @param filepath Path to the file
 * @param bytes_to_read Number of bytes to read from end
 * @return Vector of lines from the file footer
 */
std::vector<std::string> read_file_footer(const std::string& filepath, size_t bytes_to_read) {
    std::vector<std::string> lines;

    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        return lines;
    }

    // Get file size
    std::streampos file_size = file.tellg();
    if (file_size <= 0) {
        return lines;
    }

    // Calculate start position
    size_t actual_size = static_cast<size_t>(file_size);
    size_t start_pos = (actual_size > bytes_to_read) ? (actual_size - bytes_to_read) : 0;

    // Seek and read
    file.seekg(static_cast<std::streamoff>(start_pos));
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // Split into lines
    std::istringstream stream(content);
    std::string line;

    // Skip first partial line if we didn't start at beginning
    if (start_pos > 0 && std::getline(stream, line)) {
        // Discard partial first line
    }

    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    return lines;
}

} // anonymous namespace

GCodeHeaderMetadata extract_header_metadata(const std::string& filepath) {
    GCodeHeaderMetadata metadata;
    metadata.filename = filepath;

    // Get file size and modification time
    struct stat file_stat;
    if (stat(filepath.c_str(), &file_stat) == 0) {
        metadata.file_size = static_cast<uint64_t>(file_stat.st_size);
        metadata.modified_time = static_cast<double>(file_stat.st_mtime);
    }

    std::ifstream file(filepath);
    if (!file.is_open()) {
        return metadata;
    }

    // Phase 1: Scan header (first ~500 lines) for slicer info, layer count, temps
    std::string line;
    int lines_read = 0;
    constexpr int max_header_lines = 500;

    while (std::getline(file, line) && lines_read < max_header_lines) {
        lines_read++;

        // Skip non-comment lines
        if (line.empty() || line[0] != ';') {
            // Check if we've hit actual G-code
            if (!line.empty() && (line[0] == 'G' || line[0] == 'M' || line[0] == 'T')) {
                break;
            }
            continue;
        }

        parse_metadata_line(line, metadata);
    }

    file.close();

    // Phase 2: Scan footer for print time and filament usage
    // OrcaSlicer/PrusaSlicer place these computed values at the end of the file
    constexpr size_t footer_bytes = 64 * 1024; // Read last 64KB
    auto footer_lines = read_file_footer(filepath, footer_bytes);

    for (const auto& footer_line : footer_lines) {
        if (footer_line.empty() || footer_line[0] != ';') {
            continue;
        }
        parse_metadata_line(footer_line, metadata);
    }

    return metadata;
}

} // namespace gcode
} // namespace helix
