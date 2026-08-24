// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file gcode_parser.h
 * @brief Streaming G-code parser extracting toolpath, layers, and metadata
 *
 * @pattern Line-by-line streaming (no full buffer); layer-indexed geometry
 * @threading Main thread only
 * @gotchas clear_segments() frees 40-160MB after geometry build; layer detection via Z changes
 */

#pragma once

#include <spdlog/spdlog.h>

#include <array>
#include <climits>
#include <cmath>
#include <glm/glm.hpp>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace helix {
namespace gcode {

/**
 * @brief Axis-aligned bounding box for spatial queries
 */
struct AABB {
    glm::vec3 min{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                  std::numeric_limits<float>::infinity()};
    glm::vec3 max{-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                  -std::numeric_limits<float>::infinity()};

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
     * @return true if empty (min > max)
     */
    bool is_empty() const {
        return min.x > max.x || min.y > max.y || min.z > max.z;
    }

    /**
     * @brief Copy with every axis pair ordered so min <= max.
     *
     * A box assembled field-by-field from independently tracked sources (the
     * streaming index's stats, slicer metadata) can arrive with one axis
     * inverted while the others hold good data. Every consumer gates on
     * is_empty(), which is a single OR across all three axes, so one bad axis
     * silently discards the two that were fine — the 2D auto-fit falls back to
     * scale 1.0 about the world origin and the 3D camera declines to move.
     *
     * Axes that were never written keep their sentinel (±inf from this struct,
     * or the float max/lowest pair the index stats use) and stay inverted:
     * they carry no extent to order, and fabricating a span from the sentinels
     * would turn "no geometry" into a plausible-looking box.
     */
    AABB normalized() const {
        AABB out = *this;
        order_axis(out.min.x, out.max.x);
        order_axis(out.min.y, out.max.y);
        order_axis(out.min.z, out.max.z);
        return out;
    }

  private:
    /// Swap one axis pair into order, unless either side is still a sentinel.
    static void order_axis(float& lo, float& hi) {
        if (lo <= hi) {
            return;
        }
        if (!std::isfinite(lo) || !std::isfinite(hi)) {
            return;
        }
        if (lo >= std::numeric_limits<float>::max() || hi <= std::numeric_limits<float>::lowest()) {
            return;
        }
        std::swap(lo, hi);
    }

  public:
    /// 8 corners of the box. Order: bits 0/1/2 of the index select max for x/y/z.
    std::array<glm::vec3, 8> corners() const {
        return {{
            {min.x, min.y, min.z},
            {max.x, min.y, min.z},
            {min.x, max.y, min.z},
            {max.x, max.y, min.z},
            {min.x, min.y, max.z},
            {max.x, min.y, max.z},
            {min.x, max.y, max.z},
            {max.x, max.y, max.z},
        }};
    }

