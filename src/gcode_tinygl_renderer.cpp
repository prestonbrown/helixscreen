// Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_tinygl_renderer.h"

#ifdef ENABLE_TINYGL_3D

#include <spdlog/spdlog.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// TinyGL headers
#include <GL/gl.h>
extern "C" {
#include <zbuffer.h>
}

namespace gcode {

GCodeTinyGLRenderer::GCodeTinyGLRenderer()
    : geometry_builder_(std::make_unique<GeometryBuilder>()) {
    // Set default configuration
    simplification_.tolerance_mm = 0.15f;
    simplification_.min_segment_length_mm = 0.01f;
    simplification_.enable_merging = true;

    geometry_builder_->set_smooth_shading(smooth_shading_);
    geometry_builder_->set_extrusion_width(extrusion_width_);
    geometry_builder_->set_use_height_gradient(false); // Use actual G-code filament colors

    spdlog::debug("GCodeTinyGLRenderer created");
}

GCodeTinyGLRenderer::~GCodeTinyGLRenderer() {
    shutdown_tinygl();

    if (draw_buf_) {
        lv_draw_buf_destroy(draw_buf_);
        draw_buf_ = nullptr;
    }

    spdlog::debug("GCodeTinyGLRenderer destroyed");
}

void GCodeTinyGLRenderer::set_viewport_size(int width, int height) {
    if (width == viewport_width_ && height == viewport_height_) {
        return; // No change
    }

    viewport_width_ = width;
    viewport_height_ = height;

    // Reinitialize TinyGL with new size
    shutdown_tinygl();

    spdlog::debug("TinyGL viewport resized to {}x{}", width, height);
}

void GCodeTinyGLRenderer::set_filament_color(const std::string& hex_color) {
    geometry_builder_->set_filament_color(hex_color);
    geometry_builder_->set_use_height_gradient(false);
}

void GCodeTinyGLRenderer::set_smooth_shading(bool enable) {
    smooth_shading_ = enable;
    geometry_builder_->set_smooth_shading(enable);

    // Geometry needs rebuild if it exists
    if (geometry_) {
        geometry_.reset();
        current_gcode_filename_.clear();
    }
}

void GCodeTinyGLRenderer::set_extrusion_width(float width_mm) {
    extrusion_width_ = width_mm;
    geometry_builder_->set_extrusion_width(width_mm);

    // Geometry needs rebuild
    if (geometry_) {
        geometry_.reset();
        current_gcode_filename_.clear();
    }
}

void GCodeTinyGLRenderer::set_simplification_tolerance(float tolerance_mm) {
    simplification_.tolerance_mm = tolerance_mm;

    // Geometry needs rebuild
    if (geometry_) {
        geometry_.reset();
        current_gcode_filename_.clear();
    }
}

size_t GCodeTinyGLRenderer::get_memory_usage() const {
    return geometry_ ? geometry_->memory_usage() : 0;
}

size_t GCodeTinyGLRenderer::get_triangle_count() const {
    return geometry_ ? (geometry_->extrusion_triangle_count + geometry_->travel_triangle_count) : 0;
}

void GCodeTinyGLRenderer::init_tinygl() {
    if (zbuffer_) {
        return; // Already initialized
    }

    // Initialize TinyGL (it allocates its own framebuffer)
    ZBuffer* zb = ZB_open(viewport_width_, viewport_height_, ZB_MODE_RGBA, 0);
    if (!zb) {
        spdlog::error("Failed to initialize TinyGL");
        return;
    }

    zbuffer_ = zb; // Store as void*

    // Get framebuffer pointer from ZBuffer
    framebuffer_ = (unsigned int*)zb->pbuf;

    // Verify the actual ZBuffer dimensions
    spdlog::warn("TinyGL ZBuffer actual size: {}x{} (requested: {}x{})",
                 zb->xsize, zb->ysize, viewport_width_, viewport_height_);

    glInit(zb);

    // Set up OpenGL state
    glViewport(0, 0, viewport_width_, viewport_height_);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    setup_lighting();

    spdlog::info("TinyGL INIT: framebuffer={}x{}, aspect={:.3f}",
                 viewport_width_, viewport_height_,
                 (float)viewport_width_ / (float)viewport_height_);
}

void GCodeTinyGLRenderer::shutdown_tinygl() {
    if (zbuffer_) {
        spdlog::info("TinyGL SHUTDOWN: releasing {}x{} framebuffer",
                     viewport_width_, viewport_height_);
        glClose();
        ZB_close(static_cast<ZBuffer*>(zbuffer_));
        zbuffer_ = nullptr;
        framebuffer_ = nullptr; // ZB_close frees the framebuffer
    }
}

void GCodeTinyGLRenderer::setup_lighting() {
    // Balanced lighting: moderate ambient + diffuse for accurate colors with soft shadows
    glEnable(GL_LIGHT0);

    // Key light: Front-right
    GLfloat key_pos[] = {100.0f, 100.0f, 200.0f, 1.0f};
    GLfloat key_ambient[] = {0.6f, 0.6f, 0.6f, 1.0f};  // Moderate ambient for even base lighting
    GLfloat key_diffuse[] = {0.7f, 0.7f, 0.7f, 1.0f};  // Good diffuse for form definition
    GLfloat key_specular[] = {0.0f, 0.0f, 0.0f, 1.0f}; // No specular highlights

    glLightfv(GL_LIGHT0, GL_POSITION, key_pos);
    glLightfv(GL_LIGHT0, GL_AMBIENT, key_ambient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, key_diffuse);
    glLightfv(GL_LIGHT0, GL_SPECULAR, key_specular);

    // Fill light: Back-left for soft fill
    glEnable(GL_LIGHT1);
    GLfloat fill_pos[] = {-80.0f, -80.0f, 100.0f, 1.0f};
    GLfloat fill_ambient[] = {0.0f, 0.0f, 0.0f, 1.0f};
    GLfloat fill_diffuse[] = {0.5f, 0.5f, 0.5f, 1.0f}; // Moderate fill
    GLfloat fill_specular[] = {0.0f, 0.0f, 0.0f, 1.0f};

    glLightfv(GL_LIGHT1, GL_POSITION, fill_pos);
    glLightfv(GL_LIGHT1, GL_AMBIENT, fill_ambient);
    glLightfv(GL_LIGHT1, GL_DIFFUSE, fill_diffuse);
    glLightfv(GL_LIGHT1, GL_SPECULAR, fill_specular);
}

void GCodeTinyGLRenderer::build_geometry(const ParsedGCodeFile& gcode) {
    // Check if we need to rebuild (different file or no geometry yet)
    if (geometry_ && current_gcode_filename_ == gcode.filename) {
        return; // Geometry already built for this file
    }

    spdlog::info("Building TinyGL geometry for {}", gcode.filename);

    // Use filament color from G-code metadata if available, otherwise use default
    if (!gcode.filament_color_hex.empty()) {
        spdlog::info("Using filament color from G-code: {}", gcode.filament_color_hex);
        geometry_builder_->set_filament_color(gcode.filament_color_hex);
    } else {
        spdlog::info("No filament color in G-code, using default: {}",
                     GeometryBuilder::DEFAULT_FILAMENT_COLOR);
        geometry_builder_->set_filament_color(GeometryBuilder::DEFAULT_FILAMENT_COLOR);
    }

    // Apply layer filtering and travel/extrusion filtering
    ParsedGCodeFile filtered_gcode = gcode; // Copy the file

    // Filter layers if range is specified
    if (layer_start_ > 0 || layer_end_ >= 0) {
        std::vector<Layer> filtered_layers;
        int end = (layer_end_ < 0) ? gcode.layers.size()
                                   : std::min((size_t)layer_end_ + 1, gcode.layers.size());

        for (int i = layer_start_; i < end && i < (int)gcode.layers.size(); i++) {
            filtered_layers.push_back(gcode.layers[i]);
        }

        filtered_gcode.layers = std::move(filtered_layers);
        spdlog::debug("Layer filtering: showing layers {} to {} ({} layers)", layer_start_, end - 1,
                      filtered_gcode.layers.size());
    }

    // Filter travel/extrusion moves
    if (!show_travels_ || !show_extrusions_) {
        for (auto& layer : filtered_gcode.layers) {
            std::vector<ToolpathSegment> filtered_segments;
            for (const auto& segment : layer.segments) {
                if (segment.is_extrusion && !show_extrusions_)
                    continue;
                if (!segment.is_extrusion && !show_travels_)
                    continue;
                filtered_segments.push_back(segment);
            }
            layer.segments = std::move(filtered_segments);
        }
        spdlog::debug("Move filtering: travels={}, extrusions={}", show_travels_, show_extrusions_);
    }

    // Build optimized ribbon geometry
    geometry_ = geometry_builder_->build(filtered_gcode, simplification_);
    current_gcode_filename_ = gcode.filename;

    const auto& stats = geometry_builder_->last_stats();
    spdlog::info("Geometry built: {} vertices, {} triangles, {:.2f} MB", stats.vertices_generated,
                 stats.triangles_generated, stats.memory_bytes / 1024.0 / 1024.0);
}

void GCodeTinyGLRenderer::render_geometry(const GCodeCamera& camera) {
    if (!geometry_) {
        return; // No geometry to render
    }

    // Clear framebuffer
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Set up matrices from camera
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(glm::value_ptr(camera.get_projection_matrix()));

    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(glm::value_ptr(camera.get_view_matrix()));

    // Render triangle strips
    static bool logged_once = false;
    for (const auto& strip : geometry_->strips) {
        glBegin(GL_TRIANGLE_STRIP);

        for (int i = 0; i < 4; i++) {
            const auto& vertex = geometry_->vertices[strip[i]];

            // Lookup normal from palette
            const glm::vec3& normal = geometry_->normal_palette[vertex.normal_index];
            glNormal3f(normal.x, normal.y, normal.z);

            // Lookup color from palette and unpack RGB
            uint32_t color_rgb = geometry_->color_palette[vertex.color_index];
            uint8_t r = (color_rgb >> 16) & 0xFF;
            uint8_t g = (color_rgb >> 8) & 0xFF;
            uint8_t b = color_rgb & 0xFF;

            if (!logged_once) {
                spdlog::debug("Rendering: color_palette[{}] = 0x{:06X} = RGB({}, {}, {})",
                              vertex.color_index, color_rgb, r, g, b);
                logged_once = true;
            }

            glColor3f(r / 255.0f, g / 255.0f, b / 255.0f);

            // Dequantize position
            glm::vec3 pos = geometry_->quantization.dequantize_vec3(vertex.position);
            glVertex3f(pos.x, pos.y, pos.z);
        }

        glEnd();
    }
}

void GCodeTinyGLRenderer::draw_to_lvgl(lv_layer_t* layer) {
    if (!framebuffer_) {
        return;
    }

    // Create or recreate LVGL draw buffer if size changed
    if (!draw_buf_ || draw_buf_->header.w != viewport_width_ ||
        draw_buf_->header.h != viewport_height_) {
        if (draw_buf_) {
            lv_draw_buf_destroy(draw_buf_);
        }

        draw_buf_ =
            lv_draw_buf_create(viewport_width_, viewport_height_, LV_COLOR_FORMAT_RGB888, 0);
        if (!draw_buf_) {
            spdlog::error("Failed to create LVGL draw buffer");
            return;
        }
        spdlog::info("TinyGL DRAW: created draw_buf {}x{}, aspect={:.3f}",
                     viewport_width_, viewport_height_,
                     (float)viewport_width_ / (float)viewport_height_);
    }

    // Copy TinyGL framebuffer to LVGL draw buffer
    // TinyGL ZB_MODE_RGBA is actually ABGR format (Alpha-Blue-Green-Red)
    // LVGL uses RGB888, so we need to swap R and B
    uint8_t* dest = (uint8_t*)draw_buf_->data;
    unsigned int* src = framebuffer_;

    for (int i = 0; i < viewport_width_ * viewport_height_; i++) {
        unsigned int pixel = src[i];
        dest[i * 3 + 0] = pixel & 0xFF;         // R (from TinyGL B channel)
        dest[i * 3 + 1] = (pixel >> 8) & 0xFF;  // G (stays the same)
        dest[i * 3 + 2] = (pixel >> 16) & 0xFF; // B (from TinyGL R channel)
    }

    // Draw image to layer
    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
    img_dsc.src = draw_buf_;

    lv_area_t area;
    area.x1 = 0;
    area.y1 = 0;
    area.x2 = viewport_width_ - 1;
    area.y2 = viewport_height_ - 1;

    lv_draw_image(layer, &img_dsc, &area);
}

void GCodeTinyGLRenderer::render(lv_layer_t* layer, const ParsedGCodeFile& gcode,
                                 const GCodeCamera& camera) {
    // Initialize TinyGL if needed
    if (!zbuffer_) {
        init_tinygl();
        if (!zbuffer_) {
            return; // Initialization failed
        }
    }

    // Build geometry if needed
    build_geometry(gcode);

    // Render 3D geometry
    render_geometry(camera);

    // Draw to LVGL
    draw_to_lvgl(layer);
}

// ==============================================
// Rendering Options Implementation
// ==============================================

void GCodeTinyGLRenderer::set_show_travels(bool show) {
    show_travels_ = show;
    // Geometry needs rebuild to include/exclude travels
    geometry_.reset();
    current_gcode_filename_.clear();
}

void GCodeTinyGLRenderer::set_show_extrusions(bool show) {
    show_extrusions_ = show;
    // Geometry needs rebuild to include/exclude extrusions
    geometry_.reset();
    current_gcode_filename_.clear();
}

void GCodeTinyGLRenderer::set_layer_range(int start, int end) {
    layer_start_ = start;
    layer_end_ = end;
    // Geometry needs rebuild for layer filtering
    geometry_.reset();
    current_gcode_filename_.clear();
}

void GCodeTinyGLRenderer::set_highlighted_object(const std::string& name) {
    highlighted_object_ = name;
    // Could implement highlighting without full rebuild in future
    // For now, rebuild to apply highlight color
    if (!name.empty()) {
        geometry_.reset();
        current_gcode_filename_.clear();
    }
}

void GCodeTinyGLRenderer::reset_colors() {
    // Reset to default filament color mode
    geometry_builder_->set_use_height_gradient(false);
    brightness_factor_ = 1.0f;
    global_opacity_ = LV_OPA_100;
}

void GCodeTinyGLRenderer::set_global_opacity(lv_opa_t opacity) {
    global_opacity_ = opacity;
    // Opacity affects final rendering - could be applied during draw_to_lvgl
}

GCodeTinyGLRenderer::RenderingOptions GCodeTinyGLRenderer::get_options() const {
    return {show_extrusions_, show_travels_, layer_start_, layer_end_, highlighted_object_};
}

} // namespace gcode

#endif // ENABLE_TINYGL_3D
