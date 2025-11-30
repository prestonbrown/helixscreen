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

#include "gcode_renderer.h"

#include "ui_theme.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

namespace gcode {

GCodeRenderer::GCodeRenderer() {
    // Load colors from theme
    // Note: ui_theme_parse_color requires theme to be initialized
    // If called before theme init, will use fallback colors
    color_extrusion_ = ui_theme_parse_color(lv_xml_get_const(NULL, "primary_color"));
    color_travel_ = ui_theme_parse_color(lv_xml_get_const(NULL, "text_secondary"));
    color_object_boundary_ = ui_theme_parse_color(lv_xml_get_const(NULL, "secondary_color"));
    color_highlighted_ = ui_theme_parse_color(lv_xml_get_const(NULL, "secondary_color"));
    color_excluded_ = ui_theme_parse_color(lv_xml_get_const(NULL, "danger_color"));

    // Save theme defaults for reset
    theme_color_extrusion_ = color_extrusion_;
    theme_color_travel_ = color_travel_;
}

void GCodeRenderer::set_viewport_size(int width, int height) {
    viewport_width_ = width;
    viewport_height_ = height;
}

void GCodeRenderer::set_options(const RenderOptions& options) {
    options_ = options;
}

void GCodeRenderer::set_show_travels(bool show) {
    options_.show_travels = show;
}

void GCodeRenderer::set_show_extrusions(bool show) {
    options_.show_extrusions = show;
}

void GCodeRenderer::set_highlighted_object(const std::string& name) {
    options_.highlighted_object = name;
}

void GCodeRenderer::set_excluded_objects(const std::unordered_set<std::string>& names) {
    options_.excluded_objects = names;
}

void GCodeRenderer::set_lod_level(LODLevel level) {
    options_.lod = level;
}

void GCodeRenderer::set_layer_range(int start, int end) {
    options_.layer_start = start;
    options_.layer_end = end;
}

void GCodeRenderer::set_extrusion_color(lv_color_t color) {
    color_extrusion_ = color;
    use_custom_extrusion_color_ = true;
}

void GCodeRenderer::set_travel_color(lv_color_t color) {
    color_travel_ = color;
    use_custom_travel_color_ = true;
}

void GCodeRenderer::set_global_opacity(lv_opa_t opacity) {
    global_opacity_ = opacity;
}

void GCodeRenderer::set_brightness_factor(float factor) {
    // Clamp to reasonable range
    brightness_factor_ = std::max(0.5f, std::min(2.0f, factor));
}

void GCodeRenderer::reset_colors() {
    color_extrusion_ = theme_color_extrusion_;
    color_travel_ = theme_color_travel_;
    use_custom_extrusion_color_ = false;
    use_custom_travel_color_ = false;
    brightness_factor_ = 1.0f;
    global_opacity_ = LV_OPA_90;
}

void GCodeRenderer::render(lv_layer_t* layer, const ParsedGCodeFile& gcode,
                           const GCodeCamera& camera) {
    if (!layer) {
        spdlog::error("Cannot render: null layer");
        return;
    }

    if (gcode.layers.empty()) {
        spdlog::debug("No layers to render");
        return;
    }

    // Reset statistics
    segments_rendered_ = 0;
    segments_culled_ = 0;

    // Get view-projection matrix
    glm::mat4 transform = camera.get_view_projection_matrix();

    // Store view matrix for depth calculations
    view_matrix_ = camera.get_view_matrix();

    // Calculate depth range from model bounding box for normalization
    glm::vec3 bbox_min = gcode.global_bounding_box.min;
    glm::vec3 bbox_max = gcode.global_bounding_box.max;

    // Transform bounding box corners to view space
    glm::vec4 corner1_view = view_matrix_ * glm::vec4(bbox_min, 1.0f);
    glm::vec4 corner2_view = view_matrix_ * glm::vec4(bbox_max, 1.0f);

    // In view space, negative Z = in front of camera (OpenGL convention)
    // We want depth range to normalize
    float depth_min = std::min(corner1_view.z, corner2_view.z);
    float depth_max = std::max(corner1_view.z, corner2_view.z);
    depth_range_ = depth_max - depth_min;
    min_depth_ = depth_min;

    // Avoid division by zero
    if (depth_range_ < 0.001f) {
        depth_range_ = 1.0f;
    }

    // Calculate Z-height range for color gradient mapping
    z_min_ = bbox_min.z;
    z_max_ = bbox_max.z;

    // Avoid division by zero in gradient mapping
    if (std::abs(z_max_ - z_min_) < 0.001f) {
        z_max_ = z_min_ + 1.0f;
    }

    // Determine layer range
    int start_layer = options_.layer_start;
    int end_layer = (options_.layer_end >= 0)
                        ? std::min(options_.layer_end, static_cast<int>(gcode.layers.size()) - 1)
                        : static_cast<int>(gcode.layers.size()) - 1;

    start_layer = std::clamp(start_layer, 0, static_cast<int>(gcode.layers.size()) - 1);

    // Render object boundaries if enabled
    if (options_.show_object_bounds) {
        for (const auto& [name, obj] : gcode.objects) {
            render_object_boundary(layer, obj, transform);
        }
    }

    // Render layers
    for (int i = start_layer; i <= end_layer; ++i) {
        render_layer(layer, gcode.layers[i], transform);
    }

    spdlog::trace("Rendered {} segments, culled {} segments", segments_rendered_, segments_culled_);
}

void GCodeRenderer::render_layer(lv_layer_t* layer, const Layer& gcode_layer,
                                 const glm::mat4& transform) {
    // LOD: Skip segments based on level
    int skip_factor = 1 << static_cast<int>(options_.lod); // 1, 2, or 4

    for (size_t i = 0; i < gcode_layer.segments.size(); i += skip_factor) {
        const auto& segment = gcode_layer.segments[i];

        if (should_render_segment(segment)) {
            render_segment(layer, segment, transform);
            segments_rendered_++;
        } else {
            segments_culled_++;
        }
    }
}

void GCodeRenderer::render_segment(lv_layer_t* layer, const ToolpathSegment& segment,
                                   const glm::mat4& transform) {
    // Project 3D points to 2D screen space
    auto p1_opt = project_to_screen(segment.start, transform);
    auto p2_opt = project_to_screen(segment.end, transform);

    if (!p1_opt || !p2_opt) {
        return; // Outside view
    }

    glm::vec2 p1 = *p1_opt;
    glm::vec2 p2 = *p2_opt;

    // Clip line to viewport
    if (!clip_line_to_viewport(p1, p2)) {
        return;
    }

    // Calculate view-space depth for the segment midpoint
    glm::vec3 midpoint = (segment.start + segment.end) * 0.5f;
    glm::vec4 view_pos = view_matrix_ * glm::vec4(midpoint, 1.0f);
    float depth = view_pos.z;

    // Normalize depth to [0, 1] where 0 = closest, 1 = farthest
    float normalized_depth = (depth - min_depth_) / depth_range_;
    normalized_depth = std::clamp(normalized_depth, 0.0f, 1.0f);

    // Get line style with depth info, then draw
    lv_draw_line_dsc_t dsc = get_line_style(segment, normalized_depth);
    draw_line(layer, p1, p2, dsc);
}

void GCodeRenderer::render_object_boundary(lv_layer_t* layer, const GCodeObject& object,
                                           const glm::mat4& transform) {
    if (object.polygon.size() < 2) {
        return;
    }

    // Draw polygon outline at Z=0 (print bed level)
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color =
        (object.name == options_.highlighted_object) ? color_highlighted_ : color_object_boundary_;
    dsc.width = 2;
    dsc.opa = LV_OPA_70;

    for (size_t i = 0; i < object.polygon.size(); ++i) {
        size_t next = (i + 1) % object.polygon.size();

        glm::vec3 p1_3d(object.polygon[i].x, object.polygon[i].y, 0.0f);
        glm::vec3 p2_3d(object.polygon[next].x, object.polygon[next].y, 0.0f);

        auto p1_opt = project_to_screen(p1_3d, transform);
        auto p2_opt = project_to_screen(p2_3d, transform);

        if (p1_opt && p2_opt) {
            glm::vec2 p1 = *p1_opt;
            glm::vec2 p2 = *p2_opt;

            if (clip_line_to_viewport(p1, p2)) {
                draw_line(layer, p1, p2, dsc);
            }
        }
    }
}

std::optional<glm::vec2> GCodeRenderer::project_to_screen(const glm::vec3& world_pos,
                                                          const glm::mat4& transform) const {
    // Transform to clip space
    glm::vec4 clip_space = transform * glm::vec4(world_pos, 1.0f);

    // Perspective divide
    if (clip_space.w == 0.0f) {
        return std::nullopt; // Invalid
    }

    glm::vec3 ndc(clip_space.x / clip_space.w, clip_space.y / clip_space.w,
                  clip_space.z / clip_space.w);

    // Frustum culling: Check if in normalized device coordinates [-1, 1]
    if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f || ndc.z < -1.0f ||
        ndc.z > 1.0f) {
        return std::nullopt; // Outside view frustum
    }

