// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_parser.h"

#include "filament_database.h"
#include "gcode_color_metadata.h"
#include "text_io.h"
#include "utils/decimal_parse.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <utility>

#if !defined(HELIX_PLATFORM_ESP32)
#include "lodepng_encode.h"
#include "stb_image.h"

#include <lvgl.h>
#include <memory>
#endif

namespace {

/// Parse a G-code parameter value the way Klipper reads it: klippy's gcode.py
/// pulls parameters through Python's float() (mathutil.safe_float), which
/// accepts a single leading '+' that parse_decimal's from_chars-exact grammar
/// does not. Skips at most one leading '+' and delegates to parse_decimal;
/// a second sign right after it ("++1", "+-1") is rejected the same way
/// float() rejects it, since parse_decimal alone would accept the '-' in
/// "+-1" as a negative number once the '+' is stripped.
inline helix::DecimalParseResult parse_gcode_decimal(const char* first, const char* last,
                                                     float& value) {
    if (first == last || *first != '+') {
        return helix::parse_decimal(first, last, value);
    }
    const char* after_sign = first + 1;
    if (after_sign == last || *after_sign == '+' || *after_sign == '-') {
        return {first, std::errc::invalid_argument};
    }
    return helix::parse_decimal(after_sign, last, value);
}

std::string_view trim_ws(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

/// Split a slicer comment ("; key = value" or ";key: value") into its trimmed
/// key and value. The separator is whichever of '=' and ':' comes first, so a
/// value holding a colon ("1h: 20m") stays whole behind an '=' key.
std::optional<std::pair<std::string_view, std::string_view>>
split_comment_kv(std::string_view line) {
    if (line.empty() || line[0] != ';')
        return std::nullopt;
    line.remove_prefix(1);
    const size_t sep = line.find_first_of("=:");
    if (sep == std::string_view::npos)
        return std::nullopt;
    std::string_view key = trim_ws(line.substr(0, sep));
    if (key.empty())
        return std::nullopt;
    return std::make_pair(key, trim_ws(line.substr(sep + 1)));
}

/// Seconds in a slicer duration such as "1d 2h 3m 4s", "36m 25s" or "45s".
/// Every number must carry a d/h/m/s unit; anything else is not a duration.
std::optional<double> parse_slicer_duration(std::string_view s) {
    double total = 0.0;
    bool any = false;
    const char* p = s.data();
    const char* end = s.data() + s.size();
    while (p != end) {
        if (std::isspace(static_cast<unsigned char>(*p))) {
            ++p;
            continue;
        }
        float v = 0.0f;
        auto [next, ec] = helix::parse_decimal(p, end, v);
        if (ec != std::errc{})
            return std::nullopt;
        p = next;
        while (p != end && *p == ' ')
            ++p;
        if (p == end)
            return std::nullopt;
        switch (*p++) {
        case 'd':
            total += v * 86400.0;
            break;
        case 'h':
            total += v * 3600.0;
            break;
        case 'm':
            total += v * 60.0;
            break;
        case 's':
            total += v;
            break;
        default:
            return std::nullopt;
        }
        any = true;
    }
    if (!any)
        return std::nullopt;
    return total;
}

} // namespace

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
    current_object_index_ = -1;
    object_name_lookup_.clear();
    object_name_table_.clear();
    is_absolute_positioning_ = true;
    is_absolute_extrusion_ = true;
    current_tool_index_ = 0;
    initial_tool_index_ = -1;
    tools_used_.clear();
    layers_.clear();
    objects_.clear();
    global_bounds_ = AABB();
    lines_parsed_ = 0;
    out_of_range_width_count_ = 0;
    // Running counter, so it must clear with layers_ above: finalize() re-sums
    // total_segments from the (now empty) layers, and a surviving drawable
    // count would exceed it on the next parse.
    drawable_segments_ = 0;

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
        parse_type_marker(comment);
    }

    std::string trimmed = trim_line(line);
    if (trimmed.empty()) {
        return;
    }

    if (trimmed[0] == 'T') {
        parse_tool_change_command(trimmed);
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

    // G92: Set position (resets extruder and/or axis positions)
    if (trimmed.find("G92 ") == 0 || trimmed == "G92") {
        parse_set_position_command(trimmed);
        return;
    }

    // Parse movement commands (G0, G1)
    if (trimmed[0] == 'G' && (trimmed.find("G0 ") == 0 || trimmed.find("G1 ") == 0 ||
                              trimmed == "G0" || trimmed == "G1")) {
        parse_movement_command(trimmed);
        return;
    }

    // G2/G3: Arc moves — linearize into short segments
    if (trimmed[0] == 'G' && (trimmed.find("G2 ") == 0 || trimmed.find("G3 ") == 0)) {
        bool clockwise = (trimmed[1] == '2');
        parse_arc_command(trimmed, clockwise);
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

void GCodeParser::parse_set_position_command(const std::string& line) {
    // G92 sets the current position without moving.
    // Most commonly: G92 E0 (reset extruder position to 0)
    float value;
    if (extract_param(line, 'E', value)) {
        current_e_ = value;
    }
    if (extract_param(line, 'X', value)) {
        current_position_.x = value;
    }
    if (extract_param(line, 'Y', value)) {
        current_position_.y = value;
    }
    if (extract_param(line, 'Z', value)) {
        current_position_.z = value;
    }
}

void GCodeParser::parse_arc_command(const std::string& line, bool clockwise) {
    // G2/G3 arc moves: linearize into short line segments for the 2D renderer.
    // Parameters: X Y Z (endpoint), I J (center offset from start), E (extrusion)
    float value;
    glm::vec3 end_pos = current_position_;
    float new_e = current_e_;
    bool has_extrusion = false;
    float i_offset = 0.0f, j_offset = 0.0f;

    if (extract_param(line, 'X', value))
        end_pos.x = is_absolute_positioning_ ? value : current_position_.x + value;
    if (extract_param(line, 'Y', value))
        end_pos.y = is_absolute_positioning_ ? value : current_position_.y + value;
    if (extract_param(line, 'Z', value))
        end_pos.z = is_absolute_positioning_ ? value : current_position_.z + value;
    if (extract_param(line, 'E', value)) {
        new_e = is_absolute_extrusion_ ? value : current_e_ + value;
        has_extrusion = true;
    }
    if (extract_param(line, 'I', value))
        i_offset = value;
    if (extract_param(line, 'J', value))
        j_offset = value;

    // Calculate arc center
    float cx = current_position_.x + i_offset;
    float cy = current_position_.y + j_offset;

    // Calculate start and end angles
    float start_angle = std::atan2(current_position_.y - cy, current_position_.x - cx);
    float end_angle = std::atan2(end_pos.y - cy, end_pos.x - cx);
    float radius = std::sqrt(i_offset * i_offset + j_offset * j_offset);

    if (radius < 0.001f) {
        // Degenerate arc — just update position
        current_position_ = end_pos;
        if (has_extrusion)
            current_e_ = new_e;
        return;
    }

    // Calculate sweep angle
    float sweep = end_angle - start_angle;
    if (clockwise) {
        if (sweep >= 0)
            sweep -= 2.0f * static_cast<float>(M_PI);
    } else {
        if (sweep <= 0)
            sweep += 2.0f * static_cast<float>(M_PI);
    }

    // Check for full circle (P parameter or endpoint == startpoint)
    int full_turns = 0;
    if (extract_param(line, 'P', value))
        full_turns = static_cast<int>(value);
    if (full_turns > 0) {
        float dir = clockwise ? -1.0f : 1.0f;
        sweep = dir * full_turns * 2.0f * static_cast<float>(M_PI) + (end_angle - start_angle);
        if (clockwise && sweep > 0)
            sweep -= 2.0f * static_cast<float>(M_PI);
        else if (!clockwise && sweep < 0)
            sweep += 2.0f * static_cast<float>(M_PI);
    }

    // Linearize: ~1mm segments or at least 8 segments
    float arc_length = std::abs(sweep) * radius;
    int num_segments = std::max(8, static_cast<int>(arc_length / 1.0f));
    num_segments = std::min(num_segments, 128); // Cap to avoid excessive segments

    float total_e_delta = has_extrusion ? (new_e - current_e_) : 0.0f;
    bool is_extruding = has_extrusion && total_e_delta > 0.00001f;

    for (int i = 1; i <= num_segments; i++) {
        float t = static_cast<float>(i) / static_cast<float>(num_segments);
        float angle = start_angle + sweep * t;

        glm::vec3 seg_end;
        seg_end.x = cx + radius * std::cos(angle);
        seg_end.y = cy + radius * std::sin(angle);
        seg_end.z = current_position_.z + (end_pos.z - current_position_.z) * t;

        // Only add segment if there's XY movement
        if (seg_end.x != current_position_.x || seg_end.y != current_position_.y) {
            float seg_e_delta = is_extruding ? (total_e_delta / num_segments) : 0.0f;
            add_segment(current_position_, seg_end, is_extruding, seg_e_delta);
        }

        current_position_ = seg_end;
    }

    // Snap to exact endpoint
    current_position_ = end_pos;
    if (has_extrusion) {
        current_e_ = new_e;
    }
}

std::optional<std::string> gcode_param_value(std::string_view line, std::string_view key) {
    std::string needle(key);
    needle += '=';
    const size_t pos = line.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t start = pos + needle.size();
    if (start >= line.size()) {
        return std::nullopt;
    }
    const char quote = line[start];
    if (quote == '"' || quote == '\'') {
        const size_t close = line.find(quote, start + 1);
        if (close == std::string_view::npos || close == start + 1) {
            return std::nullopt;
        }
        return std::string(line.substr(start + 1, close - start - 1));
    }
    const size_t end = line.find_first_of(" \t", start);
    return std::string(
        line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
}

std::optional<GCodeObject> parse_exclude_object_define(std::string_view line) {
    line = helix::text_io::trim(line.substr(0, line.find(';')));
    constexpr std::string_view kDefine = "EXCLUDE_OBJECT_DEFINE";
    if (line.substr(0, kDefine.size()) != kDefine) {
        return std::nullopt;
    }
    auto name = gcode_param_value(line, "NAME");
    if (!name) {
        return std::nullopt;
    }

    GCodeObject obj;
    obj.name = std::move(*name);

    // CENTER=X,Y
    if (auto center_str = gcode_param_value(line, "CENTER")) {
        const size_t comma = center_str->find(',');
        if (comma != std::string::npos) {
            auto [px, ecx] =
                parse_gcode_decimal(center_str->data(), center_str->data() + comma, obj.center.x);
            auto [py, ecy] =
                parse_gcode_decimal(center_str->data() + comma + 1,
                                    center_str->data() + center_str->size(), obj.center.y);
            if (ecx != std::errc{} || ecy != std::errc{}) {
                spdlog::debug("[GCode Parser] Failed to parse CENTER for object: {}", obj.name);
            }
        }
    }

    // POLYGON=[[x1,y1],[x2,y2],...]
    if (auto polygon = gcode_param_value(line, "POLYGON")) {
        std::string polygon_str = std::move(*polygon);
        polygon_str.erase(std::remove_if(polygon_str.begin(), polygon_str.end(), ::isspace),
                          polygon_str.end());
        size_t pos = (!polygon_str.empty() && polygon_str[0] == '[') ? 1 : 0;
        while (pos < polygon_str.length()) {
            if (polygon_str[pos] != '[') {
                pos++;
                continue;
            }
            pos++;
            const size_t comma = polygon_str.find(',', pos);
            if (comma == std::string::npos) {
                break;
            }
            float x = 0, y = 0;
            auto [px, ecx] =
                parse_gcode_decimal(polygon_str.data() + pos, polygon_str.data() + comma, x);
            if (ecx != std::errc{}) {
                break;
            }
            pos = comma + 1;
            const size_t close = polygon_str.find(']', pos);
            if (close == std::string::npos) {
                break;
            }
            auto [py, ecy] =
                parse_gcode_decimal(polygon_str.data() + pos, polygon_str.data() + close, y);
            if (ecy != std::errc{}) {
                break;
            }
            obj.polygon.push_back(glm::vec2(x, y));
            pos = close + 1;
        }
    }
    return obj;
}

std::vector<GCodeObject> collect_exclude_object_defines(std::string_view content) {
    const size_t last_newline = content.rfind('\n');
    content = last_newline == std::string_view::npos ? std::string_view{}
                                                     : content.substr(0, last_newline + 1);
    std::vector<GCodeObject> out;
    for (std::string_view line : helix::text_io::lines(content)) {
        auto obj = parse_exclude_object_define(line);
        if (!obj) {
            continue;
        }
        auto same = std::find_if(out.begin(), out.end(),
                                 [&](const GCodeObject& o) { return o.name == obj->name; });
        if (same != out.end()) {
            *same = std::move(*obj);
        } else {
            out.push_back(std::move(*obj));
        }
    }
    return out;
}

bool GCodeParser::parse_exclude_object_command(const std::string& line) {
    // EXCLUDE_OBJECT_DEFINE NAME=... CENTER=... POLYGON=...
    if (line.find("EXCLUDE_OBJECT_DEFINE") == 0) {
        auto obj = parse_exclude_object_define(line);
        if (!obj) {
            return false;
        }
        spdlog::trace("[GCode Parser] Defined object: {} at ({}, {})", obj->name, obj->center.x,
                      obj->center.y);
        objects_[obj->name] = std::move(*obj);
        return true;
    }
    // EXCLUDE_OBJECT_START NAME=...
    else if (line.find("EXCLUDE_OBJECT_START") == 0) {
        if (!extract_string_param(line, "NAME", current_object_)) {
            current_object_.clear();
            current_object_index_ = -1;
            return false;
        }
        // Intern the object name for segment tagging
        auto it = object_name_lookup_.find(current_object_);
        if (it != object_name_lookup_.end()) {
            current_object_index_ = it->second;
        } else if (object_name_table_.size() >= static_cast<size_t>(INT16_MAX)) {
            spdlog::warn("[GCode Parser] Object name table full ({} entries), ignoring: {}",
                         object_name_table_.size(), current_object_);
            current_object_index_ = -1;
        } else {
            current_object_index_ = static_cast<int16_t>(object_name_table_.size());
            object_name_table_.push_back(current_object_);
            object_name_lookup_[current_object_] = current_object_index_;
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
            current_object_index_ = -1;
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
    // Use string_view to avoid allocations for the layer marker check
    std::string_view line_sv(line);
    std::string_view after_semi = line_sv.substr(1);

    // Skip leading whitespace
    size_t ws_start = 0;
    while (ws_start < after_semi.length() && std::isspace(after_semi[ws_start])) {
        ws_start++;
    }
    std::string_view trimmed_content = after_semi.substr(ws_start);

    // Quick uppercase check for LAYER markers without allocating a string
    // LAYER_CHANGE starts with 'L'/'l', LAYER: also starts with 'L'/'l'
    if (!trimmed_content.empty() && (trimmed_content[0] == 'L' || trimmed_content[0] == 'l')) {
        const std::string content_upper = helix::text_io::to_upper(trimmed_content);

        // Detect layer change markers (but not LAYER_COUNT which is metadata)
        if (content_upper.find("LAYER_CHANGE") == 0 || content_upper.find("LAYER:") == 0) {
            use_layer_markers_ = true;
            pending_layer_marker_ = true;
            spdlog::trace("[GCode Parser] Layer marker detected: '{}' (use_markers={}, pending={})",
                          line, use_layer_markers_, pending_layer_marker_);
            return;
        }
    }

    const auto kv = split_comment_kv(line_sv);
    if (!kv) {
        return;
    }
    const std::string value(kv->second);

    // Convert key to lowercase for case-insensitive matching
    const std::string key_lower = helix::text_io::to_lower(kv->first);

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
        // The shared palette parser owns every list form (';' and ','
        // separated). Ask it: a value that parses into multiple entries is
        // per-tool metadata, a single valid entry is a plain single color,
        // and anything it rejects leaves the metadata untouched rather than
        // storing an unvalidated blob in filament_color_hex.
        std::vector<std::string> as_palette;
        if (helix::gcode::parse_filament_color_palette(line, as_palette) && as_palette.size() > 1) {
            parse_extruder_color_metadata(line);
        } else if (const std::string* single = helix::gcode::first_named_color(as_palette)) {
            // Single color metadata — the first entry that actually holds one,
            // not index 0: a list like ",,#FF0000" parks an empty placeholder
            // at slot 0, and storing that would drop the only color the file
            // stated.
            metadata_filament_color_ = *single;
            spdlog::trace("[GCode Parser] Parsed single filament color: {}",
                          metadata_filament_color_);
        } else if (std::string_view cleaned = helix::gcode::clean_color_hex(value);
                   !cleaned.empty()) {
            // The list parser rejects this key spelling ("filament colour",
            // say) but the value is still one real color token. Store the
            // validated token, never the raw value: a comma blob stored raw
            // paints everything in its first field.
            metadata_filament_color_ = std::string(cleaned);
            spdlog::trace("[GCode Parser] Parsed single filament color: {}",
                          metadata_filament_color_);
        }
    } else if (contains_all({"filament", "type"})) {
        metadata_filament_type_ = value;
        spdlog::trace("[GCode Parser] Parsed filament type: {}", value);
    } else if (contains_all({"nozzle", "diameter"})) {
        if (const auto v = helix::text_io::parse_leading<float>(value)) {
            metadata_nozzle_diameter_ = *v;
            spdlog::trace("[GCode Parser] Parsed nozzle diameter: {}mm", metadata_nozzle_diameter_);
        }
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
        if (const auto parsed = helix::text_io::parse_leading<float>(numeric_value)) {
            const float h = *parsed;
            if (h > 0.01f && h < 2.0f) {
                if (key_lower.find("first") != std::string::npos) {
                    metadata_first_layer_height_ = h;
                    spdlog::trace("[GCode Parser] Parsed first layer height: {}mm", h);
                } else {
                    metadata_layer_height_ = h;
                    spdlog::trace("[GCode Parser] Parsed layer height: {}mm", h);
                }
            }
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

        const auto parsed_width = helix::text_io::parse_leading<float>(numeric_value);
        if (!parsed_width) {
            // Failed to parse width value
            return;
        }
        const float width = *parsed_width;

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
    }
}

void GCodeParser::parse_extruder_color_metadata(const std::string& line) {
    // Format: "; extruder_colour = #ED1C24;#00C1AE;#F4E2C1;#000000"
    //     OR: "; filament_colour = ..." (fallback)
    //     OR: ";extruder_colour=#AA0000 ; #00BB00 ;#0000CC" (with variations)
    std::vector<std::string> palette;
    if (!helix::gcode::parse_filament_color_palette(line, palette)) {
        return;
    }
    tool_color_palette_ = std::move(palette);

    // Log color palette (manual join since fmt::join may not be available)
    std::string palette_str;
    for (size_t i = 0; i < tool_color_palette_.size(); ++i) {
        if (i > 0)
            palette_str += ", ";
        palette_str += tool_color_palette_[i];
    }
    spdlog::debug("[GCode Parser] Parsed {} extruder colors from metadata: [{}]",
                  tool_color_palette_.size(), palette_str);

    // Set metadata_filament_color_ to the active tool's color (for single-color
    // rendering fallback). Prefer the first T command seen — picking palette[0]
    // unconditionally would render a print on T3 with T0's color, which can be
    // wildly wrong on multi-material setups.
    int fallback_tool = (initial_tool_index_ >= 0 &&
                         initial_tool_index_ < static_cast<int>(tool_color_palette_.size()) &&
                         !tool_color_palette_[initial_tool_index_].empty())
                            ? initial_tool_index_
                            : -1;
    if (fallback_tool >= 0) {
        metadata_filament_color_ = tool_color_palette_[fallback_tool];
    } else if (const std::string* first = helix::gcode::first_named_color(tool_color_palette_)) {
        // No covered initial tool: the first color the file does state, not
        // palette[0], which can be an empty placeholder for an unknown slot.
        metadata_filament_color_ = *first;
    }
}

void GCodeParser::parse_tool_change_command(const std::string& line) {
    const int tool_num = tool_index_for_line(line);
    if (tool_num < 0) {
        return;
    }

    current_tool_index_ = tool_num;
    tools_used_.insert(tool_num);
    if (initial_tool_index_ < 0) {
        initial_tool_index_ = tool_num;
        // If palette already parsed, retroactively pick the active tool's color
        // as the single-color fallback (used when per-tool data isn't available
        // to the renderer at draw time).
        if (tool_num >= 0 && tool_num < static_cast<int>(tool_color_palette_.size()) &&
            !tool_color_palette_[tool_num].empty()) {
            metadata_filament_color_ = tool_color_palette_[tool_num];
        }
    }
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

std::optional<FeatureType> GCodeParser::extract_type_marker(const char* line, size_t len) {
    // Find a `;` (the marker must be inside a comment).
    size_t semi = std::string::npos;
    for (size_t i = 0; i < len; ++i) {
        if (line[i] == ';') {
            semi = i;
            break;
        }
    }
    if (semi == std::string::npos) {
        return std::nullopt;
    }
    // Expect "TYPE:" after `;` and optional whitespace.
    size_t k = semi + 1;
    while (k < len && (line[k] == ' ' || line[k] == '\t')) {
        ++k;
    }
    static constexpr char KEY[] = "TYPE:";
    static constexpr size_t KEY_LEN = 5;
    if (k + KEY_LEN > len) {
        return std::nullopt;
    }
    for (size_t i = 0; i < KEY_LEN; ++i) {
        if (line[k + i] != KEY[i]) {
            return std::nullopt;
        }
    }
    // Extract trimmed value.
    size_t v_start = k + KEY_LEN;
    while (v_start < len && (line[v_start] == ' ' || line[v_start] == '\t')) {
        ++v_start;
    }
    size_t v_end = len;
    while (v_end > v_start && (line[v_end - 1] == '\r' || line[v_end - 1] == '\n' ||
                               line[v_end - 1] == ' ' || line[v_end - 1] == '\t')) {
        --v_end;
    }
    std::string value(line + v_start, v_end - v_start);
    return parse_feature_type_value(value);
}

void GCodeParser::parse_type_marker(const std::string& comment) {
    auto t = extract_type_marker(comment.data(), comment.size());
    if (t) {
        current_feature_type_ = *t;
    }
}

FeatureType GCodeParser::parse_feature_type_value(const std::string& value) {
    if (value.empty()) {
        return FeatureType::Unknown;
    }
    // OrcaSlicer / PrusaSlicer / Bambu (space-separated, mixed case).
    // Cura emits hyphenated UPPERCASE.
    struct Mapping {
        const char* name;
        FeatureType type;
    };
    static constexpr Mapping table[] = {
        // OrcaSlicer / PrusaSlicer / Bambu
        {"Custom", FeatureType::Custom},
        {"Skirt", FeatureType::Skirt},
        {"Brim", FeatureType::Brim},
        {"Outer wall", FeatureType::OuterWall},
        {"Inner wall", FeatureType::InnerWall},
        {"Overhang wall", FeatureType::OverhangWall},
        {"Sparse infill", FeatureType::SparseInfill},
        {"Solid infill", FeatureType::SolidInfill},
        {"Internal solid infill", FeatureType::SolidInfill},
        {"Top surface", FeatureType::TopSurface},
        {"Top solid infill", FeatureType::TopSurface},
        {"Bottom surface", FeatureType::BottomSurface},
        {"Bridge", FeatureType::Bridge},
        {"Internal Bridge", FeatureType::Bridge},
        {"Bridge infill", FeatureType::Bridge},
        {"Gap infill", FeatureType::GapInfill},
        {"Support material", FeatureType::Support},
        {"Support material interface", FeatureType::Support},
        {"Support", FeatureType::Support},
        {"Wipe tower", FeatureType::WipeTower},
        {"Prime tower", FeatureType::WipeTower},
        {"Ironing", FeatureType::TopSurface},
        {"Thin wall", FeatureType::InnerWall},
        // PrusaSlicer legacy names
        {"Perimeter", FeatureType::InnerWall},
        {"External perimeter", FeatureType::OuterWall},
        {"Overhang perimeter", FeatureType::OverhangWall},
        {"Internal infill", FeatureType::SparseInfill},
        // Cura
        {"WALL-OUTER", FeatureType::OuterWall},
        {"WALL-INNER", FeatureType::InnerWall},
        {"SKIRT", FeatureType::Skirt},
        {"BRIM", FeatureType::Brim},
        {"FILL", FeatureType::SparseInfill},
        {"SKIN", FeatureType::SolidInfill},
        {"SUPPORT", FeatureType::Support},
        {"SUPPORT-INTERFACE", FeatureType::Support},
        {"SUPPORT-INFILL", FeatureType::Support},
        {"PRIME-TOWER", FeatureType::WipeTower},
        {"CUSTOM", FeatureType::Custom},
    };
    for (const auto& m : table) {
        if (value == m.name) {
            return m.type;
        }
    }
    return FeatureType::Unknown;
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

    auto [ptr, ec] = parse_gcode_decimal(line.data() + start, line.data() + end, out_value);
    return ec == std::errc{};
}

bool GCodeParser::extract_string_param(const std::string& line, const std::string& param,
                                       std::string& out_value) {
    auto value = gcode_param_value(line, param);
    if (!value) {
        return false;
    }
    out_value = std::move(*value);
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
    segment.object_name_index = current_object_index_;
    segment.extrusion_amount = e_delta;
    segment.feature_type = current_feature_type_;

    // Multi-color support: Tag segment with current tool
    segment.tool_index = static_cast<int8_t>(current_tool_index_);

    // Wipe tower support: Tag wipe tower segments with interned special name
    if (in_wipe_tower_ && segment.object_name_index < 0) {
        auto it = object_name_lookup_.find("__WIPE_TOWER__");
        if (it != object_name_lookup_.end()) {
            segment.object_name_index = it->second;
        } else if (object_name_table_.size() < static_cast<size_t>(INT16_MAX)) {
            segment.object_name_index = static_cast<int16_t>(object_name_table_.size());
            object_name_table_.push_back("__WIPE_TOWER__");
            object_name_lookup_["__WIPE_TOWER__"] = segment.object_name_index;
        }
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
    // (avoids including implicit (0,0,0) starting position in print bounds).
    // Auxiliary geometry (purge, prime tower) stays out of the layer box for
    // the same reason it stays out of the global one: the single-layer view
    // and its centering frame against the print, not the scaffolding beside it.
    bool is_first_segment = (layers_.size() == 1 && current_layer.segments.size() == 1);
    const bool auxiliary = is_auxiliary_geometry(current_feature_type_);

    // Count what the geometry builder will keep. Reads the SAME local the bbox
    // guards below use, so the budget estimate and the builder's skip test the
    // same predicate on the same value and cannot drift apart.
    if (!auxiliary) {
        ++drawable_segments_;
    }

    // Travels never frame a layer either: the approach and departure travels
    // around a prime tower would inject its coordinates right back in. Like
    // the global box, the layer box is extrusion-only, so a travel-only layer
    // leaves it empty.
    if (is_extrusion && !auxiliary) {
        if (!is_first_segment) {
            current_layer.bounding_box.expand(start);
        }
        current_layer.bounding_box.expand(end);
    }

    // Global bounds only include extrusion moves — travel moves to homing/probing/
    // parking positions would inflate the viewport and make the model appear tiny.
    // Auxiliary geometry (purge, prime tower) is excluded for the same reason:
    // the auto-fit viewport zooms to the actual print object (parity with the
    // streaming-mode filter in GCodeLayerRenderer::auto_fit).
    if (is_extrusion) {
        if (!auxiliary) {
            if (!is_first_segment) {
                global_bounds_.expand(start);
            }
            global_bounds_.expand(end);
        }
        current_layer.segment_count_extrusion++;
    } else {
        current_layer.segment_count_travel++;
    }

    // Update object bounding box (only for extrusion moves, not travels, and
    // never auxiliary geometry: an object box reaching into the tower region
    // turns every pick footprint and selection bracket toward empty plate,
    // and a tap there can long-press-exclude real printed geometry).
    if (!current_object_.empty() && objects_.count(current_object_) > 0 && is_extrusion &&
        !auxiliary) {
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

    // The layer just finished is never appended to again, but its segment vector still holds
    // the geometric-growth tail (a 600-segment layer sits on capacity 1024). Release it.
    size_t prev_segment_count = 0;
    if (!layers_.empty()) {
        layers_.back().segments.shrink_to_fit();
        prev_segment_count = layers_.back().segments.size();
    }

    Layer layer;
    layer.z_height = z;
    layers_.push_back(layer);

    // Layers are near-uniform in segment count, so the previous layer is a good size hint
    // and avoids the doubling series on the way up.
    if (prev_segment_count > 0) {
        layers_.back().segments.reserve(prev_segment_count);
    }

    spdlog::trace("[GCode Parser] Started layer {} at Z={:.3f}", layers_.size() - 1, z);
}

std::string GCodeParser::trim_line(const std::string& line) {
    if (line.empty()) {
        return line;
    }

    // Work with string_view to avoid intermediate allocations
    std::string_view sv(line);

    // Remove comments (everything after ';')
    size_t comment_pos = sv.find(';');
    if (comment_pos != std::string_view::npos) {
        sv = sv.substr(0, comment_pos);
    }

    // Trim leading whitespace
    size_t start = 0;
    while (start < sv.length() && std::isspace(sv[start])) {
        start++;
    }

    if (start == sv.length()) {
        return "";
    }

    // Trim trailing whitespace
    size_t end = sv.length();
    while (end > start && std::isspace(sv[end - 1])) {
        end--;
    }

    return std::string(sv.substr(start, end - start));
}

void GCodeParser::promote_auxiliary_only_extrusion() {
    bool has_real_extrusion = false;
    bool has_auxiliary_extrusion = false;
    for (const auto& layer : layers_) {
        for (const auto& seg : layer.segments) {
            if (!seg.is_extrusion) {
                continue;
            }
            if (is_auxiliary_geometry(seg.feature_type)) {
                has_auxiliary_extrusion = true;
            } else {
                has_real_extrusion = true;
            }
        }
    }
    if (has_real_extrusion || !has_auxiliary_extrusion) {
        return;
    }

    for (auto& layer : layers_) {
        for (auto& seg : layer.segments) {
            if (is_auxiliary_geometry(seg.feature_type)) {
                seg.feature_type = FeatureType::Unknown;
            }
        }
    }

    // Re-apply every rule add_segment() gated on the auxiliary answer. The
    // trigger guarantees none of these fired during parse (no non-auxiliary
    // extrusion existed), so the bounds start empty and the running drawable
    // count missed every promoted segment. The file's first segment starts at
    // the implicit (0,0,0) origin and must not frame the print - the same
    // skip add_segment() applies. Per-layer extrusion/travel counts never
    // filtered auxiliary and need no rebuild.
    drawable_segments_ = 0;
    bool is_first_segment = true;
    for (auto& layer : layers_) {
        for (auto& seg : layer.segments) {
            const bool auxiliary = is_auxiliary_geometry(seg.feature_type);
            if (!auxiliary) {
                ++drawable_segments_;
            }
            if (seg.is_extrusion && !auxiliary) {
                if (!is_first_segment) {
                    layer.bounding_box.expand(seg.start);
                    global_bounds_.expand(seg.start);
                }
                layer.bounding_box.expand(seg.end);
                global_bounds_.expand(seg.end);

                // Object boxes filtered auxiliary during parse too; re-expand
                // from the interned name each promoted segment carries. The
                // interned "__WIPE_TOWER__" name has no GCodeObject, so the
                // find() guard skips it.
                if (seg.object_name_index >= 0 &&
                    static_cast<size_t>(seg.object_name_index) < object_name_table_.size()) {
                    auto it = objects_.find(object_name_table_[seg.object_name_index]);
                    if (it != objects_.end()) {
                        it->second.bounding_box.expand(seg.start);
                        it->second.bounding_box.expand(seg.end);
                    }
                }
            }
            is_first_segment = false;
        }
    }

    spdlog::debug("[GCode Parser] Auxiliary-only extrusion promoted to the print: {} segments",
                  drawable_segments_);
}

ParsedGCodeFile GCodeParser::finalize(bool whole_file) {
    // start_new_layer() shrinks each layer as it is closed out; the last one never gets that
    // pass, so trim it here before the layers move into the result.
    if (!layers_.empty()) {
        layers_.back().segments.shrink_to_fit();
    }

    if (whole_file) {
        promote_auxiliary_only_extrusion();
    }

    ParsedGCodeFile result;
    result.filename = "";
    result.layers = std::move(layers_);
    result.objects = std::move(objects_);
    result.global_bounding_box = global_bounds_;

    // Calculate statistics
    for (const auto& layer : result.layers) {
        result.total_segments += layer.segments.size();
    }
    result.drawable_segments = drawable_segments_;

    // Transfer metadata
    result.filament_type = metadata_filament_type_;
    result.filament_color_hex = metadata_filament_color_;
    result.nozzle_diameter_mm = metadata_nozzle_diameter_;

    // Transfer layer height + extrusion width metadata
    result.layer_height_mm = metadata_layer_height_;
    result.first_layer_height_mm = metadata_first_layer_height_;
    result.extrusion_width_mm = metadata_extrusion_width_;
    result.perimeter_extrusion_width_mm = metadata_perimeter_extrusion_width_;
    result.infill_extrusion_width_mm = metadata_infill_extrusion_width_;
    result.first_layer_extrusion_width_mm = metadata_first_layer_extrusion_width_;

    spdlog::debug("[GCode Parser] Layer height: {}mm, first layer: {}mm, extrusion width: {}mm",
                  result.layer_height_mm,
                  result.first_layer_height_mm > 0 ? result.first_layer_height_mm
                                                   : result.layer_height_mm,
                  result.extrusion_width_mm);

    // Transfer multi-color tool palette
    result.tool_color_palette = tool_color_palette_;

    // Transfer the set of tool indices actually referenced by T-commands.
    // For single-extruder files with palette metadata but no T-commands
    // (manual-swap multi-color, or single-tool prints), fall back to {0}
    // so the swatch UI still has something to render.
    result.tools_used_indices = tools_used_;
    if (result.tools_used_indices.empty() && !result.tool_color_palette.empty()) {
        result.tools_used_indices.insert(0);
    }

    // Transfer interned object name table
    result.object_name_table = std::move(object_name_table_);

    spdlog::debug("[GCode Parser] Parsed G-code: {} layers, {} segments, {} objects",
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
namespace {

const unsigned char base64_decode_table[256] = {
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

/// Re-encode a JPEG thumbnail as PNG, since every GCodeThumbnail consumer
/// expects PNG. Empty when it does not decode or exceeds 512px a side, which
/// still covers the largest card thumbnails slicers write (512x512).
std::vector<uint8_t> jpeg_to_png(const std::vector<uint8_t>& jpeg) {
#if defined(HELIX_PLATFORM_ESP32)
    (void)jpeg;
    return {};
#else
    constexpr int kMaxSide = 512;
    const int len = static_cast<int>(jpeg.size());
    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory(jpeg.data(), len, &w, &h, &channels) || w <= 0 || h <= 0 ||
        w > kMaxSide || h > kMaxSide) {
        return {};
    }
    std::unique_ptr<unsigned char, void (*)(void*)> rgba(
        stbi_load_from_memory(jpeg.data(), len, &w, &h, &channels, 4), stbi_image_free);
    if (!rgba) {
        return {};
    }
    unsigned char* encoded = nullptr;
    size_t encoded_size = 0;
    const unsigned err = lodepng_encode32(&encoded, &encoded_size, rgba.get(),
                                          static_cast<unsigned>(w), static_cast<unsigned>(h));
    // lodepng allocates through lv_malloc, so the buffer goes back through lv_free.
    std::unique_ptr<unsigned char, void (*)(void*)> owned(encoded, lv_free);
    if (err != 0 || !encoded) {
        return {};
    }
    return {encoded, encoded + encoded_size};
#endif
}

bool is_jpeg(const std::vector<uint8_t>& data) {
    return data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

/// Scan header comments for embedded thumbnails ("; thumbnail begin WxH SIZE",
/// Cura's "; thumbnail_JPG begin WxH SIZE", or Creality's "; png begin W*H
/// SIZE") and decode the largest. A JPEG block that wins but cannot be
/// re-encoded yields to the largest PNG block.
template <typename NextLine>
GCodeThumbnail scan_best_thumbnail(NextLine next_line, std::string_view source) {
    struct Candidate {
        int width = 0;
        int height = 0;
        std::string base64;
        int pixels() const {
            return width * height;
        }
    };
    Candidate best_png;
    Candidate best_jpg;
    int width = 0, height = 0;
    std::string base64;
    bool in_block = false;
    bool block_is_jpg = false;
    std::string line;
    int lines_read = 0;
    constexpr int max_header_lines = 2000; // Thumbnails should be in first ~2000 lines

    while (lines_read < max_header_lines && next_line(line)) {
        lines_read++;

        size_t begin_pos = line.find("; thumbnail begin ");
        size_t begin_len = 18;
        bool jpg_header = false;
        if (begin_pos == std::string::npos) {
            begin_pos = line.find("; thumbnail_JPG begin ");
            begin_len = 22;
            jpg_header = begin_pos != std::string::npos;
        }
        size_t png_begin_pos = line.find("; png begin ");
        if (begin_pos != std::string::npos || png_begin_pos != std::string::npos) {
            const bool creality = begin_pos == std::string::npos;
            const char* dims =
                line.c_str() + (creality ? png_begin_pos + 12 : begin_pos + begin_len);
            int w = 0, h = 0, size = 0;
            if (sscanf(dims, creality ? "%d*%d %d" : "%dx%d %d", &w, &h, &size) >= 2 &&
                (!creality || (w > 0 && h > 0))) {
                width = w;
                height = h;
                base64.clear();
                in_block = true;
                block_is_jpg = jpg_header;
                spdlog::debug("[GCode Parser] Found {}thumbnail {}x{} in {}",
                              creality ? "Creality " : "", w, h, source);
            }
            continue;
        }

        if (in_block && (line.find("; thumbnail end") != std::string::npos ||
                         line.find("; thumbnail_JPG end") != std::string::npos ||
                         line.find("; png end") != std::string::npos)) {
            Candidate& slot = block_is_jpg ? best_jpg : best_png;
            if (!base64.empty() && width * height > slot.pixels()) {
                slot.width = width;
                slot.height = height;
                slot.base64.swap(base64);
            }
            in_block = false;
            continue;
        }

        // Accumulate base64 data (lines start with "; ")
        if (in_block && line.size() > 2 && line[0] == ';' && line[1] == ' ') {
            base64 += line.substr(2);
        }

        // Thumbnails live in the header, but slicers emit setup commands (M73,
        // M104, G21, ...) around them. The first motion command ends the header.
        if (!in_block && line.size() > 2 && line[0] == 'G' && line[1] >= '0' && line[1] <= '3' &&
            (line[2] == ' ' || line[2] == '\t')) {
            break;
        }
    }

    // Largest first; only the winner is decoded unless it fails.
    std::vector<const Candidate*> order;
#if !defined(HELIX_PLATFORM_ESP32)
    if (!best_jpg.base64.empty()) {
        order.push_back(&best_jpg);
    }
#endif
    if (!best_png.base64.empty()) {
        order.push_back(&best_png);
    }
    std::stable_sort(order.begin(), order.end(), [](const Candidate* a, const Candidate* b) {
        return a->pixels() > b->pixels();
    });

    GCodeThumbnail best;
    for (const Candidate* c : order) {
        std::vector<uint8_t> data = base64_decode(c->base64);
        if (is_jpeg(data)) {
            data = jpeg_to_png(data);
        }
        if (!data.empty()) {
            best.width = c->width;
            best.height = c->height;
            best.png_data = std::move(data);
            break;
        }
    }
    spdlog::debug("[GCode Parser] Best thumbnail {}x{} ({} bytes) from {}", best.width, best.height,
                  best.png_data.size(), source);
    return best;
}

} // namespace

GCodeThumbnail get_best_thumbnail(const std::string& filepath) {
    helix::text_io::LineReader file(filepath);
    if (!file) {
        spdlog::warn("[GCode Parser] Cannot open G-code file for thumbnail extraction: {}",
                     filepath);
        return {};
    }
    return scan_best_thumbnail([&file](std::string& line) { return file.next(line); }, filepath);
}

GCodeThumbnail get_best_thumbnail_from_content(const std::string& content) {
    auto records = helix::text_io::lines(content);
    auto it = records.begin();
    const auto end = records.end();
    return scan_best_thumbnail(
        [&](std::string& line) {
            if (it == end)
                return false;
            line.assign(it->data(), it->size());
            ++it;
            return true;
        },
        "content");
}

bool save_thumbnail_to_file(const std::string& gcode_path, const std::string& output_path) {
    GCodeThumbnail thumb = get_best_thumbnail(gcode_path);
    if (thumb.png_data.empty()) {
        spdlog::debug("[GCode Parser] No thumbnail found in {}", gcode_path);
        return false;
    }

    if (!helix::text_io::write_file(
            output_path, std::string_view(reinterpret_cast<const char*>(thumb.png_data.data()),
                                          thumb.png_data.size()))) {
        spdlog::error("[GCode Parser] Cannot write thumbnail to {}", output_path);
        return false;
    }
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
            // The consumer decodes the file as a PNG, so a fresh-looking entry that
            // is not one (e.g. raw JPEG bytes) is as useless as a missing entry.
            const auto head = helix::text_io::read_file(cache_path, 8);
            if (head && head->size() == 8 && head->compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0) {
                spdlog::trace("[GCode Parser] Using cached thumbnail: {}", cache_path);
                return cache_path;
            }
            spdlog::warn("[GCode Parser] Cached thumbnail {} is not a PNG, regenerating",
                         cache_path);
            std::remove(cache_path.c_str());
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
        if (const auto t = helix::text_io::parse_leading<double>(line.substr(cura_time.length()))) {
            metadata.estimated_time_seconds = *t;
            return true;
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
            // Estimate grams as PLA (1.24 g/cm³) at the default diameter: the
            // file names neither, and the parser has no printer to ask.
            metadata.filament_used_g =
                filament::length_to_weight_g(static_cast<float>(metadata.filament_used_mm), 1.24f);
        }
        return true;
    }

    // ====================
    // Cura format: ";Layer height: 0.12"
    // ====================
    const std::string cura_layer_height = ";Layer height: ";
    if (line.rfind(cura_layer_height, 0) == 0) {
        if (const auto h =
                helix::text_io::parse_leading<double>(line.substr(cura_layer_height.length()))) {
            metadata.layer_height = *h;
        }
        return true;
    }

    // ====================
    // Standard key=value or key: value format (OrcaSlicer/PrusaSlicer)
    // ====================
    const auto kv = split_comment_kv(line);
    if (!kv) {
        return false;
    }
    const std::string_view key = kv->first;
    const std::string value(kv->second);

    // Map known keys to metadata fields
    if (key == "generated by" || key == "slicer") {
        metadata.slicer = value;
    } else if (key == "slicer_version") {
        metadata.slicer_version = value;
    } else if (key == "estimated printing time" || key == "estimated printing time (normal mode)") {
        if (const auto seconds = parse_slicer_duration(value)) {
            metadata.estimated_time_seconds = *seconds;
        }
    } else if (key == "total filament used [g]" || key == "filament used [g]" ||
               key == "total filament weight") {
        // Multi-tool slicers emit "0.00, 0.00, 0.00, 0.00, 10.16" — record per-tool
        // breakdown if there are commas (OrcaSlicer / PrusaSlicer / Bambu format),
        // and use the sum as the total. Single-value form is handled the same way
        // (one element in the vector, total = that value).
        if (value.find(',') != std::string::npos) {
            metadata.filament_used_per_tool_g.clear();
            double total_g = 0.0;
            size_t pos = 0;
            while (pos < value.size()) {
                size_t end = value.find(',', pos);
                if (end == std::string::npos) {
                    end = value.size();
                }
                std::string token = value.substr(pos, end - pos);
                const auto v = helix::text_io::parse_leading<double>(token);
                if (v) {
                    metadata.filament_used_per_tool_g.push_back(*v);
                    total_g += *v;
                } else {
                    metadata.filament_used_per_tool_g.push_back(0.0);
                }
                pos = end + 1;
            }
            // Only overwrite the total if the "total filament used [g]" line hasn't
            // already set it directly (key == "total filament used [g]" wins
            // because it's the slicer's authoritative sum).
            if (metadata.filament_used_g == 0.0 || key == "filament used [g]") {
                metadata.filament_used_g = total_g;
            }
        } else {
            if (const auto v = helix::text_io::parse_leading<double>(value)) {
                metadata.filament_used_g = *v;
            }
        }
    } else if (key == "filament used [mm]" || key == "total filament used [mm]") {
        if (const auto v = helix::text_io::parse_leading<double>(value)) {
            metadata.filament_used_mm = *v;
        }
    } else if (key == "total layers" || key == "total layer number") {
        if (const auto v = helix::text_io::parse_leading<unsigned long>(value)) {
            metadata.layer_count = static_cast<uint32_t>(*v);
        }
    } else if (key == "first_layer_bed_temperature" || key == "bed_temperature") {
        if (const auto v = helix::text_io::parse_leading<double>(value)) {
            metadata.first_layer_bed_temp = *v;
        }
    } else if (key == "first_layer_temperature" || key == "nozzle_temperature") {
        if (const auto v = helix::text_io::parse_leading<double>(value)) {
            metadata.first_layer_nozzle_temp = *v;
        }
    } else if (key == "layer_height") {
        if (const auto v = helix::text_io::parse_leading<double>(value)) {
            metadata.layer_height = *v;
        }
    } else if (key == "first_layer_height") {
        if (const auto v = helix::text_io::parse_leading<double>(value)) {
            metadata.first_layer_height = *v;
        }
    } else if (key == "max_z_height") {
        if (const auto v = helix::text_io::parse_leading<double>(value)) {
            metadata.object_height = *v;
        }
    } else if (key == "filament_type") {
        // Slicers output multiple types separated by semicolons (e.g., "PLA;PLA;ASA;PETG")
        // Preserve full string for per-tool material matching
        metadata.filament_type = value;
    } else if (key == "extruder_colour" || key == "filament_colour") {
        std::vector<std::string> palette;
        if (helix::gcode::parse_filament_color_palette(line, palette)) {
            metadata.tool_colors = std::move(palette);
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

    namespace tio = helix::text_io;
    tio::File file = tio::open_file(filepath, "rb");
    if (!file || !tio::seek(file.get(), 0, SEEK_END)) {
        return lines;
    }

    const std::int64_t file_size = tio::tell(file.get()).value_or(0);
    if (file_size <= 0) {
        return lines;
    }

    // Calculate start position
    const auto want = static_cast<std::int64_t>(bytes_to_read);
    const std::int64_t start_pos = file_size > want ? file_size - want : 0;
    if (!tio::seek(file.get(), start_pos, SEEK_SET)) {
        return lines;
    }
    std::string content(static_cast<size_t>(file_size - start_pos), '\0');
    content.resize(std::fread(content.data(), 1, content.size(), file.get()));

    bool skip_partial_first = start_pos > 0;
    for (std::string_view line : helix::text_io::lines(content)) {
        if (skip_partial_first) {
            skip_partial_first = false;
            continue;
        }
        lines.emplace_back(line);
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

    helix::text_io::LineReader file(filepath);
    if (!file) {
        return metadata;
    }

    // Phase 1: Scan header (first ~500 lines) for slicer info, layer count, temps
    std::string line;
    int lines_read = 0;
    constexpr int max_header_lines = 500;

    while (file.next(line) && lines_read < max_header_lines) {
        lines_read++;

        // Skip non-comment lines
        if (line.empty() || line[0] != ';') {
            // Stop once the toolpath proper begins (a motion command). Only G
            // moves count: Cura emits M-code progress markers (M73) and temp
            // commands (M104/M140) in the header *before* its metadata comments
            // (;Generated with, ;TIME:, ;Layer height:), so breaking on M would
            // abort the scan before those are read. The first G command appears
            // after the comment header in every slicer dialect we support.
            if (!line.empty() && line[0] == 'G') {
                break;
            }
            continue;
        }

        parse_metadata_line(line, metadata);
    }

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

GCodeHeaderMetadata extract_header_metadata_from_content(const std::string& content) {
    GCodeHeaderMetadata metadata;

    int lines_read = 0;
    constexpr int max_header_lines = 500;

    for (std::string_view line_view : helix::text_io::lines(content)) {
        if (lines_read >= max_header_lines) {
            break;
        }
        const std::string line(line_view);
        lines_read++;

        if (line.empty() || line[0] != ';') {
            // Break only on motion (G) commands — see extract_header_metadata.
            // M-code setup lines (Cura's M73 progress markers) precede the
            // metadata comments and must not abort the scan.
            if (!line.empty() && line[0] == 'G') {
                break;
            }
            continue;
        }

        parse_metadata_line(line, metadata);
    }

    return metadata;
}

// ----------------------------------------------------------------------------
// Lightweight tool-change scan (memory-safe; no geometry model)
// ----------------------------------------------------------------------------

// Declared in gcode_parser.h — the single T-parse the whole tree shares. The
// layer index used to carry its own looser copy (see the header's note).
int tool_index_for_line(const std::string& raw, std::pair<size_t, size_t>* digits) {
    // Strip comment (everything from the first ';').
    std::string_view sv(raw);
    size_t comment_pos = sv.find(';');
    if (comment_pos != std::string_view::npos) {
        sv = sv.substr(0, comment_pos);
    }

    // Trim leading/trailing whitespace (covers spaces, tabs, and trailing \r).
    size_t start = 0;
    while (start < sv.length() && std::isspace(static_cast<unsigned char>(sv[start]))) {
        ++start;
    }
    size_t end = sv.length();
    while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1]))) {
        --end;
    }
    sv = sv.substr(start, end - start);

    // Must start with 'T' and have at least one following character.
    if (sv.length() < 2 || sv[0] != 'T') {
        return -1;
    }

    // Every remaining character must be a digit (standalone Tn only — "T0 X1"
    // and "TURN" are rejected, matching the full parser).
    for (size_t i = 1; i < sv.length(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(sv[i]))) {
            return -1;
        }
    }

    // sv[1..] is all digits; parse it. Guard against overflow on absurd input.
    long value = 0;
    for (size_t i = 1; i < sv.length(); ++i) {
        value = value * 10 + (sv[i] - '0');
        if (value > 100000) {
            return -1; // implausible tool index — ignore rather than overflow
        }
    }
    if (digits) {
        const size_t begin = static_cast<size_t>(sv.data() - raw.data()) + 1;
        *digits = {begin, begin + sv.length() - 1};
    }
    return static_cast<int>(value);
}

namespace {

// True when `seen` covers every index in `stop_set` (early-exit condition).
bool seen_all(const std::set<int>& seen, const std::set<int>& stop_set) {
    if (stop_set.empty())
        return false;
    for (int t : stop_set) {
        if (seen.count(t) == 0)
            return false;
    }
    return true;
}

} // namespace

std::set<int> scan_tools_used_from_content(const std::string& content,
                                           std::set<int> early_exit_full_set) {
    std::set<int> tools;
    std::string line;
    line.reserve(128);
    for (char ch : content) {
        if (ch == '\n') {
            int t = tool_index_for_line(line);
            if (t >= 0) {
                tools.insert(t);
                if (seen_all(tools, early_exit_full_set))
                    return tools;
            }
            line.clear();
        } else {
            line.push_back(ch);
        }
    }
    // Trailing line without a final newline.
    if (!line.empty()) {
        int t = tool_index_for_line(line);
        if (t >= 0) {
            tools.insert(t);
        }
    }
    return tools;
}

std::set<int> scan_tools_used_from_file(const std::string& filepath,
                                        std::set<int> early_exit_full_set) {
    std::set<int> tools;
    helix::text_io::LineReader in(filepath);
    if (!in) {
        return tools;
    }
    std::string line;
    while (in.next(line)) {
        // next() strips the '\n' but leaves a trailing '\r' on CRLF files;
        // tool_index_for_line() trims it as whitespace.
        int t = tool_index_for_line(line);
        if (t >= 0) {
            tools.insert(t);
            if (seen_all(tools, early_exit_full_set))
                break;
        }
    }
    return tools;
}

} // namespace gcode
} // namespace helix
