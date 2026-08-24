// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// G-Code Geometry Builder Implementation

#include "gcode_geometry_builder.h"

#include "color_utils.h"
#include "config.h"
#include "geometry_budget_manager.h"

#include <spdlog/spdlog.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <algorithm>
#include <chrono>
#include <cmath>
#include <glm/gtx/norm.hpp>
#include <limits>
#include <unordered_map>

namespace helix {
namespace gcode {

// ============================================================================
// PackedVertex Encoding Helpers
// ============================================================================

void PackedVertex::encode_normal(const glm::vec3& n_in, int8_t out[2]) {
    glm::vec3 n = n_in;
    float len2 = glm::dot(n, n);
    if (len2 > 0.0f) {
        n *= 1.0f / std::sqrt(len2);
    } else {
        n = glm::vec3(0.0f, 0.0f, 1.0f);
    }
    float sum = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    if (sum > 0.0f) {
        n *= 1.0f / sum;
    }
    glm::vec2 oct{n.x, n.y};
    if (n.z < 0.0f) {
        oct.x = (1.0f - std::abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
        oct.y = (1.0f - std::abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
    }
    auto quantize = [](float v) -> int8_t {
        v = std::max(-1.0f, std::min(1.0f, v));
        return static_cast<int8_t>(std::lround(v * 127.0f));
    };
    out[0] = quantize(oct.x);
    out[1] = quantize(oct.y);
}

void PackedVertex::encode_color(uint32_t rgb, uint8_t out[4]) {
    out[0] = static_cast<uint8_t>((rgb >> 16) & 0xFF);
    out[1] = static_cast<uint8_t>((rgb >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>(rgb & 0xFF);
    out[3] = 255;
}

// ============================================================================
// Debug Face Colors
// ============================================================================

namespace DebugColors {
constexpr uint32_t TOP = 0xFF0000;       // Bright Red
constexpr uint32_t BOTTOM = 0x0000FF;    // Bright Blue
constexpr uint32_t LEFT = 0x00FF00;      // Bright Green
constexpr uint32_t RIGHT = 0xFFFF00;     // Bright Yellow
constexpr uint32_t START_CAP = 0xFF00FF; // Bright Magenta
constexpr uint32_t END_CAP = 0x00FFFF;   // Bright Cyan
} // namespace DebugColors

// ============================================================================
// QuantizationParams Implementation
// ============================================================================

void QuantizationParams::calculate_scale(const AABB& bbox) {
    min_bounds = bbox.min;
    max_bounds = bbox.max;

    // Calculate maximum dimension to determine scale factor
    glm::vec3 extents = max_bounds - min_bounds;
    float max_extent = std::max({extents.x, extents.y, extents.z});

    // 16-bit signed int range: -32768 to +32767
    // Quantization formula: (value - min_bound) * scale
    // Maximum quantized value = (max_bound - min_bound) * scale = extent * scale
    // Constraint: extent * scale <= 32767
    // Reserve 10% headroom to avoid edge cases
    constexpr float INT16_MAX_WITH_HEADROOM = 32767.0f * 0.9f;

    if (max_extent > 0.0f) {
        scale_factor = INT16_MAX_WITH_HEADROOM / max_extent;
    } else {
        // Fallback for degenerate bounding box
        scale_factor = 1000.0f; // 1 unit = 1mm
    }

    spdlog::debug(
        "[GCode Geometry] Quantization: bounds=[{:.2f},{:.2f},{:.2f}] to [{:.2f},{:.2f},{:.2f}], "
        "scale={:.2f} units/mm, resolution={:.4f}mm",
        min_bounds.x, min_bounds.y, min_bounds.z, max_bounds.x, max_bounds.y, max_bounds.z,
        scale_factor, 1.0f / scale_factor);
}

int16_t QuantizationParams::quantize(float value, float min_bound) const {
    float normalized = (value - min_bound) * scale_factor;

    // Clamp to int16 range to prevent overflow
    normalized = std::max(-32768.0f, std::min(32767.0f, normalized));

    return static_cast<int16_t>(std::round(normalized));
}

float QuantizationParams::dequantize(int16_t value, float min_bound) const {
    // Use double precision intermediates to avoid float rounding accumulation
    // when scale_factor and min_bound differ by several orders of magnitude.
    return static_cast<float>(static_cast<double>(value) / static_cast<double>(scale_factor) +
                              static_cast<double>(min_bound));
}

QuantizedVertex QuantizationParams::quantize_vec3(const glm::vec3& v) const {
    return QuantizedVertex{quantize(v.x, min_bounds.x), quantize(v.y, min_bounds.y),
                           quantize(v.z, min_bounds.z)};
}

glm::vec3 QuantizationParams::dequantize_vec3(const QuantizedVertex& qv) const {
    return glm::vec3(dequantize(qv.x, min_bounds.x), dequantize(qv.y, min_bounds.y),
                     dequantize(qv.z, min_bounds.z));
}

// ============================================================================
// RibbonGeometry Implementation
// ============================================================================

RibbonGeometry::RibbonGeometry()
    : color_cache(std::make_unique<ColorCache>()), extrusion_triangle_count(0),
      travel_triangle_count(0) {}

RibbonGeometry::~RibbonGeometry() = default;

RibbonGeometry::RibbonGeometry(RibbonGeometry&& other) noexcept
    : vertices(std::move(other.vertices)), indices(std::move(other.indices)),
      strips(std::move(other.strips)), strip_color_index(std::move(other.strip_color_index)),
      color_palette(std::move(other.color_palette)),
      tool_palette_map(std::move(other.tool_palette_map)),
      strip_layer_index(std::move(other.strip_layer_index)),
      layer_strip_ranges(std::move(other.layer_strip_ranges)),
      max_layer_index(other.max_layer_index), layer_bboxes(std::move(other.layer_bboxes)),
      color_cache(std::move(other.color_cache)),
      prepared_buffers(std::move(other.prepared_buffers)),
      extrusion_triangle_count(other.extrusion_triangle_count),
      travel_triangle_count(other.travel_triangle_count), quantization(other.quantization),
      layer_height_mm(other.layer_height_mm) {}

RibbonGeometry& RibbonGeometry::operator=(RibbonGeometry&& other) noexcept {
    if (this != &other) {
        // Move data (unique_ptr handles cleanup automatically)
        vertices = std::move(other.vertices);
        indices = std::move(other.indices);
        strips = std::move(other.strips);
        strip_color_index = std::move(other.strip_color_index);
        color_palette = std::move(other.color_palette);
        tool_palette_map = std::move(other.tool_palette_map);
        strip_layer_index = std::move(other.strip_layer_index);
        layer_strip_ranges = std::move(other.layer_strip_ranges);
        layer_bboxes = std::move(other.layer_bboxes);
        max_layer_index = other.max_layer_index;
        color_cache = std::move(other.color_cache);
        prepared_buffers = std::move(other.prepared_buffers);
        extrusion_triangle_count = other.extrusion_triangle_count;
        travel_triangle_count = other.travel_triangle_count;
        quantization = other.quantization;
        layer_height_mm = other.layer_height_mm;
    }
    return *this;
}

void RibbonGeometry::expand_strips(size_t first_strip, size_t strip_count,
                                   PackedVertex* out) const {
    // Strip order: BL(0), BR(1), TL(2), TR(3)
    // Triangle 1: BL-BR-TL,  Triangle 2: BR-TR-TL
    static constexpr int TRI_INDICES[6] = {0, 1, 2, 1, 3, 2};

    for (size_t s = 0; s < strip_count; ++s) {
        const size_t strip_idx = first_strip + s;
        const auto& strip = strips[strip_idx];

        // Color is per-strip (a strip is exactly one face), so resolve it once
        // per strip rather than once per expanded vertex.
        uint8_t rgba[4];
        PackedVertex::encode_color(strip_color(strip_idx), rgba);

        for (int ti = 0; ti < 6; ++ti) {
            const auto& vert = vertices[strip[static_cast<size_t>(TRI_INDICES[ti])]];

            // Quantized coordinates go to the GPU as-is; the renderer folds the
            // dequantization into its matrices.
            out->position[0] = vert.position.x;
            out->position[1] = vert.position.y;
            out->position[2] = vert.position.z;

            out->color[0] = rgba[0];
            out->color[1] = rgba[1];
            out->color[2] = rgba[2];
            out->color[3] = rgba[3];

            // Already octahedral-encoded at build time — straight copy.
            out->normal[0] = vert.normal[0];
            out->normal[1] = vert.normal[1];
            ++out;
        }
    }
}

void RibbonGeometry::prepare_interleaved_buffers() {
    if (strips.empty() || vertices.empty()) {
        return;
    }

    size_t num_layers = layer_strip_ranges.empty() ? 1 : layer_strip_ranges.size();
    prepared_buffers.resize(num_layers);

    constexpr size_t STRIDE = PackedVertex::stride();

    for (size_t layer = 0; layer < num_layers; ++layer) {
        size_t first_strip = 0;
        size_t strip_count = strips.size();

        if (!layer_strip_ranges.empty()) {
            auto [fs, sc] = layer_strip_ranges[layer];
            first_strip = fs;
            strip_count = sc;
        }

        auto& prepared = prepared_buffers[layer];
        if (strip_count == 0) {
            prepared.vertex_count = 0;
            continue;
        }

        size_t total_verts = strip_count * 6; // 2 triangles per strip
        prepared.vertex_count = total_verts;
        prepared.data.resize(total_verts * STRIDE);

        expand_strips(first_strip, strip_count,
                      reinterpret_cast<PackedVertex*>(prepared.data.data()));
    }

    spdlog::debug("[GCode Geometry] Prepared {} layer buffers for GPU upload", num_layers);
}

void RibbonGeometry::patch_prepared_buffer_colors() {
    if (prepared_buffers.empty() || strips.empty() || vertices.empty()) {
        return;
    }

    size_t num_layers = layer_strip_ranges.empty() ? 1 : layer_strip_ranges.size();
    if (num_layers != prepared_buffers.size()) {
        // Sizing drift — rebuild from scratch rather than risk a bad patch.
        prepare_interleaved_buffers();
        return;
    }

    // Same traversal order as expand_strips(), but rewrites only the color bytes
    // in place rather than re-emitting whole vertices — the point is to avoid
    // regenerating megabytes of buffer for a recolor. Any change to the vertex
    // ordering in expand_strips() must be mirrored here. Color is per-strip, so
    // the within-strip vertex permutation is irrelevant here: all 6 expanded
    // vertices of a strip get the same bytes.

    for (size_t layer = 0; layer < num_layers; ++layer) {
        size_t first_strip = 0;
        size_t strip_count = strips.size();
        if (!layer_strip_ranges.empty()) {
            auto [fs, sc] = layer_strip_ranges[layer];
            first_strip = fs;
            strip_count = sc;
        }

        auto& prepared = prepared_buffers[layer];
        if (prepared.vertex_count != strip_count * 6) {
            // Sizing drift on this layer — rebuild everything to be safe.
            prepare_interleaved_buffers();
            return;
        }

        auto* out = reinterpret_cast<PackedVertex*>(prepared.data.data());
        for (size_t s = 0; s < strip_count; ++s) {
            uint8_t rgba[4];
            PackedVertex::encode_color(strip_color(first_strip + s), rgba);
            for (int ti = 0; ti < 6; ++ti) {
                out->color[0] = rgba[0];
                out->color[1] = rgba[1];
                out->color[2] = rgba[2];
                out->color[3] = rgba[3];
                ++out;
            }
        }
    }

    spdlog::debug("[GCode Geometry] Patched colors in {} prepared layer buffers", num_layers);
}

void RibbonGeometry::clear() {
    // shrink_to_fit on every buffer — clear() alone keeps the (multi-MB) allocations alive.
    vertices.clear();
    vertices.shrink_to_fit();
    indices.clear();
    indices.shrink_to_fit();
    strips.clear();
    strips.shrink_to_fit();
    strip_color_index.clear();
    strip_color_index.shrink_to_fit();
    color_palette.clear();
    color_palette.shrink_to_fit();
    tool_palette_map.clear();
    strip_layer_index.clear();
    strip_layer_index.shrink_to_fit();
    layer_strip_ranges.clear();
    layer_strip_ranges.shrink_to_fit();
    layer_bboxes.clear();
    layer_bboxes.shrink_to_fit();
    prepared_buffers.clear();
    prepared_buffers.shrink_to_fit();
    max_layer_index = 0;

    // Clear caches
    if (color_cache) {
        color_cache->clear();
    }

    extrusion_triangle_count = 0;
    travel_triangle_count = 0;
}

// ============================================================================
// RibbonGeometry Validation
// ============================================================================

void RibbonGeometry::validate() const {
    size_t issues = 0;

    // Spot-check vertex positions for NaN/Inf (check every 100th vertex, plus first and last)
    if (!vertices.empty()) {
        auto check_vertex = [&](size_t idx) {
            const auto& v = vertices[idx];
            glm::vec3 pos = quantization.dequantize_vec3(v.position);
            if (std::isnan(pos.x) || std::isnan(pos.y) || std::isnan(pos.z) || std::isinf(pos.x) ||
                std::isinf(pos.y) || std::isinf(pos.z)) {
                spdlog::warn("[GCode::Builder] Vertex {} has NaN/Inf position: ({}, {}, {})", idx,
                             pos.x, pos.y, pos.z);
                ++issues;
            }
        };

        check_vertex(0);
        check_vertex(vertices.size() - 1);
        for (size_t i = 100; i < vertices.size(); i += 100) {
            check_vertex(i);
        }
    }

    // Validate layer strip ranges are within bounds
    for (size_t layer = 0; layer < layer_strip_ranges.size(); ++layer) {
        auto [first, count] = layer_strip_ranges[layer];
        if (count == 0)
            continue;
        if (first + count > strips.size()) {
            spdlog::warn("[GCode::Builder] Layer {} strip range [{}, +{}) exceeds strip count {}",
                         layer, first, count, strips.size());
            ++issues;
        }
    }

    // strip_color_index is parallel to strips — a length mismatch means some
    // strip push_back lost its matching color push_back and every strip past
    // that point renders the wrong color.
    if (strip_color_index.size() != strips.size()) {
        spdlog::warn("[GCode::Builder] strip_color_index size {} != strips size {}",
                     strip_color_index.size(), strips.size());
        ++issues;
    }

    // Validate per-strip color palette indices (spot-check)
    size_t color_palette_size = color_palette.size();
    for (size_t i = 0; i < strip_color_index.size();
         i += std::max(size_t(1), strip_color_index.size() / 200)) {
        if (strip_color_index[i] >= color_palette_size) {
            spdlog::warn("[GCode::Builder] Strip {} color_index {} >= palette size {}", i,
                         strip_color_index[i], color_palette_size);
            ++issues;
        }
    }

    // Validate strip vertex indices are within bounds (spot-check)
    for (size_t i = 0; i < strips.size(); i += std::max(size_t(1), strips.size() / 200)) {
        for (uint32_t idx : strips[i]) {
            if (idx >= vertices.size()) {
                spdlog::warn("[GCode::Builder] Strip {} references vertex {} >= vertex count {}", i,
                             idx, vertices.size());
                ++issues;
                break;
            }
        }
    }

    if (issues > 0) {
        spdlog::warn("[GCode::Builder] Geometry validation found {} issue(s)", issues);
    } else {
        spdlog::debug("[GCode::Builder] Geometry validation passed ({} vertices, {} strips, "
                      "{} layers)",
                      vertices.size(), strips.size(), layer_strip_ranges.size());
    }
}

// ============================================================================
// BuildStats Implementation
// ============================================================================

void GeometryBuilder::BuildStats::log() const {
    spdlog::info("[GCode::Builder] ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    spdlog::info("[GCode::Builder] Geometry Build Statistics:");
    spdlog::info("[GCode::Builder]   G-code Parsing:");
    spdlog::info("[GCode::Builder]     Raw toolpath segments:    {:>8}", input_segments);
    spdlog::info("[GCode::Builder]     After simplification:     {:>8} ({:.1f}% reduction)",
                 output_segments, simplification_ratio * 100.0f);
    spdlog::info("[GCode::Builder]   3D Geometry Generation:");
    spdlog::info("[GCode::Builder]     Vertices (triangle strips): {:>8}", vertices_generated);
    spdlog::info("[GCode::Builder]     Triangles rendered:         {:>8}", triangles_generated);
    spdlog::info("[GCode::Builder]   Memory:");
    spdlog::info("[GCode::Builder]     Total geometry memory:    {:>8} KB ({:.2f} MB)",
                 memory_bytes / 1024, memory_bytes / (1024.0 * 1024.0));

    if (input_segments > 0) {
        float bytes_per_segment = static_cast<float>(memory_bytes) / input_segments;
        spdlog::info("[GCode::Builder]     Bytes per toolpath segment: {:.1f}", bytes_per_segment);
    }
    spdlog::info("[GCode::Builder] ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
}

// ============================================================================
// GeometryBuilder Implementation
// ============================================================================

GeometryBuilder::GeometryBuilder() {
    stats_ = {};

    auto* config = Config::get_instance();
    if (config) {
        tube_sides_ = config->get<int>("/gcode_viewer/tube_sides", 16);
        if (tube_sides_ != 4 && tube_sides_ != 8 && tube_sides_ != 16) {
            spdlog::warn(
                "[GCode Geometry] Invalid tube_sides={} (must be 4, 8, or 16), defaulting to 16",
                tube_sides_);
            tube_sides_ = 16;
        }
        spdlog::info("[GCode Geometry] G-code tube geometry: N={} sides (elliptical cross-section)",
                     tube_sides_);
    }
}

// ============================================================================
// Palette Management
// ============================================================================

uint8_t GeometryBuilder::add_to_color_palette(RibbonGeometry& geometry, uint32_t color_rgb) {
    // Check cache first (O(1) lookup)
    auto it = geometry.color_cache->find(color_rgb);
    if (it != geometry.color_cache->end()) {
        return it->second; // Cache hit!
    }

    // Not in cache - add to palette
    if (geometry.color_palette.size() >= 256) {
        static bool color_warned = false;
        if (!color_warned) {
            spdlog::warn("[GCode Geometry] Color palette full (256 entries), reusing last entry");
            color_warned = true;
        }
        return 255;
    }

    uint8_t index = static_cast<uint8_t>(geometry.color_palette.size());
    geometry.color_palette.push_back(color_rgb);
    (*geometry.color_cache)[color_rgb] = index; // Add to cache

    return index;
}

RibbonGeometry GeometryBuilder::build(const ParsedGCodeFile& gcode,
                                      const SimplificationOptions& options) {
    current_gcode_ = &gcode;

    // Start timing
    auto build_start = std::chrono::high_resolution_clock::now();

    RibbonGeometry geometry;
    stats_ = {}; // Reset statistics
    budget_exceeded_ = false;

    // Apply budget tube_sides override if set
    if (budget_tube_sides_ > 0) {
        tube_sides_ = budget_tube_sides_;
        spdlog::info("[GCode::Builder] Budget override: tube_sides={}", tube_sides_);
    }

    // Validate and apply options
    SimplificationOptions validated_opts = options;
    validated_opts.validate();

    spdlog::info("[GCode::Builder] Config: layer_height={:.3f}mm, extrusion_width={:.3f}mm, "
                 "tube_sides={}, tolerance={:.3f}mm",
                 layer_height_mm_, extrusion_width_mm_, tube_sides_, validated_opts.tolerance_mm);

    // Calculate quantization parameters from bounding box
    // IMPORTANT: Expand bounds to account for tube width (vertices extend beyond segment positions)
    // Use sqrt(2) safety factor because rectangular tubes on diagonal segments can expand
    // in multiple dimensions simultaneously (e.g., perp_horizontal + perp_vertical)
    float max_tube_width = std::max(extrusion_width_mm_, travel_width_mm_);
    float expansion_margin = max_tube_width * 1.5f; // Safety factor for diagonal expansion
    AABB expanded_bbox = gcode.global_bounding_box;
    expanded_bbox.min -= glm::vec3(expansion_margin, expansion_margin, expansion_margin);
    expanded_bbox.max += glm::vec3(expansion_margin, expansion_margin, expansion_margin);
    quant_params_.calculate_scale(expanded_bbox);

    spdlog::debug(
        "[GCode Geometry] Expanded quantization bounds by {:.1f}mm for tube width {:.1f}mm",
        expansion_margin, max_tube_width);

    // Collect all segments from all layers, stamping each with its source layer index
    std::vector<ToolpathSegment> all_segments;
    all_segments.reserve(gcode.total_segments);
    for (size_t li = 0; li < gcode.layers.size(); ++li) {
        for (const auto& seg : gcode.layers[li].segments) {
            all_segments.push_back(seg);
            all_segments.back().layer_index = static_cast<uint16_t>(li);
        }
    }

    stats_.input_segments = all_segments.size();
    spdlog::debug("[GCode::Builder] Collected {} total segments from {} layers",
                  all_segments.size(), gcode.layers.size());

    // Pre-filter: Remove degenerate (zero-length) segments before simplification
    size_t degenerate_count = 0;
    all_segments.erase(std::remove_if(all_segments.begin(), all_segments.end(),
                                      [&degenerate_count](const ToolpathSegment& seg) {
                                          float length = glm::distance(seg.start, seg.end);
                                          if (length < 0.0001f) {
                                              degenerate_count++;
                                              return true; // Remove this segment
                                          }
                                          return false; // Keep this segment
                                      }),
                       all_segments.end());

    if (degenerate_count > 0) {
        spdlog::debug("[GCode::Builder] Pre-filtered {} degenerate (zero-length) segments",
                      degenerate_count);
    }

    // Step 1: Simplify segments (merge collinear lines)
    std::vector<ToolpathSegment> simplified;
    if (validated_opts.enable_merging) {
        simplified = simplify_segments(all_segments, validated_opts);

        // all_segments is dead from here on — release it before generating geometry so the
        // raw and simplified copies never coexist with the vertex buffer.
        const size_t raw_segment_count = all_segments.size();
        all_segments.clear();
        all_segments.shrink_to_fit();

        stats_.output_segments = simplified.size();
        stats_.simplification_ratio =
            1.0f - (static_cast<float>(simplified.size()) / raw_segment_count);

        spdlog::info(
            "[GCode::Builder] Toolpath simplification: {} → {} segments ({:.1f}% reduction)",
            raw_segment_count, simplified.size(), stats_.simplification_ratio * 100.0f);
    } else {
        simplified = std::move(all_segments);
        stats_.output_segments = simplified.size();
        stats_.simplification_ratio = 0.0f;
        spdlog::info("[GCode::Builder] Toolpath simplification DISABLED: using {} raw segments",
                     simplified.size());
    }

    // Step 2: Generate ribbon geometry with vertex sharing
    // Track previous segment end vertices for reuse
    std::optional<TubeCap> prev_end_cap;
    glm::vec3 prev_end_pos{0.0f};

    // Layer tracking for ghost layer rendering. Only the first index, the last index and the
    // count are ever needed to derive the per-layer strip range, so accumulate those three
    // scalars per layer instead of retaining every strip index.
    const size_t layer_count = gcode.layers.size();
    std::vector<size_t> layer_first_strip(layer_count, 0);
    std::vector<size_t> layer_last_strip(layer_count, 0);
    std::vector<size_t> layer_strip_totals(layer_count, 0);

    geometry.max_layer_index =
        gcode.layers.empty() ? 0 : static_cast<uint16_t>(gcode.layers.size() - 1);

    // Initialize per-layer bounding boxes for frustum culling
    geometry.layer_bboxes.resize(gcode.layers.size());

    // Pre-size the output buffers: every extrusion segment emits exactly 5N vertices and
    // (2N-2) strips (the extra start-cap vertices/strips on the very first segment are
    // absorbed by the steady-state figure). Travel moves are skipped entirely.
    //
    // Only pre-size when the projection fits the budget. A build that is going to
    // blow the budget must be allowed to grow incrementally so the progressive
    // check below aborts it after a few MB — reserving the full projection first
    // would hand the allocator the whole request up front and OOM the device
    // instead of falling back to the 2D renderer.
    {
        const size_t extrusion_segments = static_cast<size_t>(
            std::count_if(simplified.begin(), simplified.end(),
                          [](const ToolpathSegment& s) { return s.is_extrusion; }));
        const size_t n = static_cast<size_t>(tube_sides_);
        const size_t vertex_count = extrusion_segments * 5 * n;
        const size_t strip_count = extrusion_segments * (2 * n - 2);
        const size_t projected_bytes = vertex_count * sizeof(RibbonVertex) +
                                       strip_count * sizeof(decltype(geometry.strips)::value_type) +
                                       strip_count * sizeof(uint16_t) + // strip_layer_index
                                       strip_count * sizeof(uint8_t);   // strip_color_index

        if (budget_limit_bytes_ == 0 || projected_bytes <= budget_limit_bytes_) {
            geometry.vertices.reserve(vertex_count);
            geometry.strips.reserve(strip_count);
            geometry.strip_layer_index.reserve(strip_count);
            geometry.strip_color_index.reserve(strip_count);
        } else {
            spdlog::debug("[GCode::Builder] Skipping pre-size: projected {}MB exceeds {}MB budget",
                          projected_bytes / (1024 * 1024), budget_limit_bytes_ / (1024 * 1024));
        }
    }

    size_t segments_since_budget_check = 0;

    for (size_t i = 0; i < simplified.size(); ++i) {
        const auto& segment = simplified[i];

        // Note: Degenerate segments were already filtered before simplification

        // Skip travel moves (non-extrusion moves)
        // TODO: Make this configurable if we want to visualize travel paths
        if (!segment.is_extrusion) {
            continue;
        }

        // Progressive budget check
        if (budget_limit_bytes_ > 0) {
            segments_since_budget_check++;
            if (segments_since_budget_check >= GeometryBudgetManager::CHECK_INTERVAL_SEGMENTS) {
                segments_since_budget_check = 0;
                size_t current_mem = geometry.memory_usage();
                float threshold = static_cast<float>(budget_limit_bytes_) *
                                  GeometryBudgetManager::BUDGET_THRESHOLD;
                if (static_cast<float>(current_mem) > threshold) {
                    spdlog::warn("[GCode::Builder] Budget exceeded: {}MB / {}MB at segment {}/{}",
                                 current_mem / (1024 * 1024), budget_limit_bytes_ / (1024 * 1024),
                                 i, simplified.size());
                    budget_exceeded_ = true;
                    break;
                }

                // System memory check (less frequent, only at CHECK_INTERVAL boundaries)
                if (i > 0 && i % GeometryBudgetManager::SYSTEM_CHECK_INTERVAL_SEGMENTS == 0) {
                    GeometryBudgetManager budget_mgr;
                    if (budget_mgr.is_system_memory_critical()) {
                        spdlog::error("[GCode::Builder] System memory critical — aborting build");
                        budget_exceeded_ = true;
                        break;
                    }
                }
            }
        }

        // Use source layer index stamped during segment collection
        uint16_t layer_idx = segment.layer_index;

        // Expand per-layer bounding box for frustum culling.
        // Include tube width: geometry extends perpendicular to the segment direction,
        // so expand by max(extrusion_width, segment.width) * 0.5 * sqrt(2).
        if (layer_idx < geometry.layer_bboxes.size()) {
            AABB& layer_bbox = geometry.layer_bboxes[layer_idx];
            float tube_width = std::max(extrusion_width_mm_, segment.width);
            float expansion = tube_width * 0.5f * 1.41421356f; // sqrt(2) for diagonal
            glm::vec3 expand_vec(expansion, expansion, expansion);
            layer_bbox.expand(segment.start - expand_vec);
            layer_bbox.expand(segment.start + expand_vec);
            layer_bbox.expand(segment.end - expand_vec);
            layer_bbox.expand(segment.end + expand_vec);
        }

        // Check if we can share vertices with previous segment
        bool can_share = false;
        if (prev_end_cap.has_value()) {
            // Segments must connect spatially (within epsilon) and be same type
            float dist = glm::distance(segment.start, prev_end_pos);
            float connection_tolerance = segment.width * 0.5f;
            can_share = (dist < connection_tolerance) &&
                        (segment.is_extrusion == simplified[i - 1].is_extrusion);
        }

        // Track strip count before generating geometry
        size_t strips_before = geometry.strips.size();

        // Generate geometry, reusing previous end cap if segments connect
        TubeCap end_cap = generate_ribbon_vertices(segment, geometry, quant_params_,
                                                   can_share ? prev_end_cap : std::nullopt);

        // Track which strips belong to which layer
        size_t strips_after = geometry.strips.size();
        for (size_t s = strips_before; s < strips_after; ++s) {
            geometry.strip_layer_index.push_back(layer_idx);
            if (layer_idx < layer_count) {
                if (layer_strip_totals[layer_idx] == 0) {
                    layer_first_strip[layer_idx] = s;
                }
                layer_last_strip[layer_idx] = s;
                layer_strip_totals[layer_idx]++;
            }
        }

        // Store for next iteration
        prev_end_cap = end_cap;
        prev_end_pos = segment.end;
    }

    // Release the over-reserve: an early budget abort leaves the reserved tail unused.
    geometry.vertices.shrink_to_fit();
    geometry.strips.shrink_to_fit();
    geometry.strip_layer_index.shrink_to_fit();
    geometry.strip_color_index.shrink_to_fit();

    // The palette is final; the lookup cache is build-only scratch. Drop it entirely —
    // ->clear() would keep the bucket arrays alive for the lifetime of the geometry.
    geometry.color_cache.reset();

    // Build layer_strip_ranges from accumulated data
    // Initialize with empty ranges for all layers
    geometry.layer_strip_ranges.resize(gcode.layers.size(), {0, 0});
    size_t non_contiguous_layers = 0;
    for (size_t layer_idx = 0; layer_idx < layer_count; ++layer_idx) {
        size_t strip_total = layer_strip_totals[layer_idx];
        if (strip_total > 0) {
            size_t first = layer_first_strip[layer_idx];
            size_t last = layer_last_strip[layer_idx];
            size_t span = last - first + 1;

            // Contiguity check: if the span exceeds the index count, there are gaps
            if (span != strip_total) {
                non_contiguous_layers++;
                spdlog::trace("[GCode::Builder] Layer {} strips are non-contiguous: "
                              "{} indices spanning {} slots (gaps present)",
                              layer_idx, strip_total, span);
            }

            // Use the full span range (first, span) to cover all strips including gaps.
            // This may include strips from other layers in the gap, but the renderer
            // draws by layer range so this is safe for single-VBO-per-layer upload.
            geometry.layer_strip_ranges[layer_idx] = {first, span};
        }
    }
    if (non_contiguous_layers > 0) {
        spdlog::warn("[GCode::Builder] {} layers have non-contiguous strip ranges "
                     "(using span-based ranges as fallback)",
                     non_contiguous_layers);
    }

    // Store quantization parameters for dequantization during rendering
    geometry.quantization = quant_params_;

    // Store layer height for Z-offset calculations during LOD rendering
    geometry.layer_height_mm = layer_height_mm_;

    // Update final statistics
    stats_.vertices_generated = geometry.vertices.size();
    // Each TriangleStrip has 4 indices forming 2 triangles
    stats_.triangles_generated = geometry.strips.size() * 2;
    stats_.memory_bytes = geometry.memory_usage();

    stats_.log();

    // Validate geometry integrity before returning
    geometry.validate();

    // End timing
    auto build_end = std::chrono::high_resolution_clock::now();
    auto build_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(build_end - build_start);
    spdlog::info("[GCode::Builder] Geometry build completed in {:.3f} seconds",
                 build_duration.count() / 1000.0);

    return geometry;
}

// ============================================================================
// Segment Simplification
// ============================================================================

std::vector<ToolpathSegment>
GeometryBuilder::simplify_segments(const std::vector<ToolpathSegment>& segments,
                                   const SimplificationOptions& options) {
    if (segments.empty()) {
        return {};
    }

    std::vector<ToolpathSegment> simplified;
    simplified.reserve(segments.size()); // Upper bound

    // Start with first segment
    ToolpathSegment current = segments[0];

    for (size_t i = 1; i < segments.size(); ++i) {
        const auto& next = segments[i];

        // Can only merge segments if:
        // 1. Same move type (both extrusion or both travel)
        // 2. Same layer (segments in different layers must NEVER be merged)
        // 3. Endpoints connect (current.end ≈ next.start)
        // 4. Same object (for per-object highlighting)
        // 5. Collinear within tolerance

        bool same_type = (current.is_extrusion == next.is_extrusion);
        bool same_layer = (current.layer_index == next.layer_index);
        bool endpoints_connect = glm::distance2(current.end, next.start) < 0.0001f;
        bool same_object = (current.object_name_index == next.object_name_index);
        bool same_width = (std::abs(current.width - next.width) < 0.001f);

        if (same_type && same_layer && endpoints_connect && same_object && same_width) {
            // Direction check: prevent merging segments with significantly different directions.
            // This preserves zigzag fill patterns where perpendicular distance is small but
            // the direction changes sharply (e.g., 90-degree turns in solid infill).
            glm::vec3 merged_dir = next.end - current.start;
            glm::vec3 candidate_dir = next.end - next.start;
            float merged_len2 = glm::length2(merged_dir);
            float candidate_len2 = glm::length2(candidate_dir);

            bool direction_ok = true;
            if (merged_len2 > 1e-8f && candidate_len2 > 1e-8f) {
                glm::vec3 d1 = merged_dir / std::sqrt(merged_len2);
                glm::vec3 d2 = candidate_dir / std::sqrt(candidate_len2);
                float dot = glm::dot(d1, d2);
                dot = std::max(-1.0f, std::min(1.0f, dot)); // Clamp for acos safety
                float angle_deg = glm::degrees(std::acos(dot));
                direction_ok = (angle_deg <= options.max_direction_change_deg);
            }

            if (direction_ok) {
                // Check if current.start, current.end, next.end are collinear
                bool collinear =
                    are_collinear(current.start, current.end, next.end, options.tolerance_mm);

                if (collinear) {
                    // Merge: extend current segment to end at next.end
                    current.end = next.end;
                    current.extrusion_amount += next.extrusion_amount;
                    continue; // Skip adding next to simplified list
                }
            }
        }

        // Cannot merge - save current and start new segment
        simplified.push_back(current);
        current = next;
    }

    // Add final segment
    simplified.push_back(current);

    // The reserve above is a worst-case upper bound; give back the unused tail.
    simplified.shrink_to_fit();

    return simplified;
}

bool GeometryBuilder::are_collinear(const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3,
                                    float tolerance) const {
    // Vector from p1 to p2
    glm::vec3 v1 = p2 - p1;

    // Vector from p1 to p3
    glm::vec3 v2 = p3 - p1;

    // If either vector is nearly zero-length, points are effectively same point
    float len1_sq = glm::length2(v1);
    float len2_sq = glm::length2(v2);

    if (len1_sq < 1e-8f || len2_sq < 1e-8f) {
        return true; // Degenerate case - treat as collinear
    }

    // Cross product gives vector perpendicular to both v1 and v2
    // If v1 and v2 are collinear, cross product magnitude will be zero
    glm::vec3 cross = glm::cross(v1, v2);
    float cross_mag = glm::length(cross);

    // Distance from p3 to line defined by p1-p2 is:
    // distance = |cross(v1, v2)| / |v1|
    float distance = cross_mag / std::sqrt(len1_sq);

    return distance <= tolerance;
}

// ============================================================================
// Ribbon Geometry Generation
// ============================================================================

GeometryBuilder::TubeCap
GeometryBuilder::generate_ribbon_vertices(const ToolpathSegment& segment, RibbonGeometry& geometry,
                                          const QuantizationParams& quant,
                                          std::optional<TubeCap> prev_start_cap) {
    const int N = tube_sides_;

    // Determine tube dimensions
    float width;
    if (segment.is_extrusion && segment.width >= 0.1f && segment.width <= 2.0f) {
        width = segment.width;
    } else {
        width = segment.is_extrusion ? extrusion_width_mm_ : travel_width_mm_;
    }
    width = width * 1.1f; // 10% safety margin

    const float half_width = width * 0.5f;
    const float half_height = layer_height_mm_ * 0.5f;

    // Calculate direction and perpendicular vectors
    const glm::vec3 dir = glm::normalize(segment.end - segment.start);
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    glm::vec3 right = glm::cross(dir, up);

    if (glm::length2(right) < 1e-6f) {
        right = glm::vec3(1.0f, 0.0f, 0.0f);
    } else {
        right = glm::normalize(right);
    }

    // OrcaSlicer: up = right.cross(dir), NOT cross(dir, up)!
    const glm::vec3 perp_up = glm::normalize(glm::cross(right, dir));

    // Compute color
    uint32_t rgb = compute_segment_color(segment, quant.min_bounds.z, quant.max_bounds.z);
    static const std::string empty_obj_name;
    const std::string& seg_obj_name =
        current_gcode_ ? current_gcode_->get_object_name(segment.object_name_index)
                       : empty_obj_name;
    if (!highlighted_objects_.empty() && !seg_obj_name.empty() &&
        highlighted_objects_.count(seg_obj_name) > 0) {
        constexpr float HIGHLIGHT_BRIGHTNESS = 1.8f;
        uint8_t r =
            static_cast<uint8_t>(std::min(255.0f, ((rgb >> 16) & 0xFF) * HIGHLIGHT_BRIGHTNESS));
        uint8_t g =
            static_cast<uint8_t>(std::min(255.0f, ((rgb >> 8) & 0xFF) * HIGHLIGHT_BRIGHTNESS));
        uint8_t b = static_cast<uint8_t>(std::min(255.0f, (rgb & 0xFF) * HIGHLIGHT_BRIGHTNESS));
        rgb = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
              static_cast<uint32_t>(b);
    }

    uint8_t color_idx = add_to_color_palette(geometry, rgb);

    // Record tool → palette index mapping for per-tool recoloring (AMS overrides)
    if (segment.tool_index >= 0) {
        auto tool_idx = static_cast<uint8_t>(segment.tool_index);
        geometry.tool_palette_map[tool_idx] = color_idx;
    }

    // Face colors: one color per face (N faces total)
    std::vector<uint8_t> face_colors(static_cast<size_t>(N), color_idx);

    if (debug_face_colors_) {
        // Cycle through 4 debug colors for N faces
        constexpr uint32_t DEBUG_COLORS[] = {
            DebugColors::TOP,    // Red
            DebugColors::RIGHT,  // Yellow
            DebugColors::BOTTOM, // Blue
            DebugColors::LEFT    // Green
        };

        for (int i = 0; i < N; i++) {
            uint32_t color = DEBUG_COLORS[i % 4]; // Cycle: R,Y,B,G,R,Y,B,G,...
            face_colors[static_cast<size_t>(i)] = add_to_color_palette(geometry, color);
        }

        static bool logged_once = false;
        if (!logged_once) {
            spdlog::debug("[GCode Geometry] DEBUG FACE COLORS ACTIVE: N={} faces, colors cycle "
                          "through Red/Yellow/Blue/Green",
                          N);
            logged_once = true;
        }
    }

    // OrcaSlicer approach: Apply vertical offset to BOTH prev and curr positions
    // This makes the TOP edge sit at the path Z-coordinate
    const glm::vec3 prev_pos = segment.start - half_height * perp_up;
    const glm::vec3 curr_pos = segment.end - half_height * perp_up;

    // Generate N vertex offsets for tube cross-section
    std::vector<glm::vec3> vertex_offsets(static_cast<size_t>(N));

    if (N == 4) {
        // Rectangle cross-section: flat top/bottom/sides with full width coverage.
        // Adjacent extrusion lines tile seamlessly (no gaps between solid fill lines).
        // Order: top-right, top-left, bottom-left, bottom-right
        vertex_offsets[0] = +half_width * right + half_height * perp_up;
        vertex_offsets[1] = -half_width * right + half_height * perp_up;
        vertex_offsets[2] = -half_width * right - half_height * perp_up;
        vertex_offsets[3] = +half_width * right - half_height * perp_up;
    } else {
        // Higher N: elliptical cross-section via parametric angle
        const float angle_step = 2.0f * static_cast<float>(M_PI) / N;
        for (int i = 0; i < N; i++) {
            float angle = i * angle_step;
            vertex_offsets[static_cast<size_t>(i)] =
                half_width * std::cos(angle) * right + half_height * std::sin(angle) * perp_up;
        }
    }

    // Per-vertex normals derived from vertex offset direction (smooth shading)
    std::vector<glm::vec3> vertex_normals(static_cast<size_t>(N));
    for (int i = 0; i < N; i++) {
        float len = glm::length(vertex_offsets[static_cast<size_t>(i)]);
        if (len > 1e-6f) {
            vertex_normals[static_cast<size_t>(i)] = vertex_offsets[static_cast<size_t>(i)] / len;
        } else {
            vertex_normals[static_cast<size_t>(i)] = glm::vec3(0.0f, 0.0f, 1.0f);
        }
    }

    // Octahedral-encode the per-vertex normals once per segment. RibbonVertex stores
    // the GPU-ready encoding directly, so expansion never has to re-encode.
    std::vector<std::array<int8_t, 2>> vertex_normals_enc(static_cast<size_t>(N));
    for (int i = 0; i < N; i++) {
        PackedVertex::encode_normal(vertex_normals[static_cast<size_t>(i)],
                                    vertex_normals_enc[static_cast<size_t>(i)].data());
    }

    // Both caps face backward along the segment (-dir).
    std::array<int8_t, 2> cap_normal_enc{};
    PackedVertex::encode_normal(-dir, cap_normal_enc.data());

    // Phase 3: N-based vertex generation (replaces hardcoded N=4 logic)
    uint32_t idx_start = static_cast<uint32_t>(geometry.vertices.size());
    bool is_first_segment = !prev_start_cap.has_value();

    // Declared out here because the start-cap *strips* below need the same color.
    uint8_t start_cap_color_idx = 0;

    // ========== START CAP VERTICES (first segment only) ==========
    if (is_first_segment) {
        // Use unique START_CAP color for debug visualization
        start_cap_color_idx = debug_face_colors_
                                  ? add_to_color_palette(geometry, DebugColors::START_CAP)
                                  : face_colors[0]; // Use first face color if not debugging

        // Generate N start cap vertices
        for (int i = 0; i < N; i++) {
            glm::vec3 pos = prev_pos + vertex_offsets[static_cast<size_t>(i)];
            geometry.vertices.push_back({
                quant.quantize_vec3(pos),
                {cap_normal_enc[0], cap_normal_enc[1]} // Axial normal pointing backward
            });
        }
        idx_start += static_cast<uint32_t>(N);
    }

    // ========== PREV SIDE FACE VERTICES ==========
    // Generate 2N prev vertices (2 vertices per face, N faces)
    // Each face connects vertex (i+1)%N to vertex i (going backwards around circle for correct
    // winding). Per-vertex radial normals for smooth shading.
    for (int i = 0; i < N; i++) {
        int next_i = (i + 1) % N;
        glm::vec3 pos_v1 =
            prev_pos + vertex_offsets[static_cast<size_t>(next_i)]; // REVERSED: next_i first
        glm::vec3 pos_v2 = prev_pos + vertex_offsets[static_cast<size_t>(i)]; // then i
        const auto& n_v1 = vertex_normals_enc[static_cast<size_t>(next_i)];
        const auto& n_v2 = vertex_normals_enc[static_cast<size_t>(i)];

        geometry.vertices.push_back({quant.quantize_vec3(pos_v1), {n_v1[0], n_v1[1]}});
        geometry.vertices.push_back({quant.quantize_vec3(pos_v2), {n_v2[0], n_v2[1]}});
    }
    idx_start += static_cast<uint32_t>(2 * N);

    // ========== CURR SIDE FACE VERTICES ==========
    // Generate 2N curr vertices (2 vertices per face, N faces)
    // Each face connects vertex (i+1)%N to vertex i (going backwards around circle for correct
    // winding). Per-vertex radial normals for smooth shading.
    for (int i = 0; i < N; i++) {
        int next_i = (i + 1) % N;
        glm::vec3 pos_v1 =
            curr_pos + vertex_offsets[static_cast<size_t>(next_i)]; // REVERSED: next_i first
        glm::vec3 pos_v2 = curr_pos + vertex_offsets[static_cast<size_t>(i)]; // then i
        const auto& n_v1 = vertex_normals_enc[static_cast<size_t>(next_i)];
        const auto& n_v2 = vertex_normals_enc[static_cast<size_t>(i)];

        geometry.vertices.push_back({quant.quantize_vec3(pos_v1), {n_v1[0], n_v1[1]}});
        geometry.vertices.push_back({quant.quantize_vec3(pos_v2), {n_v2[0], n_v2[1]}});
    }
    idx_start += static_cast<uint32_t>(2 * N);

    // ========== END CAP TRACKING ==========
    // Track end cap edge positions (first vertex of each face in curr ring)
    TubeCap end_cap(static_cast<size_t>(N));
    uint32_t end_cap_base = idx_start - static_cast<uint32_t>(2 * N);
    for (int i = 0; i < N; i++) {
        // Track first vertex of each face (vertex i)
        end_cap[static_cast<size_t>(i)] = end_cap_base + static_cast<uint32_t>(2 * i);
    }

    // ========== TRIANGLE STRIPS GENERATION (Phase 4: N-based) ==========

    // Calculate vertex base indices
    uint32_t base, start_cap_base = 0, prev_faces_base, curr_faces_base;

    if (is_first_segment) {
        // First segment: N (start cap) + 2N (prev) + 2N (curr) = 5N vertices
        base = idx_start - static_cast<uint32_t>(5 * N);
        start_cap_base = base;
        prev_faces_base = base + static_cast<uint32_t>(N);
        curr_faces_base = base + static_cast<uint32_t>(N + 2 * N);
    } else {
        // Subsequent: 2N (prev) + 2N (curr) = 4N vertices
        base = idx_start - static_cast<uint32_t>(4 * N);
        prev_faces_base = base;
        curr_faces_base = base + static_cast<uint32_t>(2 * N);
    }

    // Generate N side face strips (one strip per face)
    // Each face connects vertex i to vertex (i+1)%N
    //
    // Every strips.push_back() below MUST be matched by a strip_color_index.push_back()
    // — the two vectors are parallel and expand_strips() indexes them together.
    for (int i = 0; i < N; i++) {
        geometry.strips.push_back({
            prev_faces_base + static_cast<uint32_t>(2 * i),     // prev ring, vertex i
            prev_faces_base + static_cast<uint32_t>(2 * i + 1), // prev ring, vertex i+1
            curr_faces_base + static_cast<uint32_t>(2 * i),     // curr ring, vertex i
            curr_faces_base + static_cast<uint32_t>(2 * i + 1)  // curr ring, vertex i+1
        });
        geometry.strip_color_index.push_back(face_colors[static_cast<size_t>(i)]);
    }

    // Start cap (first segment only) - Triangle fan encoded as 4-vertex strips
    if (is_first_segment) {
        // For N=4: Creates 2 triangles (N-2)
        // For N=8: Creates 6 triangles (N-2)
        // For N=16: Creates 14 triangles (N-2)
        // Triangle fan: v0 is center, connects to all edges
        for (int i = 1; i < N - 1; i++) {
            geometry.strips.push_back({
                start_cap_base,                                // v0 (fan center)
                start_cap_base + static_cast<uint32_t>(i),     // vi (current edge)
                start_cap_base + static_cast<uint32_t>(i + 1), // vi+1 (next edge)
                start_cap_base + static_cast<uint32_t>(i + 1)  // Duplicate (degenerate triangle)
            });
            geometry.strip_color_index.push_back(start_cap_color_idx);
        }
    }

    // ========== END CAP VERTICES ==========
    // Create N new vertices at the SAME POSITIONS as end_cap vertices but with axial normals
    uint8_t end_cap_color_idx =
        debug_face_colors_ ? add_to_color_palette(geometry, DebugColors::END_CAP) : face_colors[0];

    uint32_t idx_end_cap_start = idx_start;

    // Create N end cap vertices with axial normals
    for (int i = 0; i < N; i++) {
        uint32_t src_idx = end_cap[static_cast<size_t>(i)];
        if (src_idx >= geometry.vertices.size()) {
            spdlog::error("[GCode Geometry] End cap vertex index {} out of bounds (size={})",
                          src_idx, geometry.vertices.size());
            continue;
        }
        geometry.vertices.push_back(
            {geometry.vertices[src_idx].position, {cap_normal_enc[0], cap_normal_enc[1]}});
    }
    idx_start += static_cast<uint32_t>(N);

    // ========== END CAP STRIPS ==========
    // Triangle fan with REVERSED winding (CW instead of CCW) for opposite-facing cap
    for (int i = 1; i < N - 1; i++) {
        geometry.strips.push_back({
            idx_end_cap_start,                                    // v0 (fan center)
            idx_end_cap_start + static_cast<uint32_t>(N - i),     // vN-i (reverse order)
            idx_end_cap_start + static_cast<uint32_t>(N - i - 1), // vN-i-1
            idx_end_cap_start + static_cast<uint32_t>(N - i - 1)  // Duplicate (degenerate)
        });
        geometry.strip_color_index.push_back(end_cap_color_idx);
    }

    // ========== TRIANGLE COUNT VALIDATION ==========
    // Side faces: 2 triangles per face, N faces
    // Start cap: N-2 triangles (triangle fan)
    // End cap: N-2 triangles (triangle fan)
    int side_triangles = 2 * N;
    int start_cap_triangles = is_first_segment ? (N - 2) : 0;
    int end_cap_triangles = N - 2;
    int triangle_count = side_triangles + start_cap_triangles + end_cap_triangles;

    // Formula validation:
    // First segment: 2N + (N-2) + (N-2) = 4N - 4
    // Subsequent: 2N + (N-2) = 3N - 2
    if (segment.is_extrusion) {
        geometry.extrusion_triangle_count += static_cast<size_t>(triangle_count);
    } else {
        geometry.travel_triangle_count += static_cast<size_t>(triangle_count);
    }

    return end_cap;
}

glm::vec3 GeometryBuilder::compute_perpendicular(const glm::vec3& direction, float width) const {
    // Define "up" vector (Z-axis)
    glm::vec3 up(0.0f, 0.0f, 1.0f);

    // Compute perpendicular in XY plane
    // perpendicular = cross(direction, up)
    glm::vec3 perp = glm::cross(direction, up);

    // If direction is vertical (parallel to up), cross product will be zero
    // Fall back to using X-axis as perpendicular
    if (glm::length2(perp) < 1e-6f) {
        perp = glm::vec3(1.0f, 0.0f, 0.0f);
    } else {
        perp = glm::normalize(perp);
    }

    return perp * width;
}

uint32_t GeometryBuilder::compute_color_rgb(float z_height, float z_min, float z_max) const {
    if (!use_height_gradient_) {
        // Use solid filament color
        uint32_t color = (static_cast<uint32_t>(filament_r_) << 16) |
                         (static_cast<uint32_t>(filament_g_) << 8) |
                         static_cast<uint32_t>(filament_b_);
        static bool logged_once = false;
        if (!logged_once) {
            spdlog::debug("[GCode Geometry] compute_color_rgb: R={}, G={}, B={} -> 0x{:06X}",
                          filament_r_, filament_g_, filament_b_, color);
            logged_once = true;
        }
        return color;
    }

    // Rainbow gradient from blue (bottom) to red (top)
    // Normalize Z to [0, 1]
    float range = z_max - z_min;
    float t = (range > 0.0f) ? (z_height - z_min) / range : 0.5f;
    t = std::max(0.0f, std::min(1.0f, t)); // Clamp to [0, 1]

    // Rainbow spectrum: Blue → Cyan → Green → Yellow → Red
    // Using HSV color space converted to RGB
    float hue = (1.0f - t) * 240.0f; // 240° (blue) to 0° (red)

    // Simple HSV to RGB conversion (assuming S=1.0, V=1.0)
    float c = 1.0f; // Chroma (full saturation)
    float h_prime = hue / 60.0f;
    float x = c * (1.0f - std::abs(std::fmod(h_prime, 2.0f) - 1.0f));

    float r, g, b;
    if (h_prime < 1.0f) {
        r = c;
        g = x;
        b = 0.0f;
    } else if (h_prime < 2.0f) {
        r = x;
        g = c;
        b = 0.0f;
    } else if (h_prime < 3.0f) {
        r = 0.0f;
        g = c;
        b = x;
    } else if (h_prime < 4.0f) {
        r = 0.0f;
        g = x;
        b = c;
    } else if (h_prime < 5.0f) {
        r = x;
        g = 0.0f;
        b = c;
    } else {
        r = c;
        g = 0.0f;
        b = x;
    }

    uint8_t r8 = static_cast<uint8_t>(r * 255.0f);
    uint8_t g8 = static_cast<uint8_t>(g * 255.0f);
    uint8_t b8 = static_cast<uint8_t>(b * 255.0f);

    return (static_cast<uint32_t>(r8) << 16) | (static_cast<uint32_t>(g8) << 8) |
           static_cast<uint32_t>(b8);
}

void GeometryBuilder::set_filament_color(const std::string& hex_color) {
    use_height_gradient_ = false; // Disable gradient

    // Remove '#' prefix if present
    const char* hex_str = hex_color.c_str();
    if (hex_str[0] == '#')
        hex_str++;

    // Parse RGB hex (e.g., "26A69A")
    uint32_t rgb = static_cast<uint32_t>(std::strtol(hex_str, nullptr, 16));
    filament_r_ = (rgb >> 16) & 0xFF;
    filament_g_ = (rgb >> 8) & 0xFF;
    filament_b_ = rgb & 0xFF;

    spdlog::info("[GCode Geometry] Filament color set to #{:02X}{:02X}{:02X} (R={}, G={}, B={})",
                 filament_r_, filament_g_, filament_b_, filament_r_, filament_g_, filament_b_);
}

uint32_t GeometryBuilder::parse_hex_color(const std::string& hex_color) const {
    auto parsed = helix::parse_hex_color(hex_color);
    return parsed.value_or(0x808080); // Default gray for invalid input
}

uint32_t GeometryBuilder::compute_segment_color(const ToolpathSegment& segment, float z_min,
                                                float z_max) const {
    // Priority 1: Tool-specific color from palette (multi-color prints)
    if (!tool_color_palette_.empty() && segment.tool_index >= 0 &&
        segment.tool_index < static_cast<int>(tool_color_palette_.size())) {
        const std::string& hex_color = tool_color_palette_[static_cast<size_t>(segment.tool_index)];
        if (!hex_color.empty()) {
            return parse_hex_color(hex_color);
        }
    }

    // Priority 2: Z-height gradient (if enabled)
    if (use_height_gradient_) {
        float mid_z = (segment.start.z + segment.end.z) * 0.5f;
        return compute_color_rgb(mid_z, z_min, z_max);
    }

    // Priority 3: Default filament color
    return (static_cast<uint32_t>(filament_r_) << 16) | (static_cast<uint32_t>(filament_g_) << 8) |
           static_cast<uint32_t>(filament_b_);
}

} // namespace gcode
} // namespace helix