    /// Visit each of the 24 corner-bracket arm segments (8 corners × 3 axes)
    /// in world space. `fn(origin, tip)` fires once per arm. Both the 2D
    /// `GCodeLayerRenderer::render_selection_brackets` and the 3D
    /// `GCodeGLESRenderer::render_brackets_3d` share this geometry — the
    /// renderers differ only in how they emit the resulting line segments.
    template <typename F> void for_each_bracket_arm(float arm_length, F&& fn) const {
        const auto cs = corners();
        for (int c = 0; c < 8; ++c) {
            const glm::vec3& origin = cs[c];
            for (int axis = 0; axis < 3; ++axis) {
                glm::vec3 tip = origin;
                tip[axis] += ((c & (1 << axis)) ? -arm_length : +arm_length);
                fn(origin, tip);
            }
        }
    }
};

/**
 * @brief Single toolpath segment (line segment in 3D space)
 *
 * Represents movement from start to end point. Can be either:
 * - Extrusion move (is_extrusion=true): Plastic is deposited
 * - Travel move (is_extrusion=false): Nozzle moves without extruding
 */
/**
 * @brief Slicer-emitted feature/section type (from ;TYPE: comments).
 *
 * Slicers annotate each region of extrusion with a `;TYPE:NAME` comment.
 * We normalize across OrcaSlicer/PrusaSlicer/Bambu (space-separated, e.g.
 * "Outer wall") and Cura (hyphenated UPPERCASE, e.g. "WALL-OUTER") into
 * a single enum so callers can filter (e.g. exclude purge from bounding
 * box) without slicer-specific string compares.
 *
 * Stored as int8_t in ToolpathSegment to fill existing padding.
 */
enum class FeatureType : int8_t {
    Unknown = -1,  ///< No ;TYPE: seen, or unrecognized value
    Custom = 0,    ///< Start/end gcode, priming, manual purge (EXCLUDED FROM BOUNDS)
    Skirt,         ///< Skirt loop (physical, included in bounds)
    Brim,          ///< Brim (physical, included in bounds)
    OuterWall,     ///< Outer perimeter
    InnerWall,     ///< Inner perimeter
    OverhangWall,  ///< Overhanging perimeter
    SparseInfill,  ///< Sparse (low-density) infill
    SolidInfill,   ///< Solid infill, top/bottom skin
    TopSurface,    ///< Top surface
    BottomSurface, ///< Bottom surface
    Bridge,        ///< Bridging extrusion
    GapInfill,     ///< Gap fill between features
    Support,       ///< Support material (included — it's physical)
    WipeTower,     ///< Multi-color purge tower (EXCLUDED FROM BOUNDS)
};

/**
 * @brief Should this feature type be excluded from auto-fit bounding box?
 *
 * Returns true for types that produce extrusion outside the user's intended
 * print object: Custom (start/end gcode purge), WipeTower (multi-color
 * purge structure). Returns false for everything else, including Skirt and
 * Brim which are physical and inside the user's mental model of the print.
 */
constexpr bool is_excluded_from_bounds(FeatureType t) {
    return t == FeatureType::Custom || t == FeatureType::WipeTower;
}

struct ToolpathSegment {
    glm::vec3 start{0.0f, 0.0f, 0.0f}; ///< Start point (X, Y, Z) — 12 bytes
    glm::vec3 end{0.0f, 0.0f, 0.0f};   ///< End point (X, Y, Z) — 12 bytes
    float extrusion_amount{0.0f};      ///< E-axis delta (mm of filament) — 4 bytes
    float width{0.0f};                 ///< Calculated extrusion width (mm), 0=default — 4 bytes
    int16_t object_name_index{-1};     ///< Index into string table (-1 = no object) — 2 bytes
    uint16_t layer_index{0};  ///< Source layer index (set during geometry collection) — 2 bytes
    int8_t tool_index{0};     ///< Which tool/extruder printed this (0-15) — 1 byte
    bool is_extrusion{false}; ///< true if extruding, false if travel move — 1 byte
    FeatureType feature_type{FeatureType::Unknown}; ///< Slicer-annotated section type — 1 byte
    int8_t _pad{0};                                 ///< Reserved (was padding) — 1 byte
    // total 40 bytes
};
static_assert(sizeof(ToolpathSegment) == 40, "ToolpathSegment should be 40 bytes after interning");

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
    std::vector<std::string> object_name_table; ///< Interned object name strings

    // Statistics
    size_t total_segments{0};                 ///< Total segment count
    float estimated_print_time_minutes{0.0f}; ///< From metadata (if available)
    float total_filament_mm{0.0f};            ///< From metadata (if available)

    // Slicer metadata (parsed from comments)
    std::string slicer_name;        ///< Slicer software name and version
    std::string filament_type;      ///< Filament material type (e.g., "PLA", "PETG")
    std::string filament_color_hex; ///< Filament color in hex format (e.g., "#26A69A")
    std::string printer_model;      ///< Printer model name
    float nozzle_diameter_mm{0.0f}; ///< Nozzle diameter in mm
    float filament_weight_g{0.0f};  ///< Total filament weight in grams
    float filament_cost{0.0f};      ///< Estimated filament cost
    int total_layer_count{0};       ///< Total layer count from metadata