    // Convert to screen coordinates
    float screen_x = (ndc.x + 1.0f) * 0.5f * viewport_width_;
    float screen_y = (1.0f - ndc.y) * 0.5f * viewport_height_; // Flip Y

    return glm::vec2(screen_x, screen_y);
}

bool GCodeRenderer::should_render_segment(const ToolpathSegment& segment) const {
    // Filter by segment type
    if (segment.is_extrusion && !options_.show_extrusions) {
        return false;
    }
    if (!segment.is_extrusion && !options_.show_travels) {
        return false;
    }

    // No further culling here - done in project_to_screen()
    return true;
}

bool GCodeRenderer::clip_line_to_viewport(glm::vec2& p1, glm::vec2& p2) const {
    // Simple Cohen-Sutherland line clipping
    // For now, just check if line is completely outside viewport
    // TODO: Implement proper clipping for partially visible lines

    float min_x = 0.0f;
    float max_x = static_cast<float>(viewport_width_);
    float min_y = 0.0f;
    float max_y = static_cast<float>(viewport_height_);

    // Both points outside on same side = completely outside
    if ((p1.x < min_x && p2.x < min_x) || (p1.x > max_x && p2.x > max_x) ||
        (p1.y < min_y && p2.y < min_y) || (p1.y > max_y && p2.y > max_y)) {
        return false;
    }

    // Simple clamp for now (not perfect but acceptable for Phase 1)
    p1.x = std::clamp(p1.x, min_x, max_x);
    p1.y = std::clamp(p1.y, min_y, max_y);
    p2.x = std::clamp(p2.x, min_x, max_x);
    p2.y = std::clamp(p2.y, min_y, max_y);

    return true;
}

