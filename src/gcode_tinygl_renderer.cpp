// Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_tinygl_renderer.h"

#ifdef ENABLE_TINYGL_3D

#include "config.h"
#include "runtime_config.h"

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
    geometry_builder_->set_debug_face_colors(false);   // Production: use actual G-code colors

    // Initialize 50% checkerboard stipple pattern for ghost layer transparency
    // Pattern is 32x32 bits = 128 bytes, alternating 0xAA/0x55 rows
    for (int row = 0; row < 32; ++row) {
        uint8_t byte_val = (row % 2 == 0) ? 0xAA : 0x55;
        for (int col = 0; col < 4; ++col) {
            ghost_stipple_pattern_[row * 4 + col] = byte_val;
        }
    }

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

void GCodeTinyGLRenderer::set_specular(float intensity, float shininess) {
    specular_intensity_ = intensity;
    specular_shininess_ = shininess;
    // Material properties will be applied on next render
}

void GCodeTinyGLRenderer::set_debug_face_colors(bool enable) {
    spdlog::debug("Setting debug face colors: {}", enable ? "ENABLED" : "disabled");
    geometry_builder_->set_debug_face_colors(enable);

    // Geometry needs rebuild to apply new coloring
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
        spdlog::warn("TinyGL init_tinygl() called but zbuffer_ already exists!");
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

    // Use the ACTUAL ZBuffer dimensions (TinyGL may round for alignment)
    // This is CRITICAL - using wrong dimensions causes massive rendering distortion!
    viewport_width_ = zb->xsize;
    viewport_height_ = zb->ysize;

    glInit(zb);

    // Set up OpenGL state with ACTUAL dimensions
    glViewport(0, 0, viewport_width_, viewport_height_);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

    // Set shading model from config (default: phong)
    Config* cfg = Config::get_instance();
    std::string shading_model = cfg->get<std::string>("/gcode_viewer/shading_model", "phong");

    if (shading_model == "flat") {
        glShadeModel(GL_FLAT);
        spdlog::debug("G-code renderer using GL_FLAT shading");
    } else if (shading_model == "smooth") {
        glShadeModel(GL_SMOOTH);
        spdlog::debug("G-code renderer using GL_SMOOTH (Gouraud) shading");
    } else if (shading_model == "phong") {
        glShadeModel(GL_PHONG);
        spdlog::debug("G-code renderer using GL_PHONG (per-pixel) shading");
    } else {
        spdlog::warn("Unknown shading model '{}', defaulting to phong", shading_model);
        glShadeModel(GL_PHONG);
    }

    // Set material properties (use current specular settings)
    // GL_COLOR_MATERIAL only controls ambient/diffuse, so we must set specular separately
    GLfloat specular[] = {specular_intensity_, specular_intensity_, specular_intensity_, 1.0f};
    GLfloat no_emission[] = {0.0f, 0.0f, 0.0f, 1.0f};
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, specular);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, no_emission);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, specular_shininess_);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Enable specular lighting calculations (TinyGL-specific, disabled by default)
    glSetEnableSpecular(1);

    // Setup lighting with current material properties
    setup_lighting();
}

void GCodeTinyGLRenderer::shutdown_tinygl() {
    if (zbuffer_) {
        glClose();
        ZB_close(static_cast<ZBuffer*>(zbuffer_));
        zbuffer_ = nullptr;
        framebuffer_ = nullptr; // ZB_close frees the framebuffer
    }
}

