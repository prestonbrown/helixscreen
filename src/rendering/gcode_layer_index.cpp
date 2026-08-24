// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_GCODE_VIEWER

#include "gcode_layer_index.h"

#include "gcode_color_metadata.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace helix {
namespace gcode {

namespace {

// Layer detection tolerance for Z changes
constexpr float Z_EPSILON = 0.001f;

// Minimum E delta that counts as extrusion rather than a travel/retract.
// Matches the threshold GCodeParser::parse_movement_command() uses so the index
// and the full-file parser agree on what "extruding" means.
constexpr float EXTRUSION_EPSILON = 0.00001f;

// Extract a single-letter float parameter (case-insensitive) from a G-code line.
// Returns true if found. Skips over coordinates embedded inside identifier
// tokens like "G1" by only matching at the start of a token (preceded by
// whitespace, comma, or start-of-line).
bool extract_axis_param(const char* line, size_t len, char axis, float& out_value) {
    char upper = axis;
    char lower = static_cast<char>(axis | 0x20);
    // Truncate at first comment so e.g. `G1 X10 Y20 ; X100 retract` doesn't
    // pick up the X inside the comment.
    for (size_t i = 0; i < len; ++i) {
        if (line[i] == ';') {
            len = i;
            break;
        }
    }
    for (size_t i = 0; i < len; ++i) {
        if (line[i] != upper && line[i] != lower) {
            continue;
        }
        // Must be at token start: preceded by whitespace or beginning of line.
        if (i > 0) {
            char prev = line[i - 1];
            if (prev != ' ' && prev != '\t' && prev != ',') {
                continue;
            }
        }
        if (i + 1 >= len) {
            continue;
        }
        char next = line[i + 1];
        if (next != '-' && next != '+' && next != '.' && (next < '0' || next > '9')) {
            continue;
        }
        char* end = nullptr;
        float v = std::strtof(line + i + 1, &end);
        if (end != line + i + 1) {
            out_value = v;
            return true;
        }
    }
    return false;
}

bool extract_z_param(const char* line, size_t len, float& out_z) {
    return extract_axis_param(line, len, 'Z', out_z);
}

// Check if line is a movement command (G0 or G1)
bool is_movement_command(const char* line, size_t len) {
    // Skip leading whitespace
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }

