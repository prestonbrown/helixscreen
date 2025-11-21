// Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// G-Code Geometry Builder Implementation

#include "gcode_geometry_builder.h"

#include <spdlog/spdlog.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <algorithm>
#include <cmath>
#include <glm/gtx/norm.hpp>
#include <limits>

namespace gcode {

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

    spdlog::debug("Quantization: bounds=[{:.2f},{:.2f},{:.2f}] to [{:.2f},{:.2f},{:.2f}], "
                  "scale={:.2f} units/mm, resolution={:.4f}mm",
                  min_bounds.x, min_bounds.y, min_bounds.z, max_bounds.x, max_bounds.y,
                  max_bounds.z, scale_factor, 1.0f / scale_factor);
}

int16_t QuantizationParams::quantize(float value, float min_bound) const {
    float normalized = (value - min_bound) * scale_factor;

    // Clamp to int16 range to prevent overflow
    normalized = std::max(-32768.0f, std::min(32767.0f, normalized));

    return static_cast<int16_t>(std::round(normalized));
}

float QuantizationParams::dequantize(int16_t value, float min_bound) const {
    return static_cast<float>(value) / scale_factor + min_bound;
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
// BuildStats Implementation
// ============================================================================

void GeometryBuilder::BuildStats::log() const {
    spdlog::info("Geometry Build Statistics:");
    spdlog::info("  Input segments:      {:>8}", input_segments);
    spdlog::info("  Simplified segments: {:>8} ({:.1f}% reduction)", output_segments,
                 simplification_ratio * 100.0f);
    spdlog::info("  Vertices generated:  {:>8}", vertices_generated);
    spdlog::info("  Triangles generated: {:>8}", triangles_generated);
    spdlog::info("  Memory usage:        {:>8} KB ({:.2f} MB)", memory_bytes / 1024,
                 memory_bytes / (1024.0 * 1024.0));

    if (input_segments > 0) {
        float bytes_per_segment = static_cast<float>(memory_bytes) / input_segments;
        spdlog::info("  Bytes per segment:   {:.1f}", bytes_per_segment);
    }
}

// ============================================================================
// GeometryBuilder Implementation
// ============================================================================

GeometryBuilder::GeometryBuilder() {
    stats_ = {};
}

// ============================================================================
// Palette Management
// ============================================================================

uint16_t GeometryBuilder::add_to_normal_palette(RibbonGeometry& geometry, const glm::vec3& normal) {
    // Very light quantization (0.001) to merge nearly-identical normals without visible banding
    constexpr float QUANT_STEP = 0.001f;
    glm::vec3 quantized;
    quantized.x = std::round(normal.x / QUANT_STEP) * QUANT_STEP;
    quantized.y = std::round(normal.y / QUANT_STEP) * QUANT_STEP;
    quantized.z = std::round(normal.z / QUANT_STEP) * QUANT_STEP;

    // Renormalize to ensure unit vector
    float length = glm::length(quantized);
    if (length > 0.0001f) {
        quantized /= length;
    } else {
        quantized = normal; // Fallback if quantization created zero vector
    }

    // Search for existing quantized normal in palette
    constexpr float EPSILON = 0.0001f;
    for (size_t i = 0; i < geometry.normal_palette.size(); ++i) {
        const glm::vec3& existing = geometry.normal_palette[i];
        if (glm::length(existing - quantized) < EPSILON) {
            return static_cast<uint16_t>(i); // Found existing
        }
    }

    // Add new quantized normal to palette (supports up to 65536 entries)
    if (geometry.normal_palette.size() >= 65536) {
        spdlog::warn("Normal palette full (65536 entries), reusing last entry");
        return 65535;
    }

    geometry.normal_palette.push_back(quantized); // Store quantized normal

    // Log palette size periodically
    if (geometry.normal_palette.size() % 1000 == 0) {
        spdlog::debug("Normal palette: {} entries", geometry.normal_palette.size());
    }

    return static_cast<uint16_t>(geometry.normal_palette.size() - 1);
}

uint8_t GeometryBuilder::add_to_color_palette(RibbonGeometry& geometry, uint32_t color_rgb) {
    // Search for existing color in palette
    for (size_t i = 0; i < geometry.color_palette.size(); ++i) {
        if (geometry.color_palette[i] == color_rgb) {
            return static_cast<uint8_t>(i); // Found existing
        }
    }

    // Add new color to palette
    if (geometry.color_palette.size() >= 256) {
        spdlog::warn("Color palette full (256 entries), reusing last entry");
        return 255;
    }

    geometry.color_palette.push_back(color_rgb);
    return static_cast<uint8_t>(geometry.color_palette.size() - 1);
}

RibbonGeometry GeometryBuilder::build(const ParsedGCodeFile& gcode,
                                      const SimplificationOptions& options) {
    RibbonGeometry geometry;
    stats_ = {}; // Reset statistics

    // Validate and apply options
    SimplificationOptions validated_opts = options;
    validated_opts.validate();

    spdlog::info("Building G-code geometry (tolerance={:.3f}mm, merging={})",
                 validated_opts.tolerance_mm, validated_opts.enable_merging);

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

    spdlog::debug("Expanded quantization bounds by {:.1f}mm for tube width {:.1f}mm",
                  expansion_margin, max_tube_width);

    // Collect all segments from all layers
    std::vector<ToolpathSegment> all_segments;
    for (const auto& layer : gcode.layers) {
        all_segments.insert(all_segments.end(), layer.segments.begin(), layer.segments.end());
    }

    stats_.input_segments = all_segments.size();
    spdlog::debug("Collected {} total segments from {} layers", all_segments.size(),
                  gcode.layers.size());

    // Step 1: Simplify segments (merge collinear lines)
    // TEMPORARILY DISABLED for testing - using raw segments
    std::vector<ToolpathSegment> simplified;
    if (false && validated_opts.enable_merging) {
        simplified = simplify_segments(all_segments, validated_opts);
        stats_.output_segments = simplified.size();
        stats_.simplification_ratio =
            1.0f - (static_cast<float>(simplified.size()) / all_segments.size());

        spdlog::info("Simplified {} → {} segments ({:.1f}% reduction)", all_segments.size(),
                     simplified.size(), stats_.simplification_ratio * 100.0f);
    } else {
        simplified = all_segments;
        stats_.output_segments = simplified.size();
        stats_.simplification_ratio = 0.0f;
        spdlog::info("Using RAW segments (simplification DISABLED): {} segments",
                     simplified.size());
    }

    // Find the maximum Z height (top layer) dynamically for debug filtering
    float max_z = -std::numeric_limits<float>::infinity();
    for (const auto& segment : simplified) {
        float z = std::round(segment.start.z * 100.0f) / 100.0f;
        if (z > max_z)
            max_z = z;
    }

    // Step 2: Generate ribbon geometry with vertex sharing
    // Track previous segment end vertices for reuse
    std::optional<TubeCap> prev_end_cap;
    glm::vec3 prev_end_pos{0.0f};

    // DEBUG: Track segment Y range
    float seg_y_min = FLT_MAX, seg_y_max = -FLT_MAX;
    size_t segments_skipped = 0;
    for (size_t i = 0; i < simplified.size(); ++i) {
        const auto& segment = simplified[i];

        // Skip degenerate segments (zero length)
        float segment_length = glm::distance(segment.start, segment.end);
        if (segment_length < 0.0001f) {
            segments_skipped++;
            continue;
        }

        // Track Y range
        seg_y_min = std::min({seg_y_min, segment.start.y, segment.end.y});
        seg_y_max = std::max({seg_y_max, segment.start.y, segment.end.y});

        // Check if we can share vertices with previous segment (OPTIMIZATION ENABLED!)
        bool can_share = false;
        float dist = 0.0f;
        float connection_tolerance = 0.0f;
        if (prev_end_cap.has_value()) {
            // Segments must connect spatially (within epsilon) and be same type
            dist = glm::distance(segment.start, prev_end_pos);
            // Use width-based tolerance: if gap is less than extrusion width, consider them
            // connected
            connection_tolerance = segment.width * 1.5f; //  50% overlap tolerance
            can_share = (dist < connection_tolerance) &&
                        (segment.is_extrusion == simplified[i - 1].is_extrusion);

            // Debug top layer connections
            float z = std::round(segment.start.z * 100.0f) / 100.0f;
            if (z == max_z) {
                spdlog::trace(
                    "  Seg {:3d}: dist={:.4f}mm, tol={:.4f}mm, width={:.4f}mm, can_share={}", i,
                    dist, connection_tolerance, segment.width, can_share);
            }
        }

        // Generate geometry, reusing previous end cap if segments connect
        TubeCap end_cap = generate_ribbon_vertices(segment, geometry, quant_params_,
                                                   can_share ? prev_end_cap : std::nullopt);

        // Store for next iteration
        prev_end_cap = end_cap;
        prev_end_pos = segment.end;
    }

    spdlog::trace("Segment Y range: [{:.1f}, {:.1f}]", seg_y_min, seg_y_max);

    // Categorize segments in top layer by angle and type (max_z already calculated above)
    size_t total_segs = 0, extrusion_segs = 0, travel_segs = 0;
    size_t diagonal_45_segs = 0, horizontal_segs = 0, vertical_segs = 0, other_angle_segs = 0;

    for (const auto& segment : simplified) {
        float z = std::round(segment.start.z * 100.0f) / 100.0f;
        if (std::abs(z - max_z) < 0.01f) {
            total_segs++;

            // Categorize by extrusion vs travel
            if (segment.is_extrusion) {
                extrusion_segs++;
            } else {
                travel_segs++;
            }

            // Calculate segment angle in XY plane
            glm::vec2 delta(segment.end.x - segment.start.x, segment.end.y - segment.start.y);
            float length_2d = glm::length(delta);

            if (length_2d > 0.01f) { // Skip near-zero length segments
                float angle_rad = std::atan2(delta.y, delta.x);
                float angle_deg = glm::degrees(angle_rad);

                // Normalize angle to [0, 180) for direction-independent classification
                if (angle_deg < 0)
                    angle_deg += 180.0f;

                // Categorize by angle (±5° tolerance)
                if (std::abs(angle_deg - 45.0f) < 5.0f || std::abs(angle_deg - 135.0f) < 5.0f) {
                    diagonal_45_segs++;
                } else if (std::abs(angle_deg - 0.0f) < 5.0f ||
                           std::abs(angle_deg - 180.0f) < 5.0f) {
                    horizontal_segs++;
                } else if (std::abs(angle_deg - 90.0f) < 5.0f) {
                    vertical_segs++;
                } else {
                    other_angle_segs++;
                }
            }
        }
    }

    if (total_segs > 0) {
        spdlog::info("═══ TOP LAYER Z={:.2f}mm SUMMARY ═══", max_z);
        spdlog::info("  Total segments: {}", total_segs);
        spdlog::info("  Extrusion: {} | Travel: {}", extrusion_segs, travel_segs);
        spdlog::info("  By angle:");
        spdlog::info("    Diagonal 45°: {}", diagonal_45_segs);
        spdlog::info("    Horizontal:   {}", horizontal_segs);
        spdlog::info("    Vertical:     {}", vertical_segs);
        spdlog::info("    Other angles: {}", other_angle_segs);
    }

    // Store quantization parameters for dequantization during rendering
    geometry.quantization = quant_params_;

    // Update final statistics
    stats_.vertices_generated = geometry.vertices.size();
    stats_.triangles_generated = geometry.indices.size();
    stats_.memory_bytes = geometry.memory_usage();

    // Log palette statistics
    spdlog::info("Palette stats: {} normals, {} colors (smooth_shading={})",
                 geometry.normal_palette.size(), geometry.color_palette.size(),
                 use_smooth_shading_);

    if (segments_skipped > 0) {
        spdlog::warn("Skipped {} degenerate segments (zero length)", segments_skipped);
    }

    stats_.log();

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
        // 2. Endpoints connect (current.end ≈ next.start)
        // 3. Same object (for per-object highlighting)
        // 4. Collinear within tolerance

        bool same_type = (current.is_extrusion == next.is_extrusion);
        bool endpoints_connect = glm::distance2(current.end, next.start) < 0.0001f;
        bool same_object = (current.object_name == next.object_name);

        if (same_type && endpoints_connect && same_object) {
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

        // Cannot merge - save current and start new segment
        simplified.push_back(current);
        current = next;
    }

    // Add final segment
    simplified.push_back(current);

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
    if (!highlighted_objects_.empty() && !segment.object_name.empty() &&
        highlighted_objects_.count(segment.object_name) > 0) {
        constexpr float HIGHLIGHT_BRIGHTNESS = 1.8f;
        uint8_t r = static_cast<uint8_t>(std::min(255.0f, ((rgb >> 16) & 0xFF) * HIGHLIGHT_BRIGHTNESS));
        uint8_t g = static_cast<uint8_t>(std::min(255.0f, ((rgb >> 8) & 0xFF) * HIGHLIGHT_BRIGHTNESS));
        uint8_t b = static_cast<uint8_t>(std::min(255.0f, (rgb & 0xFF) * HIGHLIGHT_BRIGHTNESS));
        rgb = (r << 16) | (g << 8) | b;
    }

    uint8_t color_idx = add_to_color_palette(geometry, rgb);
    uint8_t color_idx_up = color_idx;
    uint8_t color_idx_down = color_idx;
    uint8_t color_idx_right = color_idx;
    uint8_t color_idx_left = color_idx;

    // For diamond cross-section debug colors, we need FACE colors not VERTEX colors
    uint8_t face_color_top_right = color_idx;
    uint8_t face_color_right_bottom = color_idx;
    uint8_t face_color_bottom_left = color_idx;
    uint8_t face_color_left_top = color_idx;

    if (debug_face_colors_) {
        // Override with distinct debug colors for each face
        color_idx_up = add_to_color_palette(geometry, DebugColors::TOP);
        color_idx_down = add_to_color_palette(geometry, DebugColors::BOTTOM);
        color_idx_right = add_to_color_palette(geometry, DebugColors::RIGHT);
        color_idx_left = add_to_color_palette(geometry, DebugColors::LEFT);

        // Assign unique colors to each of the 4 side faces
        face_color_top_right = color_idx_up;       // RED for top-right face
        face_color_right_bottom = color_idx_right;  // YELLOW for right-bottom face
        face_color_bottom_left = color_idx_down;    // BLUE for bottom-left face
        face_color_left_top = color_idx_left;       // GREEN for left-top face

        static bool logged_once = false;
        if (!logged_once) {
            spdlog::debug("DEBUG FACE COLORS ACTIVE: TopRight=Red, RightBottom=Yellow, BottomLeft=Blue, LeftTop=Green, StartCap=Magenta, EndCap=Cyan");
            logged_once = true;
        }
    }

    // OrcaSlicer approach: Apply vertical offset to BOTH prev and curr positions
    // This makes the TOP edge sit at the path Z-coordinate
    const glm::vec3 prev_pos = segment.start - half_height * perp_up;
    const glm::vec3 curr_pos = segment.end - half_height * perp_up;

    // Pre-compute offsets (OrcaSlicer naming)
    const glm::vec3 d_up = half_height * perp_up;
    const glm::vec3 d_down = -half_height * perp_up;
    const glm::vec3 d_right = half_width * right;
    const glm::vec3 d_left = -half_width * right;

    // For diamond cross-section, vertices need normals averaged from adjacent faces
    // Face normals: perpendicular to the diagonal faces
    // Top face (UP→RIGHT): normal points UP+RIGHT at 45°
    // Right face (RIGHT→DOWN): normal points RIGHT+DOWN at 45°
    // Bottom face (DOWN→LEFT): normal points DOWN+LEFT at 45°
    // Left face (LEFT→UP): normal points LEFT+UP at 45°

    glm::vec3 face_normal_top = glm::normalize(perp_up + right);      // UP-RIGHT diagonal
    glm::vec3 face_normal_right = glm::normalize(right - perp_up);    // RIGHT-DOWN diagonal
    glm::vec3 face_normal_bottom = glm::normalize(-perp_up - right);  // DOWN-LEFT diagonal
    glm::vec3 face_normal_left = glm::normalize(-right + perp_up);    // LEFT-UP diagonal

    // Vertex normals: average of adjacent face normals (smooth shading)
    const glm::vec3 normal_up = glm::normalize(face_normal_left + face_normal_top);
    const glm::vec3 normal_right = glm::normalize(face_normal_top + face_normal_right);
    const glm::vec3 normal_down = glm::normalize(face_normal_right + face_normal_bottom);
    const glm::vec3 normal_left = glm::normalize(face_normal_bottom + face_normal_left);

    uint32_t idx_start = geometry.vertices.size();

    // OrcaSlicer approach:
    // First segment: 4 prev vertices (up/right/down/left) + 4 curr vertices
    // Subsequent: 2 prev vertices (right/left only) + 4 curr vertices

    bool is_first_segment = !prev_start_cap.has_value();

    if (is_first_segment) {
        // First segment: generate all 4 prev vertices
        // START CAP: All normals point BACKWARD along segment (-dir)
        glm::vec3 cap_normal_start = -dir;
        uint16_t cap_normal_idx = add_to_normal_palette(geometry, cap_normal_start);

        // Use unique START_CAP color for debug visualization
        uint8_t start_cap_color_idx = debug_face_colors_
            ? add_to_color_palette(geometry, DebugColors::START_CAP)
            : color_idx_up;  // Use normal color if not debugging

        glm::vec3 pos_prev_up = prev_pos + d_up;
        glm::vec3 pos_prev_right = prev_pos + d_right;
        glm::vec3 pos_prev_down = prev_pos + d_down;
        glm::vec3 pos_prev_left = prev_pos + d_left;

        geometry.vertices.push_back({
            quant.quantize_vec3(pos_prev_up),
            cap_normal_idx,  // Axial normal, not radial
            start_cap_color_idx  // MAGENTA for start cap in debug mode
        });
        geometry.vertices.push_back({
            quant.quantize_vec3(pos_prev_right),
            cap_normal_idx,  // Axial normal, not radial
            start_cap_color_idx  // MAGENTA for start cap in debug mode
        });
        geometry.vertices.push_back({
            quant.quantize_vec3(pos_prev_down),
            cap_normal_idx,  // Axial normal, not radial
            start_cap_color_idx  // MAGENTA for start cap in debug mode
        });
        geometry.vertices.push_back({
            quant.quantize_vec3(pos_prev_left),
            cap_normal_idx,  // Axial normal, not radial
            start_cap_color_idx  // MAGENTA for start cap in debug mode
        });
        idx_start += 4;

        // NOW create side tube "prev" vertices - one set per FACE (not per edge!)
        // Face 1 (TOP-RIGHT): uses UP and RIGHT edge positions
        geometry.vertices.push_back({quant.quantize_vec3(pos_prev_up), add_to_normal_palette(geometry, face_normal_top), face_color_top_right});
        geometry.vertices.push_back({quant.quantize_vec3(pos_prev_right), add_to_normal_palette(geometry, face_normal_top), face_color_top_right});
        // Face 2 (RIGHT-BOTTOM): uses RIGHT and DOWN edge positions
        geometry.vertices.push_back({quant.quantize_vec3(pos_prev_right), add_to_normal_palette(geometry, face_normal_right), face_color_right_bottom});
        geometry.vertices.push_back({quant.quantize_vec3(pos_prev_down), add_to_normal_palette(geometry, face_normal_right), face_color_right_bottom});
        // Face 3 (BOTTOM-LEFT): uses DOWN and LEFT edge positions
        geometry.vertices.push_back({quant.quantize_vec3(pos_prev_down), add_to_normal_palette(geometry, face_normal_bottom), face_color_bottom_left});
        geometry.vertices.push_back({quant.quantize_vec3(pos_prev_left), add_to_normal_palette(geometry, face_normal_bottom), face_color_bottom_left});
        // Face 4 (LEFT-TOP): uses LEFT and UP edge positions
        geometry.vertices.push_back({quant.quantize_vec3(pos_prev_left), add_to_normal_palette(geometry, face_normal_left), face_color_left_top});
        geometry.vertices.push_back({quant.quantize_vec3(pos_prev_up), add_to_normal_palette(geometry, face_normal_left), face_color_left_top});
        idx_start += 8;

        if (debug_face_colors_) {
            spdlog::info("  Orientation vectors:");
            spdlog::info("    dir=({:.3f},{:.3f},{:.3f})", dir.x, dir.y, dir.z);
            spdlog::info("    right=({:.3f},{:.3f},{:.3f})", right.x, right.y, right.z);
            spdlog::info("    perp_up=({:.3f},{:.3f},{:.3f})", perp_up.x, perp_up.y, perp_up.z);
            spdlog::info("  Start cap vertices [0-3]: MAGENTA, axial normals");
            spdlog::info("  Side tube prev vertices [4-7]: face colors, radial normals");
            spdlog::info("    PREV_UP[{}]:    ({:.3f},{:.3f},{:.3f})", idx_start-4, pos_prev_up.x, pos_prev_up.y, pos_prev_up.z);
            spdlog::info("    PREV_RIGHT[{}]: ({:.3f},{:.3f},{:.3f})", idx_start-3, pos_prev_right.x, pos_prev_right.y, pos_prev_right.z);
            spdlog::info("    PREV_DOWN[{}]:  ({:.3f},{:.3f},{:.3f})", idx_start-2, pos_prev_down.x, pos_prev_down.y, pos_prev_down.z);
            spdlog::info("    PREV_LEFT[{}]:  ({:.3f},{:.3f},{:.3f})", idx_start-1, pos_prev_left.x, pos_prev_left.y, pos_prev_left.z);
        }
    } else {
        // Subsequent segments: only generate right/left prev vertices
        geometry.vertices.push_back({
            quant.quantize_vec3(prev_pos + d_right),
            add_to_normal_palette(geometry, normal_right),
            color_idx_right
        });
        geometry.vertices.push_back({
            quant.quantize_vec3(prev_pos + d_left),
            add_to_normal_palette(geometry, normal_left),
            color_idx_left
        });
        idx_start += 2;
    }

    // Generate curr vertices - one set per FACE (matching prev structure)
    glm::vec3 pos_up = curr_pos + d_up;
    glm::vec3 pos_right = curr_pos + d_right;
    glm::vec3 pos_down = curr_pos + d_down;
    glm::vec3 pos_left = curr_pos + d_left;

    // Face 1 (TOP-RIGHT): uses UP and RIGHT edge positions
    geometry.vertices.push_back({quant.quantize_vec3(pos_up), add_to_normal_palette(geometry, face_normal_top), face_color_top_right});
    geometry.vertices.push_back({quant.quantize_vec3(pos_right), add_to_normal_palette(geometry, face_normal_top), face_color_top_right});
    // Face 2 (RIGHT-BOTTOM): uses RIGHT and DOWN edge positions
    geometry.vertices.push_back({quant.quantize_vec3(pos_right), add_to_normal_palette(geometry, face_normal_right), face_color_right_bottom});
    geometry.vertices.push_back({quant.quantize_vec3(pos_down), add_to_normal_palette(geometry, face_normal_right), face_color_right_bottom});
    // Face 3 (BOTTOM-LEFT): uses DOWN and LEFT edge positions
    geometry.vertices.push_back({quant.quantize_vec3(pos_down), add_to_normal_palette(geometry, face_normal_bottom), face_color_bottom_left});
    geometry.vertices.push_back({quant.quantize_vec3(pos_left), add_to_normal_palette(geometry, face_normal_bottom), face_color_bottom_left});
    // Face 4 (LEFT-TOP): uses LEFT and UP edge positions
    geometry.vertices.push_back({quant.quantize_vec3(pos_left), add_to_normal_palette(geometry, face_normal_left), face_color_left_top});
    geometry.vertices.push_back({quant.quantize_vec3(pos_up), add_to_normal_palette(geometry, face_normal_left), face_color_left_top});
    idx_start += 8;

    // Track end cap edge positions for end cap generation (use first occurrence of each edge)
    TubeCap end_cap;
    end_cap[0] = idx_start - 8 + 0;  // Face1's UP vertex
    end_cap[1] = idx_start - 8 + 1;  // Face1's RIGHT vertex
    end_cap[2] = idx_start - 8 + 3;  // Face2's DOWN vertex
    end_cap[3] = idx_start - 8 + 5;  // Face3's LEFT vertex

    static int debug_count = 0;
    if (debug_count < 2 && debug_face_colors_) {
        spdlog::info("=== Segment {} | is_first={} ===", debug_count, is_first_segment);
        spdlog::info("  Segment: start=({:.3f},{:.3f},{:.3f}) end=({:.3f},{:.3f},{:.3f})",
                     segment.start.x, segment.start.y, segment.start.z,
                     segment.end.x, segment.end.y, segment.end.z);
        spdlog::info("  Direction: dir=({:.3f},{:.3f},{:.3f}) right=({:.3f},{:.3f},{:.3f}) perp_up=({:.3f},{:.3f},{:.3f})",
                     dir.x, dir.y, dir.z, right.x, right.y, right.z, perp_up.x, perp_up.y, perp_up.z);
        spdlog::info("  Cross-section center: prev_pos=({:.3f},{:.3f},{:.3f}) curr_pos=({:.3f},{:.3f},{:.3f})",
                     prev_pos.x, prev_pos.y, prev_pos.z, curr_pos.x, curr_pos.y, curr_pos.z);
        spdlog::info("  Curr vertices:");
        spdlog::info("    UP[{}]:    ({:.3f},{:.3f},{:.3f})", end_cap[0], pos_up.x, pos_up.y, pos_up.z);
        spdlog::info("    RIGHT[{}]: ({:.3f},{:.3f},{:.3f})", end_cap[1], pos_right.x, pos_right.y, pos_right.z);
        spdlog::info("    DOWN[{}]:  ({:.3f},{:.3f},{:.3f})", end_cap[2], pos_down.x, pos_down.y, pos_down.z);
        spdlog::info("    LEFT[{}]:  ({:.3f},{:.3f},{:.3f})", end_cap[3], pos_left.x, pos_left.y, pos_left.z);
        debug_count++;
    }

    // Generate triangle strips for the 4 faces
    // Need to compute vertex indices based on whether this is first segment or not
    if (is_first_segment) {
        // First segment vertex layout:
        //   Start cap: 0-3
        //   Face1 prev: 4-5 (UP, RIGHT)
        //   Face2 prev: 6-7 (RIGHT, DOWN)
        //   Face3 prev: 8-9 (DOWN, LEFT)
        //   Face4 prev: 10-11 (LEFT, UP)
        //   Face1 curr: 12-13 (UP, RIGHT)
        //   Face2 curr: 14-15 (RIGHT, DOWN)
        //   Face3 curr: 16-17 (DOWN, LEFT)
        //   Face4 curr: 18-19 (LEFT, UP)
        uint32_t base = idx_start - 20;  // Total vertices: 4 (cap) + 8 (prev) + 8 (curr)
        uint32_t start_cap_up = base + 0;
        uint32_t start_cap_right = base + 1;
        uint32_t start_cap_down = base + 2;
        uint32_t start_cap_left = base + 3;

        // Face strips: each face uses 2 prev + 2 curr vertices
        uint32_t face1_prev_start = base + 4;
        uint32_t face2_prev_start = base + 6;
        uint32_t face3_prev_start = base + 8;
        uint32_t face4_prev_start = base + 10;
        uint32_t face1_curr_start = base + 12;
        uint32_t face2_curr_start = base + 14;
        uint32_t face3_curr_start = base + 16;
        uint32_t face4_curr_start = base + 18;

        // Four side faces - each face has 4 vertices in strip order [prev1, prev2, curr1, curr2]
        geometry.strips.push_back({face1_prev_start, face1_prev_start+1, face1_curr_start, face1_curr_start+1});  // TOP-RIGHT (RED)
        geometry.strips.push_back({face2_prev_start, face2_prev_start+1, face2_curr_start, face2_curr_start+1});  // RIGHT-BOTTOM (YELLOW)
        geometry.strips.push_back({face3_prev_start, face3_prev_start+1, face3_curr_start, face3_curr_start+1});  // BOTTOM-LEFT (BLUE)
        geometry.strips.push_back({face4_prev_start, face4_prev_start+1, face4_curr_start, face4_curr_start+1});  // LEFT-TOP (GREEN)

        // Start cap - REAL 4-vertex strip creating 2 valid triangles
        // Diamond quad with CCW winding (for GL_CULL_FACE)
        // Strip [A,B,C,D] creates: Triangle1(A,B,C) Triangle2(B,D,C)
        // From front (+X), we want CCW: UP → LEFT → RIGHT → DOWN
        // So: [UP, LEFT, RIGHT, DOWN] creates triangles (UP,LEFT,RIGHT) and (LEFT,DOWN,RIGHT)
        geometry.strips.push_back({start_cap_up, start_cap_left, start_cap_right, start_cap_down});

        if (debug_face_colors_) {
            spdlog::info("START CAP: Strip indices = [{}, {}, {}, {}]", start_cap_up, start_cap_left, start_cap_right, start_cap_down);
            spdlog::info("  Triangle 1: ({},{},{}) = (UP,LEFT,RIGHT) - CCW from front", start_cap_up, start_cap_left, start_cap_right);
            spdlog::info("  Triangle 2: ({},{},{}) = (LEFT,DOWN,RIGHT) - CCW from front", start_cap_left, start_cap_down, start_cap_right);
        }

    } else {
        // Subsequent segments: reuse previous segment's end_cap for prev vertices
        // We only generated 2 new prev vertices (right, left) at idx_start-6 and idx_start-5
        // The other 2 (up, down) come from the previous segment's end_cap
        uint32_t prev_up = prev_start_cap.value()[0];      // From previous segment
        uint32_t prev_down = prev_start_cap.value()[2];    // From previous segment
        uint32_t prev_right = idx_start - 6;               // Just generated
        uint32_t prev_left = idx_start - 5;                // Just generated
        uint32_t curr_up = idx_start - 4;
        uint32_t curr_right = idx_start - 3;
        uint32_t curr_down = idx_start - 2;
        uint32_t curr_left = idx_start - 1;

        // TESTING: ALL DISABLED
        // geometry.strips.push_back({prev_up, prev_right, curr_up, curr_right});
        // geometry.strips.push_back({prev_right, prev_down, curr_right, curr_down});
        // geometry.strips.push_back({prev_down, prev_left, curr_down, curr_left});
        // geometry.strips.push_back({prev_left, prev_up, curr_left, curr_up});
    }

    // End cap - Use the SAME positions as end_cap array but with axial normals
    uint8_t end_cap_color_idx = debug_face_colors_
        ? add_to_color_palette(geometry, DebugColors::END_CAP)
        : color_idx_up;

    glm::vec3 cap_normal_end = -dir;  // Same as start cap
    uint16_t end_cap_normal_idx = add_to_normal_palette(geometry, cap_normal_end);

    // Create 4 new vertices at the SAME POSITIONS as end_cap but with axial normals
    // end_cap[0] is UP, end_cap[1] is RIGHT, end_cap[2] is DOWN, end_cap[3] is LEFT
    uint32_t idx_end_cap_start = idx_start;

    if (debug_face_colors_) {
        spdlog::info("END CAP SOURCE INDICES: end_cap[0]={}, end_cap[1]={}, end_cap[2]={}, end_cap[3]={}",
                     end_cap[0], end_cap[1], end_cap[2], end_cap[3]);
        spdlog::info("  geometry.vertices.size() = {}", geometry.vertices.size());
        auto pos_up_dequant = quant.dequantize_vec3(geometry.vertices[end_cap[0]].position);
        spdlog::info("  Source vertex[{}] position: ({:.3f},{:.3f},{:.3f})",
                     end_cap[0], pos_up_dequant.x, pos_up_dequant.y, pos_up_dequant.z);
    }

    // Reuse the POSITIONS from the end_cap vertices (which are at curr_pos + d_*)
    geometry.vertices.push_back({geometry.vertices[end_cap[0]].position, end_cap_normal_idx, end_cap_color_idx});  // UP
    geometry.vertices.push_back({geometry.vertices[end_cap[1]].position, end_cap_normal_idx, end_cap_color_idx});  // RIGHT
    geometry.vertices.push_back({geometry.vertices[end_cap[2]].position, end_cap_normal_idx, end_cap_color_idx});  // DOWN
    geometry.vertices.push_back({geometry.vertices[end_cap[3]].position, end_cap_normal_idx, end_cap_color_idx});  // LEFT
    idx_start += 4;

    if (debug_face_colors_) {
        // Verify the newly added end cap vertices
        spdlog::info("END CAP VERTICES ADDED:");
        for (int i = 0; i < 4; i++) {
            auto pos = quant.dequantize_vec3(geometry.vertices[idx_end_cap_start + i].position);
            spdlog::info("  Vertex[{}]: ({:.3f},{:.3f},{:.3f})", idx_end_cap_start + i, pos.x, pos.y, pos.z);
        }
    }

    // OPPOSITE winding from start cap (caps face opposite directions)
    // Start cap: [UP, LEFT, RIGHT, DOWN] for normal pointing backward (-X)
    // End cap: [UP, RIGHT, LEFT, DOWN] for normal pointing backward (-X) but at far end
    geometry.strips.push_back({idx_end_cap_start + 0, idx_end_cap_start + 1, idx_end_cap_start + 3, idx_end_cap_start + 2});

    if (debug_face_colors_) {
        spdlog::info("END CAP: Strip indices = [{}, {}, {}, {}]",
                     idx_end_cap_start + 0, idx_end_cap_start + 1, idx_end_cap_start + 3, idx_end_cap_start + 2);
        spdlog::info("  Total geometry.strips.size() = {}", geometry.strips.size());
    }

    // Update counters
    int triangle_count = 8 + 4; // 4 side faces (8 tri) + start cap (2 tri) + end cap (2 tri)
    if (!is_first_segment) {
        triangle_count -= 2; // No start cap for subsequent segments
    }
    if (segment.is_extrusion) {
        geometry.extrusion_triangle_count += triangle_count;
    } else {
        geometry.travel_triangle_count += triangle_count;
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
            spdlog::debug("compute_color_rgb: R={}, G={}, B={} -> 0x{:06X}", filament_r_,
                          filament_g_, filament_b_, color);
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
    uint32_t rgb = std::strtol(hex_str, nullptr, 16);
    filament_r_ = (rgb >> 16) & 0xFF;
    filament_g_ = (rgb >> 8) & 0xFF;
    filament_b_ = rgb & 0xFF;

    spdlog::info("Filament color set to #{:02X}{:02X}{:02X} (R={}, G={}, B={})", filament_r_,
                 filament_g_, filament_b_, filament_r_, filament_g_, filament_b_);
}

uint32_t GeometryBuilder::parse_hex_color(const std::string& hex_color) const {
    if (hex_color.length() < 6) {
        return 0x808080; // Default gray for invalid input
    }

    // Skip '#' prefix if present
    const char* hex_str = hex_color.c_str();
    if (hex_str[0] == '#') {
        hex_str++;
    }

    // Parse #RRGGBB format
    unsigned long value = std::strtoul(hex_str, nullptr, 16);

    uint8_t r = (value >> 16) & 0xFF;
    uint8_t g = (value >> 8) & 0xFF;
    uint8_t b = value & 0xFF;

    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}

uint32_t GeometryBuilder::compute_segment_color(const ToolpathSegment& segment, float z_min,
                                                float z_max) const {
    // Priority 1: Tool-specific color from palette (multi-color prints)
    if (!tool_color_palette_.empty() && segment.tool_index >= 0 &&
        segment.tool_index < static_cast<int>(tool_color_palette_.size())) {
        const std::string& hex_color = tool_color_palette_[segment.tool_index];
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
    return (static_cast<uint32_t>(filament_r_) << 16) |
           (static_cast<uint32_t>(filament_g_) << 8) | static_cast<uint32_t>(filament_b_);
}

} // namespace gcode
