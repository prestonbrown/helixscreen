// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gcode_parser.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace helix {
namespace gcode {

/**
 * @brief Compact layer entry for streaming G-code access
 *
 * Instead of storing all segment data in memory, this stores just the
 * file byte offsets needed to load layers on-demand. Each entry is
 * 40 bytes vs ~80KB for a full layer with segment data.
 *
 * This enables viewing 10MB+ G-code files on memory-constrained devices
 * like AD5M (47MB RAM) by loading only the layers currently being viewed.
 */
struct StreamingLayerEntry {
    /// Bit flags for `flags`. Kept in the existing uint16 so seeding state costs
    /// no extra bytes per layer.
    enum Flags : uint16_t {
        /// Extrusion mode in effect at this layer's byte offset: set = M82
        /// (absolute E), clear = M83 (relative E). See
        /// GCodeParser::set_initial_extrusion_mode().
        FLAG_ABSOLUTE_EXTRUSION = 1u << 0,
    };

    uint64_t file_offset; ///< Byte offset in file where layer starts
    uint32_t byte_length; ///< Number of bytes in this layer
    float z_height;       ///< Z coordinate of this layer (mm)
    uint16_t line_count;  ///< Number of G-code lines in this layer
    uint16_t flags;       ///< Bitfield of Flags (see above)
    /// Head position when this layer's byte range begins. Streaming mode
    /// parses each layer with a fresh GCodeParser, so the parser must be
    /// seeded with this position — otherwise the first move of the layer
    /// is drawn from (0,0) and produces stray lines from origin in the 2D
    /// viewer.
    float start_x;
    float start_y;
    float start_z;
    /// Active ;TYPE: section at the start of this layer's byte range.
    /// Same seeding rationale as start_x/y/z and initial_tool_index — the
    /// prologue ;TYPE: comments live before file_offset, so without this
    /// the per-layer parser tags segments as Unknown and the bbox filter
    /// (auto_fit) can't exclude Custom/WipeTower from the viewport.
    FeatureType start_feature_type{FeatureType::Unknown};
    /// Running E value at this layer's byte offset. Only meaningful when
    /// FLAG_ABSOLUTE_EXTRUSION is set; relative-mode deltas do not depend on it.
    /// Occupies what was previously tail padding — the struct is still 40 bytes.
    float start_e{0.0f};

    /// Check if this entry is valid (has been populated)
    bool is_valid() const {
        return byte_length > 0;
    }

    /// Extrusion mode at this layer's byte offset (true = absolute / M82).
    bool is_absolute_extrusion() const {
        return (flags & FLAG_ABSOLUTE_EXTRUSION) != 0;
    }
};

/**
 * @brief Statistics collected during index building
 */
struct LayerIndexStats {
    size_t total_layers{0}; ///< Number of layers found
    size_t total_lines{0};  ///< Total G-code lines processed
    size_t total_bytes{0};  ///< Total file size
    /// Model Z extents, accumulated over the same extruding moves as the XY
    /// pair below so the two describe one box. Empty (min > max) if no
    /// extrusion was seen — see has_z_bounds().
    float min_z{std::numeric_limits<float>::max()};
    float max_z{std::numeric_limits<float>::lowest()};
    /// Model XY extents, accumulated over extruding moves during the index scan
    /// and filtered by is_excluded_from_bounds() exactly like the full-file
    /// parser's global_bounding_box. Empty (min > max) if no extrusion was seen.
    ///
    /// Before this existed, GCodeLayerRenderer::auto_fit() estimated XY bounds by
    /// loading three layers (first/middle/last) and unioning their segments — a
    /// guess that framed spiral-vase prints against a 3-point hull (#1127) and
    /// mis-framed any model whose widest cross-section wasn't one of the three.
    /// The scan already visits every line, so this costs nothing extra and lets
    /// auto_fit avoid loading layers altogether.
    float min_x{std::numeric_limits<float>::max()};
    float max_x{std::numeric_limits<float>::lowest()};
    float min_y{std::numeric_limits<float>::max()};
    float max_y{std::numeric_limits<float>::lowest()};
    size_t extrusion_moves{0}; ///< Count of extruding moves (real E delta > 0)
    size_t travel_moves{0};    ///< Count of G0/G1 without extrusion

    /// True when the scan accumulated at least one extruding move, i.e. min/max
    /// XY describe real geometry.
    bool has_xy_bounds() const {
        return min_x <= max_x && min_y <= max_y;
    }