    // Check for G0 or G1
    if (i + 1 < len && (line[i] == 'G' || line[i] == 'g')) {
        char next = line[i + 1];
        if (next == '0' || next == '1') {
            // Make sure it's not G10, G100, etc.
            if (i + 2 >= len || line[i + 2] == ' ' || line[i + 2] == '\t' || line[i + 2] == ';' ||
                line[i + 2] == '\r' || line[i + 2] == '\n') {
                return true;
            }
            // G0 followed by coordinate is also valid (G0X10)
            char after = line[i + 2];
            if (after == 'X' || after == 'Y' || after == 'Z' || after == 'E' || after == 'F' ||
                after == 'x' || after == 'y' || after == 'z' || after == 'e' || after == 'f') {
                return true;
            }
        }
    }
    return false;
}

// NOTE: a has_positive_extrusion() text heuristic used to live here and drive
// the extrusion/travel stats. It scanned for an 'E' followed by a digit or '+',
// which missed OrcaSlicer's leading-dot form (E.05482 — so every Orca file was
// counted as ~100% travel) and could not distinguish a retraction from an
// extrusion. build_from_file() now tracks M82/M83 and the running E value and
// computes a real delta, which is both correct and what the XY bounds depend on.

// Extract filament/extruder color from metadata comment
// Thin wrapper over helix::gcode::parse_filament_color_palette() that also
// projects the legacy single-color field (palette[0]) for backward compat.
bool extract_filament_color(const char* line, size_t len, std::string& out_color,
                            std::vector<std::string>& out_palette) {
    if (len < 10 || line[0] != ';') {
        return false;
    }
    if (!helix::gcode::parse_filament_color_palette(std::string_view(line, len), out_palette)) {
        return false;
    }
    // First non-empty entry is the legacy "single color" surface.
    for (const auto& s : out_palette) {
        if (!s.empty()) {
            out_color = s;
            return true;
        }
    }
    return false;
}

// Check if line is a layer change marker.
// Matches ";LAYER_CHANGE" / "; LAYER_CHANGE" (case-insensitive) where the marker
// is the entire comment content (or terminated by whitespace). Rejects OrcaSlicer's
// ";BEFORE_LAYER_CHANGE" / ";AFTER_LAYER_CHANGE" — those contain "LAYER_CHANGE"
// as a substring but are bracketing tags for the before-layer-change user macro,
// not real layer transitions.
bool is_layer_marker(const char* line, size_t len) {
    const char* marker = "LAYER_CHANGE";
    constexpr size_t marker_len = 12;
    for (size_t i = 0; i + marker_len <= len; ++i) {
        if (line[i] != ';') {
            continue;
        }
        size_t j = i + 1;
        while (j < len && line[j] == ' ') {
            ++j;
        }
        if (j + marker_len > len) {
            continue;
        }
        bool match = true;
        for (size_t k = 0; k < marker_len; ++k) {
            char c = line[j + k];
            char m = marker[k];
            if (c != m && c != (m + 32) && (c - 32) != m) {
                match = false;
                break;
            }
        }
        if (!match) {
            continue;
        }
        // Marker must be terminated by end-of-line or whitespace (not a letter,
        // digit, or underscore — which would mean it's a longer identifier like
        // a continuation of BEFORE_/AFTER_ that we missed).
        size_t end = j + marker_len;
        if (end < len) {
            char c = line[end];
            if (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9')) {
                continue;
            }
        }
        return true;
    }
    return false;
}

} // anonymous namespace