    // Extrusion width metadata (from OrcaSlicer/PrusaSlicer headers)
    float extrusion_width_mm{0.0f}; ///< Default extrusion width (0 = use nozzle-based default)
    float perimeter_extrusion_width_mm{0.0f};   ///< Perimeter width
    float infill_extrusion_width_mm{0.0f};      ///< Infill width
    float first_layer_extrusion_width_mm{0.0f}; ///< First layer width
    float filament_diameter_mm{1.75f};          ///< Filament diameter (default: 1.75mm)
    float layer_height_mm{0.2f};                ///< Layer height (default: 0.2mm)
    float first_layer_height_mm{0.0f};          ///< First layer height (0 = use layer_height)

    // Multi-color support
    std::vector<std::string> tool_color_palette; ///< Hex colors per tool (e.g., ["#ED1C24", ...])

    /// Tool indices actually referenced by the gcode (T-commands seen during parse).
    /// For single-extruder files with slicer color metadata but no T-commands, this
    /// contains {0} (implicit default tool) so swatch UIs still render. Distinct
    /// from `tool_color_palette` which holds every color the slicer emitted.
    std::set<int> tools_used_indices;

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

    /**
     * @brief Resolve an object name index to a string
     * @param index Index from ToolpathSegment::object_name_index
     * @return Object name string, or empty string if index is invalid
     */
    const std::string& get_object_name(int16_t index) const {
        static const std::string empty;
        if (index < 0 || static_cast<size_t>(index) >= object_name_table.size())
            return empty;
        return object_name_table[index];
    }

    /**
     * @brief Intern an object name, returning its index
     * @param name Object name to intern
     * @return Index into object_name_table, or -1 if name is empty
     */
    int16_t intern_object_name(const std::string& name) {
        if (name.empty())
            return -1;
        for (size_t i = 0; i < object_name_table.size(); ++i) {
            if (object_name_table[i] == name)
                return static_cast<int16_t>(i);
        }
        if (object_name_table.size() >= static_cast<size_t>(INT16_MAX)) {
            spdlog::warn("[GCode] Object name table full ({} entries), ignoring: {}",
                         object_name_table.size(), name);
            return -1;
        }
        object_name_table.push_back(name);
        return static_cast<int16_t>(object_name_table.size() - 1);
    }

    /**
     * @brief Clear segment data to free memory
     *
     * After geometry is built, the raw segment data is no longer needed.
     * This frees the segment vectors while preserving metadata (bounding box,
     * statistics, slicer info, etc.). Call this after geometry building to
     * reduce memory usage by 40-160MB on large files.
     *
     * @return Bytes freed (approximate)
     */
    size_t clear_segments() {
        size_t freed = 0;
        for (auto& layer : layers) {
            // Packed struct with interned names — see the static_assert on ToolpathSegment
            freed += layer.segments.size() * sizeof(ToolpathSegment);
            layer.segments.clear();
            layer.segments.shrink_to_fit();
        }
        // Also clear the objects polygon data (rarely used after geometry build)
        for (auto& [name, obj] : objects) {
            freed += obj.polygon.size() * sizeof(glm::vec2);
            obj.polygon.clear();
            obj.polygon.shrink_to_fit();
        }
        return freed;
    }
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

    /**
     * @brief Seed the active tool index before parsing a chunk.
     *
     * Streaming mode parses each layer with a fresh parser, so any T-command
     * issued in the file's prologue is invisible to per-layer parses. Callers
     * that already scanned for the initial tool (e.g. GCodeLayerIndex) use
     * this to seed the parser so segments are tagged with the correct tool.
     */
    void set_active_tool_index(int tool) {
        if (tool >= 0) {
            current_tool_index_ = tool;
            tools_used_.insert(tool);
            if (initial_tool_index_ < 0) {
                initial_tool_index_ = tool;
            }
        }
    }

    /**
     * @brief Seed the head position before parsing a chunk.
     *
     * Streaming mode parses each layer with a fresh parser, so without
     * this the first move of every layer is drawn from (0,0) — producing
     * stray travel/extrusion lines from origin to the real print location
     * in the 2D viewer. Callers (GCodeLayerIndex) snapshot the position at
     * each layer boundary; this seeds the parser before consuming bytes.
     */
    void set_initial_position(float x, float y, float z) {
        current_position_ = {x, y, z};
    }