void GCodeTinyGLRenderer::setup_lighting() {
    // Lighting setup matching OrcaSlicer's gouraud_light shader for consistent appearance
    // See: OrcaSlicer/resources/shaders/110/gouraud_light.vs
    //
    // Key differences from previous setup:
    // - Lower ambient (0.3 vs 0.6) for better depth perception
    // - Directional lights (w=0.0) instead of positional for consistency
    // - Intensity correction factor (0.6) to prevent oversaturation
    // - Matches OrcaSlicer's exact light directions

    // Lower ambient for better shadow contrast
    // Higher diffuse intensity for stronger directional lighting
    constexpr float INTENSITY_CORRECTION = 0.8f; // Increased from 0.6 for brighter diffuse
    constexpr float INTENSITY_AMBIENT = 0.2f;    // Reduced from 0.3 for darker shadows

    // Top light: Primary light from above-right (matches OrcaSlicer LIGHT_TOP_DIR)
    // Direction: normalized(-0.6, 0.6, 1.0) = (-0.4574957, 0.4574957, 0.7624929)
    glEnable(GL_LIGHT0);
    GLfloat light_top_dir[] = {-0.4574957f, 0.4574957f, 0.7624929f, 0.0f}; // Directional
    GLfloat light_top_ambient[] = {INTENSITY_AMBIENT, INTENSITY_AMBIENT, INTENSITY_AMBIENT, 1.0f};
    GLfloat light_top_diffuse[] = {0.8f * INTENSITY_CORRECTION, 0.8f * INTENSITY_CORRECTION,
                                   0.8f * INTENSITY_CORRECTION, 1.0f};
    GLfloat light_top_specular[] = {1.0f, 1.0f, 1.0f,
                                    1.0f}; // Enable specular (material controls intensity)

    glLightfv(GL_LIGHT0, GL_POSITION, light_top_dir);
    glLightfv(GL_LIGHT0, GL_AMBIENT, light_top_ambient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, light_top_diffuse);
    glLightfv(GL_LIGHT0, GL_SPECULAR, light_top_specular);

    // Front light: Fill light from front-right (matches OrcaSlicer LIGHT_FRONT_DIR)
    // Direction: normalized(1.0, 0.2, 1.0) = (0.6985074, 0.1397015, 0.6985074)
    glEnable(GL_LIGHT1);
    GLfloat light_front_dir[] = {0.6985074f, 0.1397015f, 0.6985074f, 0.0f}; // Directional
    GLfloat light_front_ambient[] = {0.0f, 0.0f, 0.0f, 1.0f}; // No ambient from fill light
    GLfloat light_front_diffuse[] = {0.3f * INTENSITY_CORRECTION, 0.3f * INTENSITY_CORRECTION,
                                     0.3f * INTENSITY_CORRECTION, 1.0f};
    GLfloat light_front_specular[] = {0.0f, 0.0f, 0.0f, 1.0f}; // No specular

    glLightfv(GL_LIGHT1, GL_POSITION, light_front_dir);
    glLightfv(GL_LIGHT1, GL_AMBIENT, light_front_ambient);
    glLightfv(GL_LIGHT1, GL_DIFFUSE, light_front_diffuse);
    glLightfv(GL_LIGHT1, GL_SPECULAR, light_front_specular);
}