lv_draw_line_dsc_t GCodeRenderer::get_line_style(const ToolpathSegment& segment,
                                                 float normalized_depth) const {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);

    // Determine line width and base opacity
    bool is_highlighted =
        !options_.highlighted_object.empty() && segment.object_name == options_.highlighted_object;
    bool is_excluded = !segment.object_name.empty() &&
                       options_.excluded_objects.count(segment.object_name) > 0;

    lv_opa_t base_opa;
    int line_width;

    if (is_excluded) {
        // Excluded objects: render with reduced opacity and thinner lines
        line_width = 1;
        base_opa = LV_OPA_60; // 60% opacity for strikethrough effect
    } else if (is_highlighted) {
        line_width = 3;
        base_opa = LV_OPA_COVER;
    } else if (segment.is_extrusion) {
        line_width = 2;
        base_opa = LV_OPA_90;
    } else {
        line_width = 1;
        base_opa = LV_OPA_60;
    }

    dsc.width = line_width;

    // ======================================================================
    // Z-HEIGHT RAINBOW GRADIENT MAPPING
    // ======================================================================
    // Map Z-height to rainbow gradient: Blue (bottom) → Red (top)
    // This makes embossed features stand out as they'll be warmer colors
    // than surrounding layers at the same Z-height.
    //
    // Gradient stops:
    //   0.00 → Blue   (#0000FF)
    //   0.25 → Cyan   (#00FFFF)
    //   0.50 → Green  (#00FF00)
    //   0.75 → Yellow (#FFFF00)
    //   1.00 → Red    (#FF0000)

    // Calculate midpoint Z of segment
    float z_mid = (segment.start.z + segment.end.z) * 0.5f;

    // Normalize Z to [0, 1]
    float z_normalized = (z_mid - z_min_) / (z_max_ - z_min_);
    z_normalized = std::clamp(z_normalized, 0.0f, 1.0f);

    // Map to rainbow gradient
    lv_color_t gradient_color;
    if (z_normalized < 0.25f) {
        // Blue → Cyan (0.0 to 0.25)
        float t = z_normalized / 0.25f;
        gradient_color = lv_color_make(0, static_cast<uint8_t>(255 * t), 255);
    } else if (z_normalized < 0.5f) {
        // Cyan → Green (0.25 to 0.5)
        float t = (z_normalized - 0.25f) / 0.25f;
        gradient_color = lv_color_make(0, 255, static_cast<uint8_t>(255 * (1.0f - t)));
    } else if (z_normalized < 0.75f) {
        // Green → Yellow (0.5 to 0.75)
        float t = (z_normalized - 0.5f) / 0.25f;
        gradient_color = lv_color_make(static_cast<uint8_t>(255 * t), 255, 0);
    } else {
        // Yellow → Red (0.75 to 1.0)
        float t = (z_normalized - 0.75f) / 0.25f;
        gradient_color = lv_color_make(255, static_cast<uint8_t>(255 * (1.0f - t)), 0);
    }

    // Use gradient color (excluded/highlighted objects override with their special color)
    if (is_excluded) {
        dsc.color = color_excluded_;  // Red/orange strikethrough color
    } else if (is_highlighted) {
        dsc.color = color_highlighted_;
    } else {
        dsc.color = gradient_color;
    }

    // ======================================================================
    // DEPTH CUEING via OPACITY
    // ======================================================================
    // Far segments fade to transparent for depth perception
    // Quadratic falloff: depth=0 (near) → opacity_factor=1.0,
    //                    depth=1 (far)  → opacity_factor=0.0
    float opacity_factor = 1.0f - (normalized_depth * normalized_depth);

    // Map to opacity range: near=255 (full opacity), far=40 (more transparent)
    lv_opa_t depth_opa =
        LV_OPA_40 + static_cast<lv_opa_t>((LV_OPA_COVER - LV_OPA_40) * opacity_factor);

    // Combine base opacity with depth-based opacity
    dsc.opa = (base_opa * depth_opa) / 255;

    // Apply brightness factor if configured
    if (brightness_factor_ != 1.0f) {
        uint8_t r = std::clamp(static_cast<int>(dsc.color.red * brightness_factor_), 0, 255);
        uint8_t g = std::clamp(static_cast<int>(dsc.color.green * brightness_factor_), 0, 255);
        uint8_t b = std::clamp(static_cast<int>(dsc.color.blue * brightness_factor_), 0, 255);
        dsc.color = lv_color_make(r, g, b);
    }

    // Apply global opacity multiplier
    dsc.opa = static_cast<lv_opa_t>(std::clamp((dsc.opa * global_opacity_) / 255, 0, 255));

    return dsc;
}