    /**
     * @brief Seed the active ;TYPE: section before parsing a chunk.
     *
     * Streaming mode parses each layer with a fresh parser, so a ;TYPE:
     * comment in the file prologue (e.g. ;TYPE:Custom before the purge
     * line) is invisible to per-layer parses. Callers (GCodeLayerIndex)
     * snapshot the active type at each layer boundary; this seeds the
     * parser so segments are tagged correctly for bbox filtering.
     */
    void set_initial_feature_type(FeatureType t) {
        current_feature_type_ = t;
    }

    /**
     * @brief Seed the extrusion mode and running E value before parsing a chunk.
     *
     * M82 (absolute E) / M83 (relative E) are emitted once in the file prologue,
     * before any layer's byte range. A fresh per-layer parser therefore defaults
     * to absolute E and reads each relative E delta as an absolute position, so
     * `e_delta = new_e - current_e_` measures the difference *between consecutive
     * deltas* instead of the delta itself.
     *
     * Spiral-vase G-code makes that fatal: the spiral is emitted as fixed-length
     * polygon steps, so consecutive lines carry an identical E value and the
     * computed delta is exactly zero — the entire wall is classified as travel
     * and never drawn (#1127). Ordinary G-code merely loses a majority of its
     * extrusion flags, which is invisible because enough segments survive.
     *
     * @param absolute  true for M82 (absolute E), false for M83 (relative E)
     * @param initial_e Running E value in effect at the chunk boundary. Only
     *                  meaningful in absolute mode; relative deltas are
     *                  independent of the starting value.
     */
    void set_initial_extrusion_mode(bool absolute, float initial_e = 0.0f) {
        is_absolute_extrusion_ = absolute;
        current_e_ = initial_e;
    }

    /**
     * @brief Seed layer-marker mode before parsing a chunk.
     *
     * `;LAYER_CHANGE` sits at the *end* of the preceding layer's byte range, so
     * a per-layer parse never sees one and falls back to legacy "every Z change
     * is a new layer" splitting. With spiral-vase G-code every move carries a Z,
     * which produced one Layer object (and its segment vector) per move — 65
     * sub-layers for a single real layer. Segments are concatenated afterwards
     * so output was correct, but the allocation churn was not.
     *
     * The index knows file-wide whether markers are in use; this hands that
     * knowledge to each per-layer parser.
     */
    void set_use_layer_markers(bool use_markers) {
        use_layer_markers_ = use_markers;
    }

    /**
     * @brief Normalize a slicer's ;TYPE: value into a FeatureType.
     *
     * Recognizes both OrcaSlicer/PrusaSlicer/Bambu space-separated names
     * ("Outer wall") and Cura hyphenated UPPERCASE ("WALL-OUTER"). Returns
     * FeatureType::Unknown for empty or unrecognized input — never throws.
     *
     * Exposed as a static method so callers (and tests) can probe the
     * normalization table without constructing a parser.
     */
    static FeatureType parse_feature_type_value(const std::string& value);

    /**
     * @brief Extract a `;TYPE:NAME` marker from an arbitrary G-code line.
     *
     * Returns the normalized FeatureType if the line contains a well-formed
     * `;TYPE:` comment (optionally preceded by whitespace after `;`).
     * Returns std::nullopt if the line has no such marker. Shared by the
     * full-file parser and the streaming indexer so both classify identical
     * input identically.
     */
    static std::optional<FeatureType> extract_type_marker(const char* line, size_t len);

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