void GCodeTinyGLRenderer::render_bounding_box(const ParsedGCodeFile& gcode) {
    spdlog::trace("render_bounding_box: {} highlighted objects", highlighted_objects_.size());

    // Only render if we have highlighted objects
    if (highlighted_objects_.empty()) {
        return;
    }

    // Render bounding box for each highlighted object
    for (const auto& object_name : highlighted_objects_) {
        // Find the highlighted object in the G-code data
        auto it = gcode.objects.find(object_name);
        if (it == gcode.objects.end()) {
            spdlog::warn("TinyGL: Cannot render bounding box - object '{}' not found in G-code",
                         object_name);
            continue; // Skip this object
        }

        const GCodeObject& obj = it->second;
        const AABB& bbox = obj.bounding_box;

        // IMPORTANT: The object AABB is in original G-code coordinates, but the rendered
        // geometry has been quantized and dequantized using expanded bounds (to account for
        // tube width). We need to transform the AABB through the same quantization process
        // to get it into the same coordinate space as the rendered geometry.

        if (!geometry_) {
            spdlog::warn("TinyGL: No geometry available for bounding box quantization");
            return;
        }

        const auto& quant = geometry_->quantization;

        // Quantize the bounding box min/max
        auto qmin = quant.quantize_vec3(bbox.min);
        auto qmax = quant.quantize_vec3(bbox.max);

        // Dequantize back to get coordinates in the same space as rendered geometry
        glm::vec3 bbox_min = quant.dequantize_vec3(qmin);
        glm::vec3 bbox_max = quant.dequantize_vec3(qmax);

        spdlog::debug("TinyGL: Object '{}' AABB (original): min=({:.2f},{:.2f},{:.2f}) "
                      "max=({:.2f},{:.2f},{:.2f})",
                      object_name, bbox.min.x, bbox.min.y, bbox.min.z, bbox.max.x, bbox.max.y,
                      bbox.max.z);
        spdlog::debug("TinyGL: Object AABB (transformed): min=({:.2f},{:.2f},{:.2f}) "
                      "max=({:.2f},{:.2f},{:.2f})",
                      bbox_min.x, bbox_min.y, bbox_min.z, bbox_max.x, bbox_max.y, bbox_max.z);

        // Use material emission for bright white lines
        glDisable(GL_DEPTH_TEST); // Draw on top
        GLfloat white[] = {1.0f, 1.0f, 1.0f, 1.0f};

        // Set bright white emission for visibility (glowing lines)
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, white);
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, white);

        // Draw complete wireframe cube using GL_LINES (12 edges total)
        glBegin(GL_LINES);

        // Bottom rectangle (4 edges at Z = min)
        glVertex3f(bbox_min.x, bbox_min.y, bbox_min.z);
        glVertex3f(bbox_max.x, bbox_min.y, bbox_min.z);

        glVertex3f(bbox_max.x, bbox_min.y, bbox_min.z);
        glVertex3f(bbox_max.x, bbox_max.y, bbox_min.z);

        glVertex3f(bbox_max.x, bbox_max.y, bbox_min.z);
        glVertex3f(bbox_min.x, bbox_max.y, bbox_min.z);

        glVertex3f(bbox_min.x, bbox_max.y, bbox_min.z);
        glVertex3f(bbox_min.x, bbox_min.y, bbox_min.z);

        // Top rectangle (4 edges at Z = max)
        glVertex3f(bbox_min.x, bbox_min.y, bbox_max.z);
        glVertex3f(bbox_max.x, bbox_min.y, bbox_max.z);

        glVertex3f(bbox_max.x, bbox_min.y, bbox_max.z);
        glVertex3f(bbox_max.x, bbox_max.y, bbox_max.z);

        glVertex3f(bbox_max.x, bbox_max.y, bbox_max.z);
        glVertex3f(bbox_min.x, bbox_max.y, bbox_max.z);

        glVertex3f(bbox_min.x, bbox_max.y, bbox_max.z);
        glVertex3f(bbox_min.x, bbox_min.y, bbox_max.z);

        // Vertical edges (4 edges connecting bottom to top)
        glVertex3f(bbox_min.x, bbox_min.y, bbox_min.z);
        glVertex3f(bbox_min.x, bbox_min.y, bbox_max.z);

        glVertex3f(bbox_max.x, bbox_min.y, bbox_min.z);
        glVertex3f(bbox_max.x, bbox_min.y, bbox_max.z);

        glVertex3f(bbox_max.x, bbox_max.y, bbox_min.z);
        glVertex3f(bbox_max.x, bbox_max.y, bbox_max.z);

        glVertex3f(bbox_min.x, bbox_max.y, bbox_min.z);
        glVertex3f(bbox_min.x, bbox_max.y, bbox_max.z);

        glEnd();

        // Reset material state to defaults (no emission, gray diffuse)
        // This prevents the white material from affecting the next frame's geometry
        GLfloat no_emission[] = {0.0f, 0.0f, 0.0f, 1.0f};
        GLfloat default_gray[] = {0.8f, 0.8f, 0.8f, 1.0f};
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, no_emission);
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, default_gray);

        // Re-enable depth test for subsequent geometry
        glEnable(GL_DEPTH_TEST);

        spdlog::debug("TinyGL: Bounding box rendered for object '{}'", object_name);
    } // end for each highlighted object
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

    // Use extrusion width from G-code metadata if available
    // Prefer comment-based widths (more reliable) over calculated per-segment widths
    if (gcode.perimeter_extrusion_width_mm > 0.0f) {
        // Use perimeter width (most common for visual appearance)
        extrusion_width_ = gcode.perimeter_extrusion_width_mm;
        geometry_builder_->set_extrusion_width(extrusion_width_);
        spdlog::info("Using perimeter extrusion width from G-code: {:.2f}mm", extrusion_width_);
    } else if (gcode.extrusion_width_mm > 0.0f) {
        // Use default width if perimeter not specified
        extrusion_width_ = gcode.extrusion_width_mm;
        geometry_builder_->set_extrusion_width(extrusion_width_);
        spdlog::info("Using extrusion width from G-code: {:.2f}mm", extrusion_width_);
    } else {
        // Keep current width (user-set or default)
        spdlog::info("No extrusion width in G-code, using current: {:.2f}mm", extrusion_width_);
    }

    // Set layer height from G-code metadata for correct tube proportions
    geometry_builder_->set_layer_height(gcode.layer_height_mm);
    spdlog::info("Using layer height from G-code: {:.2f}mm", gcode.layer_height_mm);

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

    // Configure highlighted objects (segments will be brightened)
    geometry_builder_->set_highlighted_objects(highlighted_objects_);
    if (!highlighted_objects_.empty()) {
        spdlog::debug("Highlighting {} objects", highlighted_objects_.size());
    }

    // Configure multi-color support: pass tool color palette from parsed G-code
    if (!gcode.tool_color_palette.empty()) {
        geometry_builder_->set_tool_color_palette(gcode.tool_color_palette);
        spdlog::info("Multi-color print detected: {} tool colors", gcode.tool_color_palette.size());
    }

    // Build optimized ribbon geometry
    geometry_ = geometry_builder_->build(filtered_gcode, simplification_);
    current_gcode_filename_ = gcode.filename;

    const auto& stats = geometry_builder_->last_stats();
    spdlog::info("Geometry built: {} vertices, {} triangles, {:.2f} MB", stats.vertices_generated,
                 stats.triangles_generated, stats.memory_bytes / 1024.0 / 1024.0);
}

