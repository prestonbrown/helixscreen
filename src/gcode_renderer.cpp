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
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

namespace gcode {

GCodeRenderer::GCodeRenderer() {
    // Load colors from theme
    // Note: ui_theme_parse_color requires theme to be initialized
    // If called before theme init, will use fallback colors
    color_extrusion_ = ui_theme_parse_color(lv_xml_get_const("primary"));
    color_travel_ = ui_theme_parse_color(lv_xml_get_const("secondary_light"));
    color_object_boundary_ = ui_theme_parse_color(lv_xml_get_const("accent"));
    color_highlighted_ = ui_theme_parse_color(lv_xml_get_const("success"));
    color_excluded_ = ui_theme_parse_color(lv_xml_get_const("text_disabled"));
}

void GCodeRenderer::set_viewport_size(int width, int height) {
    viewport_width_ = width;
    viewport_height_ = height;
}

void GCodeRenderer::set_options(const RenderOptions &options) {
    options_ = options;
}

void GCodeRenderer::set_show_travels(bool show) {
    options_.show_travels = show;
}

void GCodeRenderer::set_show_extrusions(bool show) {
    options_.show_extrusions = show;
}

void GCodeRenderer::set_highlighted_object(const std::string &name) {
    options_.highlighted_object = name;
}

void GCodeRenderer::set_lod_level(LODLevel level) {
    options_.lod = level;
}

void GCodeRenderer::set_layer_range(int start, int end) {
    options_.layer_start = start;
    options_.layer_end = end;
}

void GCodeRenderer::render(lv_layer_t *layer,
                           const ParsedGCodeFile &gcode,
                           const GCodeCamera &camera) {
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

    // Determine layer range
    int start_layer = options_.layer_start;
    int end_layer = (options_.layer_end >= 0)
                       ? std::min(options_.layer_end, static_cast<int>(gcode.layers.size()) - 1)
                       : static_cast<int>(gcode.layers.size()) - 1;

    start_layer = std::clamp(start_layer, 0, static_cast<int>(gcode.layers.size()) - 1);

    // Render object boundaries if enabled
    if (options_.show_object_bounds) {
        for (const auto &[name, obj] : gcode.objects) {
            render_object_boundary(layer, obj, transform);
        }
    }

    // Render layers
    for (int i = start_layer; i <= end_layer; ++i) {
        render_layer(layer, gcode.layers[i], transform);
    }

    spdlog::trace("Rendered {} segments, culled {} segments",
                 segments_rendered_, segments_culled_);
}

void GCodeRenderer::render_layer(lv_layer_t *layer,
                                 const Layer &gcode_layer,
                                 const glm::mat4 &transform) {
    // LOD: Skip segments based on level
    int skip_factor = 1 << static_cast<int>(options_.lod);  // 1, 2, or 4

    for (size_t i = 0; i < gcode_layer.segments.size(); i += skip_factor) {
        const auto &segment = gcode_layer.segments[i];

        if (should_render_segment(segment)) {
            render_segment(layer, segment, transform);
            segments_rendered_++;
        } else {
            segments_culled_++;
        }
    }
}

void GCodeRenderer::render_segment(lv_layer_t *layer,
                                   const ToolpathSegment &segment,
                                   const glm::mat4 &transform) {
    // Project 3D points to 2D screen space
    auto p1_opt = project_to_screen(segment.start, transform);
    auto p2_opt = project_to_screen(segment.end, transform);

    if (!p1_opt || !p2_opt) {
        return;  // Outside view
    }

    glm::vec2 p1 = *p1_opt;
    glm::vec2 p2 = *p2_opt;

    // Clip line to viewport
    if (!clip_line_to_viewport(p1, p2)) {
        return;
    }

    // Get line style and draw
    lv_draw_line_dsc_t dsc = get_line_style(segment);
    draw_line(layer, p1, p2, dsc);
}

void GCodeRenderer::render_object_boundary(lv_layer_t *layer,
                                           const GCodeObject &object,
                                           const glm::mat4 &transform) {
    if (object.polygon.size() < 2) {
        return;
    }

    // Draw polygon outline at Z=0 (print bed level)
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = (object.name == options_.highlighted_object)
                   ? color_highlighted_
                   : color_object_boundary_;
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

std::optional<glm::vec2> GCodeRenderer::project_to_screen(
    const glm::vec3 &world_pos,
    const glm::mat4 &transform) const {

    // Transform to clip space
    glm::vec4 clip_space = transform * glm::vec4(world_pos, 1.0f);

    // Perspective divide
    if (clip_space.w == 0.0f) {
        return std::nullopt;  // Invalid
    }

    glm::vec3 ndc(clip_space.x / clip_space.w,
                 clip_space.y / clip_space.w,
                 clip_space.z / clip_space.w);

    // Frustum culling: Check if in normalized device coordinates [-1, 1]
    if (ndc.x < -1.0f || ndc.x > 1.0f ||
        ndc.y < -1.0f || ndc.y > 1.0f ||
        ndc.z < -1.0f || ndc.z > 1.0f) {
        return std::nullopt;  // Outside view frustum
    }

    // Convert to screen coordinates
    float screen_x = (ndc.x + 1.0f) * 0.5f * viewport_width_;
    float screen_y = (1.0f - ndc.y) * 0.5f * viewport_height_;  // Flip Y

    return glm::vec2(screen_x, screen_y);
}

bool GCodeRenderer::should_render_segment(const ToolpathSegment &segment) const {
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

bool GCodeRenderer::clip_line_to_viewport(glm::vec2 &p1, glm::vec2 &p2) const {
    // Simple Cohen-Sutherland line clipping
    // For now, just check if line is completely outside viewport
    // TODO: Implement proper clipping for partially visible lines

    float min_x = 0.0f;
    float max_x = static_cast<float>(viewport_width_);
    float min_y = 0.0f;
    float max_y = static_cast<float>(viewport_height_);

    // Both points outside on same side = completely outside
    if ((p1.x < min_x && p2.x < min_x) ||
        (p1.x > max_x && p2.x > max_x) ||
        (p1.y < min_y && p2.y < min_y) ||
        (p1.y > max_y && p2.y > max_y)) {
        return false;
    }

    // Simple clamp for now (not perfect but acceptable for Phase 1)
    p1.x = std::clamp(p1.x, min_x, max_x);
    p1.y = std::clamp(p1.y, min_y, max_y);
    p2.x = std::clamp(p2.x, min_x, max_x);
    p2.y = std::clamp(p2.y, min_y, max_y);

    return true;
}

lv_draw_line_dsc_t GCodeRenderer::get_line_style(const ToolpathSegment &segment) const {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);

    // Determine color
    bool is_highlighted = !options_.highlighted_object.empty() &&
                         segment.object_name == options_.highlighted_object;

    if (is_highlighted) {
        dsc.color = color_highlighted_;
        dsc.width = 3;
        dsc.opa = LV_OPA_COVER;
    } else if (segment.is_extrusion) {
        dsc.color = color_extrusion_;
        dsc.width = 2;
        dsc.opa = LV_OPA_80;
    } else {
        dsc.color = color_travel_;
        dsc.width = 1;
        dsc.opa = LV_OPA_50;
    }

    return dsc;
}

void GCodeRenderer::draw_line(lv_layer_t *layer,
                              const glm::vec2 &p1,
                              const glm::vec2 &p2,
                              const lv_draw_line_dsc_t &dsc) {
    lv_point_precise_t point1 = {p1.x, p1.y};
    lv_point_precise_t point2 = {p2.x, p2.y};

    lv_draw_line(layer, &dsc, &point1, &point2);
}

std::optional<std::string> GCodeRenderer::pick_object(
    const glm::vec2 &screen_pos,
    const ParsedGCodeFile &gcode,
    const GCodeCamera &camera) const {

    // Get ray from screen position
    glm::vec3 ray_dir = camera.screen_to_world_ray(screen_pos);

    // For orthographic projection, ray origin is screen position projected onto Z=0 plane
    // For simplicity, just test against object center points

    glm::mat4 transform = camera.get_view_projection_matrix();
    float closest_distance = std::numeric_limits<float>::max();
    std::optional<std::string> picked_object;

    for (const auto &[name, obj] : gcode.objects) {
        // Project object center to screen
        glm::vec3 center_3d(obj.center.x, obj.center.y, 0.0f);
        auto center_screen = project_to_screen(center_3d, transform);

        if (center_screen) {
            // Calculate distance from touch point to object center
            float dist = glm::length(*center_screen - screen_pos);

            // Pick if within 30 pixel radius and closest
            if (dist < 30.0f && dist < closest_distance) {
                closest_distance = dist;
                picked_object = name;
            }
        }
    }

    return picked_object;
}

} // namespace gcode