    /**
     * @brief Get tool color palette parsed from metadata
     * @return Vector of hex color strings (e.g., ["#ED1C24", "#00C1AE"])
     *
     * Returns empty vector if no color metadata was found.
     */
    const std::vector<std::string>& get_tool_color_palette() const {
        return tool_color_palette_;
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
     * @brief Parse G92 (set position) command
     */
    void parse_set_position_command(const std::string& line);

    /**
     * @brief Parse G2/G3 arc command, linearizing into short line segments
     */
    void parse_arc_command(const std::string& line, bool clockwise);

    /**
     * @brief Parse EXCLUDE_OBJECT_* command
     * @param line Trimmed G-code line
     * @return true if parsed successfully
     */
    bool parse_exclude_object_command(const std::string& line);

    /**
     * @brief Parse slicer metadata from comment line
     * @param line Comment line (starts with ';')
     *
     * Extracts key-value pairs from slicer comments in OrcaSlicer/PrusaSlicer format.
     * Examples:
     * - "; filament_colour = #26A69A"
     * - "; estimated printing time (normal mode) = 29m 25s"
     * - "; printer_model = Flashforge Adventurer 5M Pro"
     */
    void parse_metadata_comment(const std::string& line);

    /**
     * @brief Parse extruder color palette from header metadata
     * @param line Comment line containing extruder_colour or filament_colour
     *
     * Extracts semicolon-separated hex color values for multi-color prints.
     * Format: "; extruder_colour = #ED1C24;#00C1AE;#F4E2C1;#000000"
     */
    void parse_extruder_color_metadata(const std::string& line);

    /**
     * @brief Parse tool change command (T0, T1, T2, etc.)
     * @param line Trimmed G-code line
     *
     * Updates current_tool_index_ when tool change commands are encountered.
     */
    void parse_tool_change_command(const std::string& line);

    /**
     * @brief Parse wipe tower markers from comments
     * @param comment Comment string
     *
     * Detects WIPE_TOWER_START/END markers for optional wipe tower filtering.
     */
    void parse_wipe_tower_marker(const std::string& comment);

    /**
     * @brief Parse ;TYPE: section markers (per-region feature annotation)
     * @param comment Comment string (without leading ';')
     *
     * Updates current_feature_type_ when a `;TYPE:NAME` comment is seen.
     * Subsequent segments inherit the new type until the next `;TYPE:`.
     */
    void parse_type_marker(const std::string& comment);

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
     * @param e_delta Extruder delta (mm of filament), used to calculate width
     */
    void add_segment(const glm::vec3& start, const glm::vec3& end, bool is_extrusion,
                     float e_delta = 0.0f);

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
    std::string current_object_;       ///< Current object name (from EXCLUDE_OBJECT_START)
    int16_t current_object_index_{-1}; ///< Current object name index for segments
    std::unordered_map<std::string, int16_t> object_name_lookup_; ///< O(1) name → index
    std::vector<std::string> object_name_table_;                  ///< Accumulated string table
    bool is_absolute_positioning_{true}; ///< G90 (absolute) vs G91 (relative)
    bool is_absolute_extrusion_{true};   ///< M82 (absolute E) vs M83 (relative E)

    // Multi-color tool tracking
    int current_tool_index_{0};                   ///< Active extruder/tool (0-indexed)
    int initial_tool_index_{-1};                  ///< First T command seen (-1 = none)
    std::vector<std::string> tool_color_palette_; ///< Hex colors per tool: ["#ED1C24", ...]
    std::set<int> tools_used_;                    ///< Tool indices seen via T-commands
    bool in_wipe_tower_{false};                   ///< True when inside wipe tower section
    FeatureType current_feature_type_{FeatureType::Unknown}; ///< Active ;TYPE: section

    // Accumulated data
    std::vector<Layer> layers_;                  ///< All parsed layers
    std::map<std::string, GCodeObject> objects_; ///< Object metadata
    AABB global_bounds_;                         ///< Global bounding box

    // Parsed metadata (transferred to ParsedGCodeFile on finalize())
    std::string metadata_slicer_name_;
    std::string metadata_filament_type_;
    std::string metadata_filament_color_;
    std::string metadata_printer_model_;
    float metadata_nozzle_diameter_{0.0f};
    float metadata_filament_length_{0.0f};
    float metadata_filament_weight_{0.0f};
    float metadata_filament_cost_{0.0f};
    float metadata_print_time_{0.0f};
    int metadata_layer_count_{0};

    // Extrusion width metadata
    float metadata_extrusion_width_{0.0f};
    float metadata_perimeter_extrusion_width_{0.0f};
    float metadata_infill_extrusion_width_{0.0f};
    float metadata_first_layer_extrusion_width_{0.0f};
    float metadata_filament_diameter_{1.75f}; ///< Filament diameter (default: 1.75mm)
    float metadata_layer_height_{0.2f};       ///< Layer height (default: 0.2mm)
    float metadata_first_layer_height_{0.0f}; ///< First layer height (0 = use layer_height)

    // Progress tracking
    size_t lines_parsed_{0};           ///< Line counter
    bool use_layer_markers_{false};    ///< True if ;LAYER_CHANGE markers found
    bool pending_layer_marker_{false}; ///< Layer change marker seen, layer not yet started

    // Warning counters (logged as summary in finalize() instead of per-segment)
    size_t out_of_range_width_count_{
        0}; ///< Count of segments with calculated width outside 0.1-2.0mm
};

// ============================================================================
// Thumbnail Extraction (Standalone Functions)
// ============================================================================

/**
 * @brief Thumbnail extracted from G-code file header
 *
 * G-code files embed thumbnails as base64-encoded PNG in comment blocks.
 * Multiple sizes may be present (e.g., 48x48 for printer LCD, 300x300 for web).
 */
struct GCodeThumbnail {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> png_data; ///< Decoded PNG binary data