void GCodeTinyGLRenderer::set_prebuilt_geometry(std::unique_ptr<RibbonGeometry> geometry,
                                                const std::string& filename) {
    if (!geometry) {
        spdlog::warn("GCodeTinyGLRenderer::set_prebuilt_geometry called with null geometry");
        return;
    }

    spdlog::info("[GCode::Renderer] set_prebuilt_geometry: incoming max_layer_index={}",
                 geometry->max_layer_index);

    geometry_ = std::move(*geometry); // Move the value from unique_ptr into optional
    current_gcode_filename_ = filename;

    spdlog::info("[GCode::Renderer] Pre-built geometry set: {} vertices, {} triangles (extrusion: "
                 "{}, travel: {}), max_layer_index={}",
                 geometry_->vertices.size(),
                 geometry_->extrusion_triangle_count + geometry_->travel_triangle_count,
                 geometry_->extrusion_triangle_count, geometry_->travel_triangle_count,
                 geometry_->max_layer_index);
}

void GCodeTinyGLRenderer::render_geometry(const GCodeCamera& camera) {
    if (!geometry_) {
        return; // No geometry to render
    }

    // Clear buffers
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Apply current material properties (in case they changed)
    GLfloat specular[] = {specular_intensity_, specular_intensity_, specular_intensity_, 1.0f};
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, specular);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, specular_shininess_);

    // Set up matrices from camera
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(glm::value_ptr(camera.get_projection_matrix()));

    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(glm::value_ptr(camera.get_view_matrix()));

    // Check if we need two-pass ghost layer rendering
    if (ghost_mode_enabled_ && current_progress_layer_ >= 0 &&
        !geometry_->strip_layer_index.empty()) {
        // Two-pass rendering for print progress visualization
        int max_layer = static_cast<int>(geometry_->max_layer_index);

        // Pass 1: Render solid layers (0 to current_progress_layer_)
        render_layer_range(0, current_progress_layer_, 1.0f);

        // Pass 2: Render ghost layers (current_progress_layer_+1 to max) with mode-specific rendering
        if (current_progress_layer_ < max_layer) {
            float dim_factor = ghost_opacity_ / 255.0f;

            // Apply stipple pattern for ghost layers (if enabled)
            if (ghost_render_mode_ == GhostRenderMode::Stipple) {
                glEnable(GL_POLYGON_STIPPLE);
                glPolygonStipple(ghost_stipple_pattern_);
            }

            render_layer_range(current_progress_layer_ + 1, max_layer, dim_factor);

            // Restore state after ghost pass
            if (ghost_render_mode_ == GhostRenderMode::Stipple) {
                glDisable(GL_POLYGON_STIPPLE);
            }
        }
    } else {
        // Normal rendering - all strips at full brightness
        render_layer_range(0, static_cast<int>(geometry_->max_layer_index), 1.0f);
    }
}