void GCodeRenderer::draw_line(lv_layer_t* layer, const glm::vec2& p1, const glm::vec2& p2,
                              const lv_draw_line_dsc_t& dsc) {
    // LVGL 9.4 API: points are now embedded in the descriptor
    lv_draw_line_dsc_t dsc_copy = dsc;
    dsc_copy.p1.x = p1.x;
    dsc_copy.p1.y = p1.y;
    dsc_copy.p2.x = p2.x;
    dsc_copy.p2.y = p2.y;

    lv_draw_line(layer, &dsc_copy);
}

std::optional<std::string> GCodeRenderer::pick_object(const glm::vec2& screen_pos,
                                                      const ParsedGCodeFile& gcode,
                                                      const GCodeCamera& camera) const {
    // Segment-based picking: find closest rendered segment to click point
    // This works even without EXCLUDE_OBJECT metadata by checking segment.object_name

    glm::mat4 transform = camera.get_view_projection_matrix();
    float closest_distance = std::numeric_limits<float>::max();
    std::optional<std::string> picked_object;

    const float PICK_THRESHOLD = 15.0f; // pixels - how close to segment to register click

    // Iterate through visible layers
    int layer_start = options_.layer_start;
    int layer_end =
        (options_.layer_end < 0 || options_.layer_end >= static_cast<int>(gcode.layers.size()))
            ? static_cast<int>(gcode.layers.size()) - 1
            : options_.layer_end;

    for (int layer_idx = layer_start; layer_idx <= layer_end; ++layer_idx) {
        if (layer_idx < 0 || layer_idx >= static_cast<int>(gcode.layers.size())) {
            continue;
        }

        const Layer& layer = gcode.layers[layer_idx];

        for (const auto& segment : layer.segments) {
            // Only check segments that would be rendered
            if (!should_render_segment(segment)) {
                continue;
            }

            // Skip segments without object names
            if (segment.object_name.empty()) {
                continue;
            }

            // Project segment endpoints to screen space
            auto start_screen = project_to_screen(segment.start, transform);
            auto end_screen = project_to_screen(segment.end, transform);

            // Skip if either endpoint is off-screen
            if (!start_screen || !end_screen) {
                continue;
            }

            // Calculate distance from click point to line segment
            glm::vec2 v = *end_screen - *start_screen;
            glm::vec2 w = screen_pos - *start_screen;

            // Project click onto line segment (clamped to [0,1])
            float segment_length_sq = glm::dot(v, v);
            float t = (segment_length_sq > 0.0001f)
                          ? std::clamp(glm::dot(w, v) / segment_length_sq, 0.0f, 1.0f)
                          : 0.0f;

            // Closest point on segment to click
            glm::vec2 closest_point = *start_screen + t * v;

            // Distance from click to closest point on segment
            float dist = glm::length(screen_pos - closest_point);

            // Update if this is the closest segment within threshold
            if (dist < PICK_THRESHOLD && dist < closest_distance) {
                closest_distance = dist;
                picked_object = segment.object_name;
            }
        }
    }

    return picked_object;
}

} // namespace gcode