    int pixel_count() const {
        return width * height;
    }
};

/**
 * @brief Extract all thumbnails from G-code file header
 *
 * Parses thumbnail blocks in the format:
 *   ; thumbnail begin WIDTHxHEIGHT SIZE
 *   ; <base64 data line 1>
 *   ; <base64 data line 2>
 *   ; ...
 *   ; thumbnail end
 *
 * @param filepath Path to the G-code file
 * @return Vector of thumbnails sorted largest-first. Empty if none found.
 */
std::vector<GCodeThumbnail> extract_thumbnails(const std::string& filepath);

/**
 * @brief Extract all thumbnails from G-code content string
 *
 * Same as extract_thumbnails() but works on string content instead of file.
 * Useful for processing downloaded gcode without writing to disk.
 *
 * @param content G-code content (typically first ~100KB of file header)
 * @return Vector of thumbnails sorted largest-first. Empty if none found.
 */
std::vector<GCodeThumbnail> extract_thumbnails_from_content(const std::string& content);

/**
 * @brief Get the largest thumbnail from a G-code file
 *
 * @param filepath Path to the G-code file
 * @return Largest thumbnail, or empty thumbnail if none found
 */
GCodeThumbnail get_best_thumbnail(const std::string& filepath);

/**
 * @brief Extract thumbnail and save to PNG file
 *
 * Extracts the largest thumbnail and writes raw PNG bytes to file.
 *
 * @param gcode_path Path to the G-code file
 * @param output_path Path for the output PNG file
 * @return true if successful, false if no thumbnail or write failed
 */
bool save_thumbnail_to_file(const std::string& gcode_path, const std::string& output_path);

/**
 * @brief Get or create cached thumbnail for a G-code file
 *
 * If cached thumbnail exists and is newer than G-code file, returns cache path.
 * Otherwise extracts thumbnail and saves to cache directory.
 *
 * @param gcode_path Path to the G-code file
 * @param cache_dir Directory for cached thumbnails
 * @return Path to cached PNG, or empty string if no thumbnail available
 */
std::string get_cached_thumbnail(const std::string& gcode_path, const std::string& cache_dir);

/**
 * @brief Decode base64 string to binary data
 *
 * @param encoded Base64 encoded string (may contain whitespace)
 * @return Decoded binary data
 */
std::vector<uint8_t> base64_decode(const std::string& encoded);

/**
 * @brief Basic metadata extracted from G-code header
 *
 * Lightweight struct for quick file listings without full toolpath parsing.
 */
struct GCodeHeaderMetadata {
    std::string filename;
    uint64_t file_size = 0;
    double modified_time = 0.0; ///< Unix timestamp
    std::string slicer;
    std::string slicer_version;
    double estimated_time_seconds = 0.0;
    double filament_used_mm = 0.0;
    double filament_used_g = 0.0;
    std::vector<double>
        filament_used_per_tool_g; ///< Per-tool grams from "; filament used [g] = a, b, c, ..."
                                  ///< Empty when the slicer didn't emit a per-tool breakdown.
    std::string filament_type;    ///< e.g., "PLA", "PETG", "ABS", "TPU", "ASA"
    uint32_t layer_count = 0;
    double layer_height = 0.0;       ///< mm per layer (e.g. 0.2)
    double first_layer_height = 0.0; ///< mm first layer (e.g. 0.3)
    double object_height = 0.0;      ///< mm max Z height of the print
    double first_layer_bed_temp = 0.0;
    double first_layer_nozzle_temp = 0.0;
    std::vector<std::string> tool_colors; ///< Hex colors per tool (e.g., ["#ED1C24", "#00C1AE"])
};

/**
 * @brief Quick metadata extraction from G-code header only
 *
 * Extracts just the header metadata (slicer info, print time, filament)
 * without parsing the full toolpath. Much faster for file listings.
 *
 * @param filepath Path to the G-code file
 * @return GCodeHeaderMetadata with basic info populated
 */
GCodeHeaderMetadata extract_header_metadata(const std::string& filepath);

/**
 * @brief Extract header metadata from in-memory content (e.g., partial HTTP download)
 *
 * Scans the provided string line-by-line for slicer metadata comments.
 * Useful when the file content was fetched via HTTP range request.
 *
 * @param content G-code file content (typically first ~16KB)
 * @return GCodeHeaderMetadata with whatever fields were found
 */
GCodeHeaderMetadata extract_header_metadata_from_content(const std::string& content);

/**
 * @brief Lightweight, memory-safe scan for the set of distinct tool indices a
 *        G-code body references.
 *
 * Matches standalone tool-change commands (bare `Tn`, possibly indented, with a
 * trailing comment) over the WHOLE content — tool changes occur throughout a
 * multi-color print body, so a header-only / partial scan is insufficient. This
 * retains ONLY the integer set; it never builds a geometry model, so it is safe
 * to run on memory-constrained devices (AD5M / 2D-only platforms) where the full
 * GCodeParser is too heavy.
 *
 * Semantics intentionally mirror GCodeParser::parse_tool_change_command():
 *   - A line is a tool change only if, after stripping a trailing `;` comment and
 *     surrounding whitespace, it is exactly `T` followed by one or more digits
 *     (e.g. "T0", "  T2  ", "T1 ; change"). Tokens like "T0 X1" or "TURN" are
 *     ignored, and `Tn` appearing inside a comment is ignored.
 *
 * @param content G-code content (a whole file, or any line-complete chunk).
 * @param early_exit_full_set When non-empty, scanning stops as soon as the
 *        seen set covers it (`seen ⊇ early_exit_full_set`). Soundness:
 *        callers pass the full slicer palette {0..N-1}; tools at indices
 *        >= palette size are already dropped by every downstream consumer
 *        (get_used_tool_info() filters to palette indices), so a missed
 *        beyond-palette tool is invisible. Empty set => scan whole content
 *        (back-compat).
 * @return Distinct tool indices seen. Unlike ParsedGCodeFile::tools_used_indices,
 *         this does NOT inject {0} for single-extruder files with only a color
 *         palette (it has no palette knowledge); callers needing that convention
 *         should fall back to {0} when this returns an empty set and tools exist.
 */
std::set<int> scan_tools_used_from_content(const std::string& content,
                                           std::set<int> early_exit_full_set = {});

/**
 * @brief Streaming, memory-safe variant of scan_tools_used_from_content() that
 *        reads a file line-by-line from disk.
 *
 * Never loads the whole file into memory — reads one line at a time and retains
 * only the distinct-tool-index set. Preferred over downloading the entire file
 * into a std::string on constrained devices.
 *
 * @param filepath Path to a local G-code file.
 * @param early_exit_full_set When non-empty, scanning stops as soon as the
 *        seen set covers it (`seen ⊇ early_exit_full_set`). Soundness:
 *        callers pass the full slicer palette {0..N-1}; tools at indices
 *        >= palette size are already dropped by every downstream consumer
 *        (get_used_tool_info() filters to palette indices), so a missed
 *        beyond-palette tool is invisible. Empty set => scan whole file
 *        (back-compat).
 * @return Distinct tool indices seen (empty if the file can't be opened).
 */
std::set<int> scan_tools_used_from_file(const std::string& filepath,
                                        std::set<int> early_exit_full_set = {});

} // namespace gcode
} // namespace helix