void GCodeTinyGLRenderer::render_layer_range(int start_layer, int end_layer, float dim_factor) {
    if (!geometry_ || geometry_->strips.empty()) {
        return;
    }

    // If we don't have layer tracking data, render all strips
    if (geometry_->strip_layer_index.empty()) {
        // Fallback: render all strips with the given dim factor
        for (size_t i = 0; i < geometry_->strips.size(); ++i) {
            const auto& strip = geometry_->strips[i];

            glBegin(GL_TRIANGLE_STRIP);
            for (int j = 0; j < 4; j++) {
                const auto& vertex = geometry_->vertices[strip[j]];

                // Lookup normal from palette
                const glm::vec3& normal = geometry_->normal_palette[vertex.normal_index];
                glNormal3f(normal.x, normal.y, normal.z);

                // Lookup color from palette and apply dimming
                uint32_t color_rgb = geometry_->color_palette[vertex.color_index];
                uint8_t r = (color_rgb >> 16) & 0xFF;
                uint8_t g = (color_rgb >> 8) & 0xFF;
                uint8_t b = color_rgb & 0xFF;

                glColor3f((r / 255.0f) * dim_factor,
                          (g / 255.0f) * dim_factor,
                          (b / 255.0f) * dim_factor);

                // Dequantize position
                glm::vec3 pos = geometry_->quantization.dequantize_vec3(vertex.position);
                glVertex3f(pos.x, pos.y, pos.z);
            }
            glEnd();
        }
        return;
    }

    // Render only strips in the specified layer range
    for (size_t i = 0; i < geometry_->strips.size(); ++i) {
        // Check if this strip's layer is in range
        uint16_t strip_layer = geometry_->strip_layer_index[i];
        if (static_cast<int>(strip_layer) < start_layer ||
            static_cast<int>(strip_layer) > end_layer) {
            continue; // Skip strips outside the layer range
        }

        const auto& strip = geometry_->strips[i];

        glBegin(GL_TRIANGLE_STRIP);
        for (int j = 0; j < 4; j++) {
            const auto& vertex = geometry_->vertices[strip[j]];

            // Lookup normal from palette
            const glm::vec3& normal = geometry_->normal_palette[vertex.normal_index];
            glNormal3f(normal.x, normal.y, normal.z);

            // Lookup color from palette and apply dimming
            uint32_t color_rgb = geometry_->color_palette[vertex.color_index];
            uint8_t r = (color_rgb >> 16) & 0xFF;
            uint8_t g = (color_rgb >> 8) & 0xFF;
            uint8_t b = color_rgb & 0xFF;

            glColor3f((r / 255.0f) * dim_factor,
                      (g / 255.0f) * dim_factor,
                      (b / 255.0f) * dim_factor);

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
    // Note: Use exact framebuffer dimensions to avoid scaling artifacts
    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
    img_dsc.src = draw_buf_;

    lv_area_t area;
    area.x1 = 0;
    area.y1 = 0;
    area.x2 = viewport_width_ - 1;  // Actual framebuffer width
    area.y2 = viewport_height_ - 1; // Actual framebuffer height

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

    // Render bounding box wireframe for highlighted objects
    spdlog::trace("TinyGL render: {} highlighted objects, gcode.objects.size()={}",
                  highlighted_objects_.size(), gcode.objects.size());
    render_bounding_box(gcode);

    // Draw to LVGL
    draw_to_lvgl(layer);

    // Draw camera debug info overlay (if verbose mode OR camera params set via CLI)
    const RuntimeConfig& config = get_runtime_config();
    bool show_debug_overlay = spdlog::get_level() <= spdlog::level::debug ||
                              config.gcode_camera_azimuth_set ||
                              config.gcode_camera_elevation_set || config.gcode_camera_zoom_set;
    if (show_debug_overlay) {
        char debug_text[128];
        snprintf(debug_text, sizeof(debug_text), "Az: %.1f° El: %.1f° Zoom: %.1fx",
                 camera.get_azimuth(), camera.get_elevation(), camera.get_zoom_level());

        lv_draw_label_dsc_t label_dsc;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = lv_color_make(255, 255, 255); // White text
        label_dsc.text = debug_text;
        label_dsc.text_local = true;

        lv_area_t text_area;
        text_area.x1 = 10; // 10px from left
        text_area.y1 = 10; // 10px from top
        text_area.x2 = viewport_width_ - 1;
        text_area.y2 = 40; // Room for one line of text

        lv_draw_label(layer, &label_dsc, &text_area);
    }
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
    // Convert single object to set and delegate to multi-object version
    std::unordered_set<std::string> objects;
    if (!name.empty()) {
        objects.insert(name);
    }
    set_highlighted_objects(objects);
}

void GCodeTinyGLRenderer::set_highlighted_objects(const std::unordered_set<std::string>& names) {
    // Only rebuild if the highlighted objects actually changed
    if (highlighted_objects_ != names) {
        highlighted_objects_ = names;
        // Trigger geometry rebuild to apply/remove highlighting
        geometry_.reset();
        current_gcode_filename_.clear();
        spdlog::debug("TinyGL: Highlighted objects changed ({} selected), geometry will rebuild",
                      names.size());
    }
}

void GCodeTinyGLRenderer::set_excluded_objects(const std::unordered_set<std::string>& names) {
    // Only rebuild if the excluded objects actually changed
    if (excluded_objects_ != names) {
        excluded_objects_ = names;
        // Trigger geometry rebuild to apply excluded object coloring
        geometry_.reset();
        current_gcode_filename_.clear();
        spdlog::debug("TinyGL: Excluded objects changed ({} excluded), geometry will rebuild",
                      names.size());
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

void GCodeTinyGLRenderer::set_print_progress_layer(int current_layer) {
    current_progress_layer_ = current_layer;
    ghost_mode_enabled_ = (current_layer >= 0);

    if (ghost_mode_enabled_) {
        spdlog::debug("[GCode::Renderer] Ghost mode enabled: progress layer = {}", current_layer);
    } else {
        spdlog::debug("[GCode::Renderer] Ghost mode disabled");
    }
}

void GCodeTinyGLRenderer::set_ghost_opacity(lv_opa_t opacity) {
    ghost_opacity_ = opacity;
    spdlog::debug("[GCode::Renderer] Ghost opacity set to {} ({:.0f}%)",
                  opacity, (opacity / 255.0f) * 100.0f);
}

void GCodeTinyGLRenderer::set_ghost_render_mode(GhostRenderMode mode) {
    ghost_render_mode_ = mode;
    const char* mode_name = (mode == GhostRenderMode::Stipple) ? "Stipple" : "Dimmed";
    spdlog::debug("[GCode::Renderer] Ghost render mode set to {}", mode_name);
}

int GCodeTinyGLRenderer::get_max_layer_index() const {
    if (!geometry_) {
        return -1;
    }
    return static_cast<int>(geometry_->max_layer_index);
}

std::optional<std::string> GCodeTinyGLRenderer::pick_object(const glm::vec2& screen_pos,
                                                            const ParsedGCodeFile& gcode,
                                                            const GCodeCamera& camera) const {
    // Segment-based picking: find closest rendered segment to click point
    // Reuses same algorithm as GCodeRenderer for consistency

    glm::mat4 transform = camera.get_view_projection_matrix();
    float closest_distance = std::numeric_limits<float>::max();
    std::optional<std::string> picked_object;

    const float PICK_THRESHOLD = 15.0f; // pixels - how close to segment to register click

    // Determine visible layer range
    int layer_start = layer_start_;
    int layer_end = (layer_end_ < 0 || layer_end_ >= static_cast<int>(gcode.layers.size()))
                        ? static_cast<int>(gcode.layers.size()) - 1
                        : layer_end_;

    for (int layer_idx = layer_start; layer_idx <= layer_end; ++layer_idx) {
        if (layer_idx < 0 || layer_idx >= static_cast<int>(gcode.layers.size())) {
            continue;
        }

        const Layer& layer = gcode.layers[layer_idx];

        for (const auto& segment : layer.segments) {
            // Only check extrusion segments (TinyGL doesn't render travels)
            if (!segment.is_extrusion || !show_extrusions_) {
                continue;
            }

            // Skip segments without object names
            if (segment.object_name.empty()) {
                continue;
            }

            // Project segment endpoints to screen space
            // Use camera's projection helper (same as GCodeRenderer)
            glm::vec4 start_clip = transform * glm::vec4(segment.start, 1.0f);
            glm::vec4 end_clip = transform * glm::vec4(segment.end, 1.0f);

            // Perspective divide
            if (std::abs(start_clip.w) < 0.0001f || std::abs(end_clip.w) < 0.0001f) {
                continue;
            }

            glm::vec3 start_ndc = glm::vec3(start_clip) / start_clip.w;
            glm::vec3 end_ndc = glm::vec3(end_clip) / end_clip.w;

            // Clip to NDC bounds [-1, 1]
            if (start_ndc.x < -1.0f || start_ndc.x > 1.0f || start_ndc.y < -1.0f ||
                start_ndc.y > 1.0f || end_ndc.x < -1.0f || end_ndc.x > 1.0f || end_ndc.y < -1.0f ||
                end_ndc.y > 1.0f) {
                continue; // Simplified: skip segments with any point outside view
            }

            // NDC to screen space
            glm::vec2 start_screen((start_ndc.x + 1.0f) * 0.5f * viewport_width_,
                                   (1.0f - start_ndc.y) * 0.5f * viewport_height_);
            glm::vec2 end_screen((end_ndc.x + 1.0f) * 0.5f * viewport_width_,
                                 (1.0f - end_ndc.y) * 0.5f * viewport_height_);

            // Calculate distance from click point to line segment
            glm::vec2 v = end_screen - start_screen;
            glm::vec2 w = screen_pos - start_screen;

            // Project click onto line segment (clamped to [0,1])
            float segment_length_sq = glm::dot(v, v);
            float t = (segment_length_sq > 0.0001f)
                          ? std::clamp(glm::dot(w, v) / segment_length_sq, 0.0f, 1.0f)
                          : 0.0f;

            // Closest point on segment to click
            glm::vec2 closest_point = start_screen + t * v;

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

GCodeTinyGLRenderer::RenderingOptions GCodeTinyGLRenderer::get_options() const {
    // Return first highlighted object for compatibility (RenderingOptions uses single string)
    std::string first_object = highlighted_objects_.empty() ? "" : *highlighted_objects_.begin();
    return {show_extrusions_, show_travels_, layer_start_, layer_end_, first_object};
}

} // namespace gcode

#endif // ENABLE_TINYGL_3D