    /// True when the scan accumulated at least one extruding move, i.e. min/max
    /// Z describe real geometry.
    bool has_z_bounds() const {
        return min_z <= max_z;
    }
    double build_time_ms{0.0};  ///< Time to build index
    std::string filament_color; ///< First filament color hex from metadata (palette[0]; legacy)
    std::vector<std::string>
        filament_palette;       ///< All filament colors from semicolon-separated metadata
    int initial_tool_index{-1}; ///< First T-command seen in the file (-1 = none)
};

/**
 * @brief Layer index for streaming G-code access
 *
 * Provides random access to layers without loading the entire file.
 * Built with a single-pass scan of the file, recording byte offsets
 * for each layer boundary.
 *
 * Usage:
 * @code
 *   GCodeLayerIndex index;
 *   if (index.build_from_file("model.gcode")) {
 *       // Get layer 50's offset
 *       auto entry = index.get_entry(50);
 *       // Read just that layer's bytes from file
 *       // ... seek to entry.file_offset, read entry.byte_length bytes
 *   }
 * @endcode
 *
 * Memory usage: 40 bytes × layer_count (e.g., 1000 layers = 40KB)
 */
class GCodeLayerIndex {
  public:
    GCodeLayerIndex() = default;
    ~GCodeLayerIndex() = default;

    // Non-copyable but moveable
    GCodeLayerIndex(const GCodeLayerIndex&) = delete;
    GCodeLayerIndex& operator=(const GCodeLayerIndex&) = delete;
    GCodeLayerIndex(GCodeLayerIndex&&) = default;
    GCodeLayerIndex& operator=(GCodeLayerIndex&&) = default;

    /**
     * @brief Build index from a G-code file
     *
     * Single-pass scan that identifies layer boundaries by detecting
     * Z-axis changes or ;LAYER_CHANGE markers. Records byte offset,
     * length, and line count for each layer.
     *
     * @param filepath Path to G-code file
     * @return true if successful, false on error
     */
    bool build_from_file(const std::string& filepath);

    /**
     * @brief Get entry for a specific layer
     *
     * @param layer_index Zero-based layer index
     * @return Layer entry, or invalid entry if out of range
     */
    StreamingLayerEntry get_entry(size_t layer_index) const;

    /**
     * @brief Get total number of layers
     * @return Layer count
     */
    size_t get_layer_count() const {
        return entries_.size();
    }

    /**
     * @brief Get file size that was indexed
     * @return File size in bytes
     */
    size_t get_file_size() const {
        return stats_.total_bytes;
    }

    /**
     * @brief Get index building statistics
     * @return Statistics from build process
     */
    const LayerIndexStats& get_stats() const {
        return stats_;
    }

    /**
     * @brief Check if index is populated
     * @return true if index has been built successfully
     */
    bool is_valid() const {
        return !entries_.empty();
    }

    /**
     * @brief Whether the file segments layers with `;LAYER_CHANGE` markers.
     *
     * File-wide property discovered during the scan. Per-layer parses need it
     * because the marker sits at the end of the *previous* layer's byte range
     * and is therefore invisible to them.
     */
    bool uses_layer_markers() const {
        return uses_layer_markers_;
    }

    /**
     * @brief Find layer index closest to Z height
     *
     * @param z Z coordinate to search for
     * @return Layer index (0-based), or -1 if no layers
     */
    int find_layer_at_z(float z) const;

    /**
     * @brief Get Z height for a layer
     *
     * @param layer_index Zero-based layer index
     * @return Z height in mm, or 0.0 if out of range
     */
    float get_layer_z(size_t layer_index) const;

    /**
     * @brief Get memory usage of this index
     * @return Approximate bytes used
     */
    size_t memory_usage_bytes() const {
        return sizeof(*this) + entries_.capacity() * sizeof(StreamingLayerEntry);
    }

    /**
     * @brief Clear the index to free memory
     */
    void clear() {
        entries_.clear();
        entries_.shrink_to_fit();
        stats_ = LayerIndexStats{};
        source_path_.clear();
        uses_layer_markers_ = false;
    }

    /**
     * @brief Get source file path
     * @return Path used in build_from_file()
     */
    const std::string& get_source_path() const {
        return source_path_;
    }

  private:
    std::vector<StreamingLayerEntry> entries_;
    LayerIndexStats stats_;
    std::string source_path_;
    bool uses_layer_markers_{false};
};

} // namespace gcode
} // namespace helix