bool GCodeLayerIndex::build_from_file(const std::string& filepath) {
    auto start_time = std::chrono::high_resolution_clock::now();

    // Clear any previous data
    entries_.clear();
    stats_ = LayerIndexStats{};
    source_path_ = filepath;
    uses_layer_markers_ = false;

    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        spdlog::error("[LayerIndex] Failed to open file: {}", filepath);
        return false;
    }

    // Get file size
    stats_.total_bytes = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    spdlog::debug("[LayerIndex] Building index for {} ({} bytes)", filepath, stats_.total_bytes);

    // Reserve estimated capacity (assume ~100 layers for now)
    entries_.reserve(100);

    // Read line by line
    std::string line;
    line.reserve(256);

    float current_z = -std::numeric_limits<float>::infinity();
    // Running head position. Snapshotted into each new layer entry so the
    // streaming parser can be seeded — without it, the first move of every
    // layer would be drawn from (0,0). We assume G90 absolute mode (the
    // ubiquitous OrcaSlicer/PrusaSlicer/Bambu convention); G91 relative
    // would silently desync but isn't used in slicer output.
    float current_x = 0.0f;
    float current_y = 0.0f;
    float current_seen_z =
        0.0f; // Z position seen so far (vs current_z which only updates on layer transition)
    // Running ;TYPE: section. Snapshotted into each new layer entry so the
    // streaming parser can be seeded — without it, segments in the prologue
    // (purge) get tagged Unknown and the bbox filter can't exclude them.
    FeatureType current_feature_type = FeatureType::Unknown;
    // Running extrusion state. M82/M83 appear once in the prologue, before every
    // layer byte range, so each entry snapshots the mode and E value in effect at
    // its offset for the per-layer parser to be seeded with (#1127). Tracking E
    // here also lets the scan compute a real extrusion delta instead of the
    // "is there an E followed by a digit" guess it used before — which is what
    // makes the XY bounds below trustworthy.
    bool absolute_extrusion = true;
    float current_e = 0.0f;
    // False until a move has established a real head position, so the implicit
    // (0,0) origin never enters the XY bounds.
    bool any_position_seen = false;
    uint64_t current_layer_start = 0;
    uint64_t current_offset = 0;
    uint16_t current_layer_lines = 0;
    bool use_layer_markers = false;
    bool pending_layer_start = false;
    bool first_layer_started = false;

    while (std::getline(file, line)) {
        size_t line_len = line.length();
        stats_.total_lines++;

        // Check for layer marker
        if (is_layer_marker(line.c_str(), line_len)) {
            if (!use_layer_markers) {
                // First marker in the file. Marker mode cannot be known until
                // one appears, so the Z-change heuristic has been live over the
                // prologue — and a START_PRINT that lifts to a bed-clearance
                // height (OrcaSlicer emits `G1 Z3 F600` before the first
                // ;LAYER_CHANGE) looks exactly like a layer to it. Drop whatever
                // it guessed: none of it is a real layer, and layer 0 is about
                // to be created by this marker.
                entries_.clear();
                first_layer_started = false;
                current_layer_lines = 0;
            }
            use_layer_markers = true;
            pending_layer_start = true;
            // We'll start the new layer when we see the next Z move
        }

        // Track extrusion mode. M82/M83 are whole-line commands in the prologue.
        if (line_len >= 3 && line[0] == 'M' && line[1] == '8' &&
            (line[2] == '2' || line[2] == '3') &&
            (line_len == 3 || line[3] == ' ' || line[3] == ';' || line[3] == '\t' ||
             line[3] == '\r')) {
            absolute_extrusion = (line[2] == '2');
        }
        // G92 sets the current position of an axis without moving; G92 E0 is the
        // conventional absolute-mode extruder reset between layers/objects.
        if (line_len >= 3 && line[0] == 'G' && line[1] == '9' && line[2] == '2') {
            float e_reset;
            if (extract_axis_param(line.c_str(), line_len, 'E', e_reset)) {
                current_e = e_reset;
            }
        }

        // Track ;TYPE: section via the shared helper (kept in sync with the
        // full-file parser so streaming and full-file tag identically).
        if (auto t = GCodeParser::extract_type_marker(line.c_str(), line_len)) {
            current_feature_type = *t;
        }

        // Extract filament color from metadata (only if not already found)
        // Only check comment lines in the header (first ~1000 lines)
        if (stats_.filament_color.empty() && stats_.total_lines < 1000) {
            std::string color;
            std::vector<std::string> palette;
            if (extract_filament_color(line.c_str(), line_len, color, palette)) {
                stats_.filament_color = color;
                stats_.filament_palette = std::move(palette);
                spdlog::debug("[LayerIndex] Found filament palette ({} entries, first={})",
                              stats_.filament_palette.size(), color);
            }
        }

        // Track the first standalone T-command — streaming mode parses each
        // layer with a fresh GCodeParser, so without this, segments after a
        // PRINT_START toolchange in the prologue render as T0 (#776 / black-fill
        // bug for prints sliced to a non-T0 tool).
        if (stats_.initial_tool_index < 0 && line_len >= 2 && line[0] == 'T') {
            size_t i = 1;
            while (i < line_len && line[i] >= '0' && line[i] <= '9') {
                ++i;
            }
            if (i > 1 && (i == line_len || line[i] == ' ' || line[i] == '\t' || line[i] == '\r' ||
                          line[i] == ';')) {
                try {
                    stats_.initial_tool_index = std::stoi(line.substr(1, i - 1));
                    spdlog::debug("[LayerIndex] Initial tool: T{}", stats_.initial_tool_index);
                } catch (...) {
                    // Malformed T-line — leave as -1 and keep scanning
                }
            }
        }

        // Check for movement commands
        if (is_movement_command(line.c_str(), line_len)) {
            float z;
            if (extract_z_param(line.c_str(), line_len, z)) {
                // Z change detected
                bool is_new_layer = false;

                if (use_layer_markers) {
                    // Use marker-based layer detection
                    if (pending_layer_start) {
                        is_new_layer = true;
                        pending_layer_start = false;
                    }
                } else {
                    // Use Z-change based layer detection
                    if (z > current_z + Z_EPSILON) {
                        is_new_layer = true;
                    }
                }

                if (is_new_layer) {
                    // Finalize previous layer if any
                    if (first_layer_started && current_layer_lines > 0) {
                        StreamingLayerEntry& last = entries_.back();
                        last.byte_length =
                            static_cast<uint32_t>(current_offset - current_layer_start);
                        last.line_count = current_layer_lines;
                    }

                    // Start new layer. Snapshot the head position BEFORE the
                    // line at file_offset has been applied — i.e., the
                    // position the streaming parser should be seeded with
                    // before it reads this layer's bytes.
                    StreamingLayerEntry entry{};
                    entry.file_offset = current_offset;
                    entry.z_height = z;
                    entry.byte_length = 0; // Will be filled when layer ends
                    entry.line_count = 0;  // Will be filled when layer ends
                    entry.flags = absolute_extrusion ? StreamingLayerEntry::FLAG_ABSOLUTE_EXTRUSION
                                                     : uint16_t{0};
                    entry.start_x = current_x;
                    entry.start_y = current_y;
                    entry.start_z = current_seen_z;
                    entry.start_feature_type = current_feature_type;
                    entry.start_e = current_e;
                    entries_.push_back(entry);

                    first_layer_started = true;

                    current_z = z;
                    current_layer_start = current_offset;
                    current_layer_lines = 0;
                }
                current_seen_z = z;
            }

            // Resolve the real extrusion delta for this move, honouring the
            // M82/M83 mode. The previous "E followed by a digit" heuristic
            // mis-flagged whole files: it missed OrcaSlicer's leading-dot form
            // (E.05482) and could not tell a retraction from an extrusion.
            float e_param;
            float e_delta = 0.0f;
            if (extract_axis_param(line.c_str(), line_len, 'E', e_param)) {
                const float new_e = absolute_extrusion ? e_param : current_e + e_param;
                e_delta = new_e - current_e;
                current_e = new_e;
            }
            const bool is_extruding = e_delta > EXTRUSION_EPSILON;

            // Update running X/Y from this move (for the next layer's snapshot),
            // and grow the model bounds over extruding moves only. Both the
            // start and end point of an extruding move are inside the model, but
            // the start is only valid once we have actually seen a prior move —
            // otherwise the implicit (0,0) origin drags the box to the corner.
            float v;
            const float prev_x = current_x;
            const float prev_y = current_y;
            bool moved = false;
            if (extract_axis_param(line.c_str(), line_len, 'X', v)) {
                current_x = v;
                moved = true;
            }
            if (extract_axis_param(line.c_str(), line_len, 'Y', v)) {
                current_y = v;
                moved = true;
            }

            if (is_extruding && !is_excluded_from_bounds(current_feature_type)) {
                if (any_position_seen) {
                    stats_.min_x = std::min(stats_.min_x, prev_x);
                    stats_.max_x = std::max(stats_.max_x, prev_x);
                    stats_.min_y = std::min(stats_.min_y, prev_y);
                    stats_.max_y = std::max(stats_.max_y, prev_y);
                }
                stats_.min_x = std::min(stats_.min_x, current_x);
                stats_.max_x = std::max(stats_.max_x, current_x);
                stats_.min_y = std::min(stats_.min_y, current_y);
                stats_.max_y = std::max(stats_.max_y, current_y);
                // Z rides the same gate as X/Y so the index reports the height
                // of extruded geometry, matching what the full-file parser puts
                // in global_bounding_box (the 3D path's input). Reading Z off
                // layer entries instead measured travel: it caught prologue
                // clearance moves and per-layer Z-hops, and because it recorded
                // "first entry" and "last entry" rather than min and max, a file
                // whose last hop sat below its prologue lift produced min > max.
                stats_.min_z = std::min(stats_.min_z, current_seen_z);
                stats_.max_z = std::max(stats_.max_z, current_seen_z);
            }
            if (moved) {
                any_position_seen = true;
            }

            // Track extrusion vs travel
            if (is_extruding) {
                stats_.extrusion_moves++;
            } else {
                stats_.travel_moves++;
            }
        }

        current_layer_lines++;
        // Account for line length + newline character
        current_offset += line_len + 1;
    }

    // Finalize last layer
    if (first_layer_started && !entries_.empty()) {
        StreamingLayerEntry& last = entries_.back();
        last.byte_length = static_cast<uint32_t>(stats_.total_bytes - current_layer_start);
        last.line_count = current_layer_lines;
    }

    stats_.total_layers = entries_.size();
    uses_layer_markers_ = use_layer_markers;

    // Release the slack left by the reserve(100) growth doubling. A 1200-layer
    // print reserves 1600 entries; handing 400 × 40 bytes back matters on a
    // 47MB-RAM AD5M.
    entries_.shrink_to_fit();

    // If no filament color found in header, scan the file footer (OrcaSlicer puts metadata at end)
    if (stats_.filament_color.empty() && stats_.total_bytes > 0) {
        // Read last 32KB of file to find metadata
        size_t footer_size = std::min(stats_.total_bytes, size_t(32768));
        file.clear();
        file.seekg(-static_cast<std::streamoff>(footer_size), std::ios::end);

        while (std::getline(file, line)) {
            std::string color;
            std::vector<std::string> palette;
            if (extract_filament_color(line.c_str(), line.length(), color, palette)) {
                stats_.filament_color = color;
                stats_.filament_palette = std::move(palette);
                spdlog::debug(
                    "[LayerIndex] Found filament palette in footer ({} entries, first={})",
                    stats_.filament_palette.size(), color);
                break;
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.build_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    spdlog::info("[LayerIndex] Built index: {} layers, {} lines, Z=[{:.2f}, {:.2f}], {:.1f}ms",
                 stats_.total_layers, stats_.total_lines, stats_.min_z, stats_.max_z,
                 stats_.build_time_ms);

    spdlog::debug("[LayerIndex] Memory usage: {} bytes ({} bytes/layer)", memory_usage_bytes(),
                  entries_.empty() ? 0 : memory_usage_bytes() / entries_.size());

    return !entries_.empty();
}

StreamingLayerEntry GCodeLayerIndex::get_entry(size_t layer_index) const {
    if (layer_index < entries_.size()) {
        return entries_[layer_index];
    }
    // Return invalid entry
    return StreamingLayerEntry{0, 0, 0.0f, 0, 0};
}

int GCodeLayerIndex::find_layer_at_z(float z) const {
    if (entries_.empty()) {
        return -1;
    }

    // Binary search for closest layer
    auto it = std::lower_bound(
        entries_.begin(), entries_.end(), z,
        [](const StreamingLayerEntry& entry, float target_z) { return entry.z_height < target_z; });

    if (it == entries_.end()) {
        return static_cast<int>(entries_.size() - 1);
    }

    size_t idx = std::distance(entries_.begin(), it);

    // Check if previous layer is closer
    if (idx > 0) {
        float dist_curr = std::abs(it->z_height - z);
        float dist_prev = std::abs((it - 1)->z_height - z);
        if (dist_prev < dist_curr) {
            return static_cast<int>(idx - 1);
        }
    }

    return static_cast<int>(idx);
}

float GCodeLayerIndex::get_layer_z(size_t layer_index) const {
    if (layer_index < entries_.size()) {
        return entries_[layer_index].z_height;
    }
    return 0.0f;
}

} // namespace gcode
} // namespace helix

#endif // HELIX_HAS_GCODE_VIEWER
