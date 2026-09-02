// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef ENABLE_GLES_3D

#include "gcode_camera.h"
#include "gcode_color_palette.h"
#include "gcode_geometry_builder.h"
#include "gcode_ghost_mode.h"
#include "gcode_parser.h"
#include "gcode_render_memory.h"
#include "gcode_selection_state.h"
#include "gcode_selection_style.h"

#include <lvgl/lvgl.h>

#include <array>
#include <cmath>
#include <glm/glm.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace helix {
namespace gcode {

// ====== Named Constants (rendering parameters) ======

// Specular material defaults (plastic-like sheen)
constexpr float DEFAULT_SPECULAR_INTENSITY = 0.25f;
constexpr float DEFAULT_SPECULAR_SHININESS = 48.0f;

// Specular clamp ranges
constexpr float MIN_SPECULAR_INTENSITY = 0.0f;
constexpr float MAX_SPECULAR_INTENSITY = 1.0f;
constexpr float MIN_SPECULAR_SHININESS = 1.0f;
constexpr float MAX_SPECULAR_SHININESS = 128.0f;

// Lighting intensities
constexpr float CAMERA_LIGHT_INTENSITY = 0.6f;
constexpr float FILL_LIGHT_INTENSITY = 0.2f;
constexpr float AMBIENT_INTENSITY = 0.25f;

// Background color (neutral gray for contrast with light and dark filaments)
constexpr float BACKGROUND_GRAY = 0.45f;
constexpr float BACKGROUND_GRAY_BLUE = 0.47f;

// Default filament color (#26A69A teal)
constexpr glm::vec4 DEFAULT_FILAMENT_COLOR{0.15f, 0.65f, 0.60f, 1.0f};

// Ghost layer default opacity (out of 255)
constexpr uint8_t DEFAULT_GHOST_OPACITY = 5; // ~2% opacity — ghost layers should barely be visible

// Frame-skip epsilon for float comparisons
constexpr float ANGLE_EPSILON = 1e-5f;
constexpr float ZOOM_EPSILON = 1e-3f;

/// Return type for get_options()
struct RenderingOptions {
    bool show_extrusions = true;
    bool show_travels = false;
    int layer_start = -1;
    int layer_end = -1;
    std::string highlighted_object;
};

// ====== RAII Wrappers for GL Resource Handles ======
// Prevent resource leaks by tying GL object lifetime to C++ scope.
// These are lightweight (just a GLuint), movable, non-copyable.

struct GLBufferHandle {
    unsigned int id = 0;
    GLBufferHandle() = default;
    explicit GLBufferHandle(unsigned int existing_id) : id(existing_id) {}
    ~GLBufferHandle();
    GLBufferHandle(const GLBufferHandle&) = delete;
    GLBufferHandle& operator=(const GLBufferHandle&) = delete;
    GLBufferHandle(GLBufferHandle&& o) noexcept : id(o.id) {
        o.id = 0;
    }
    GLBufferHandle& operator=(GLBufferHandle&& o) noexcept {
        std::swap(id, o.id);
        return *this;
    }
    operator unsigned int() const {
        return id;
    }
};

struct GLFramebufferHandle {
    unsigned int id = 0;
    GLFramebufferHandle() = default;
    ~GLFramebufferHandle();
    GLFramebufferHandle(const GLFramebufferHandle&) = delete;
    GLFramebufferHandle& operator=(const GLFramebufferHandle&) = delete;
    GLFramebufferHandle(GLFramebufferHandle&& o) noexcept : id(o.id) {
        o.id = 0;
    }
    GLFramebufferHandle& operator=(GLFramebufferHandle&& o) noexcept {
        std::swap(id, o.id);
        return *this;
    }
    operator unsigned int() const {
        return id;
    }
};

struct GLRenderbufferHandle {
    unsigned int id = 0;
    GLRenderbufferHandle() = default;
    ~GLRenderbufferHandle();
    GLRenderbufferHandle(const GLRenderbufferHandle&) = delete;
    GLRenderbufferHandle& operator=(const GLRenderbufferHandle&) = delete;
    GLRenderbufferHandle(GLRenderbufferHandle&& o) noexcept : id(o.id) {
        o.id = 0;
    }
    GLRenderbufferHandle& operator=(GLRenderbufferHandle&& o) noexcept {
        std::swap(id, o.id);
        return *this;
    }
    operator unsigned int() const {
        return id;
    }
};

/// GPU-accelerated G-code 3D renderer using OpenGL ES 2.0
///
/// Renders to FBO, reads pixels back into lv_draw_buf_t for LVGL compositing.
/// Requires DRM+EGL display backend.
class GCodeGLESRenderer {
  public:
    GCodeGLESRenderer();
    ~GCodeGLESRenderer();

    GCodeGLESRenderer(const GCodeGLESRenderer&) = delete;
    GCodeGLESRenderer& operator=(const GCodeGLESRenderer&) = delete;

    // ====== Main Rendering Interface ======

    void render(lv_layer_t* layer, const ParsedGCodeFile& gcode, const GCodeCamera& camera,
                const lv_area_t* widget_coords);

    void set_viewport_size(int width, int height);
    void set_interaction_mode(bool interacting);
    bool is_interaction_mode() const {
        return interaction_mode_;
    }

    /// True while VBO upload is still in progress (caller should invalidate widget)
    bool is_uploading() const {
        return geometry_ && !geometry_uploaded_;
    }

    /// True if geometry has been set via set_prebuilt_geometry(). Used by the
    /// viewer widget to decide whether a live 2D→3D switch needs an on-demand
    /// geometry build (initial load in 2D mode skips the build).
    bool has_geometry() const {
        return geometry_ != nullptr;
    }

    /// True once a fatal GL error (out-of-memory / invalid-operation) was seen
    /// during a draw batch. The viewer polls this after render() and switches
    /// to the pure-CPU 2D renderer for the rest of the session rather than
    /// risk a driver-side crash on constrained GPUs (e.g. Mali-G31 on CB1).
    /// Sticky: once set it stays set until the renderer is destroyed.
    bool render_failed() const {
        return gl_render_failed_;
    }

    // ====== Color / Material ======

    void set_filament_color(const std::string& hex_color);
    void set_simplification_tolerance(float tolerance_mm);
    void set_specular(float intensity, float shininess);
    void set_debug_face_colors(bool enable);

    // Color setters (lv_color_t interface used by gcode viewer widget)
    void set_extrusion_color(lv_color_t color);
    void set_tool_color_overrides(const std::vector<uint32_t>& ams_colors);

    /// Put the color palette the geometry was BUILT with back.
    ///
    /// Retraction cannot be spelled as another set_tool_color_overrides() call:
    /// that one writes into geometry_->color_palette IN PLACE, because the vertex
    /// data indexes into that array and there is nowhere else for an override to
    /// live. By the time the AMS answer goes empty the slicer's own colors are
    /// already gone from it. This restores them from the snapshot taken on the
    /// first override, and re-uploads.
    ///
    /// No-op when nothing has been overridden on the current geometry.
    void clear_tool_color_overrides();
    void set_travel_color(lv_color_t) {}
    void set_brightness_factor(float) {}

    // ====== Rendering Options ======

    void set_show_travels(bool show);
    void set_show_extrusions(bool show);
    void set_layer_range(int start, int end);
    void set_highlighted_object(const std::string& name);
    void set_highlighted_objects(const std::unordered_set<std::string>& names);
    void set_excluded_objects(const std::unordered_set<std::string>& names);
    void set_global_opacity(lv_opa_t opacity);
    void reset_colors();
    void clear_cached_frame();
    RenderingOptions get_options() const;

    /// The selection palette this renderer resolved from the XML tokens.
    ///
    /// Read-only view of sel_palette_, so the token wiring is observable without
    /// a GL context: the outline colour reaches stroke_selection_rim() and the
    /// bracket colour reaches the wireframe uniforms from here, and both are
    /// wrong in the same silent way if the palette is never resolved.
    const selection::Palette& selection_palette() const {
        return sel_palette_;
    }

    // ====== Object Picking ======

    std::optional<std::string> pick_object(const glm::vec2& screen_pos,
                                           const ParsedGCodeFile& gcode,
                                           const GCodeCamera& camera) const;

    // ====== Ghost Layer / Print Progress ======

    void set_print_progress_layer(int current_layer);
    void set_ghost_opacity(lv_opa_t opacity);
    void set_ghost_render_mode(GhostRenderMode mode);
    void set_content_offset_y(float offset_percent);
    GhostRenderMode get_ghost_render_mode() const {
        return ghost_render_mode_;
    }
    bool is_ghost_mode_enabled() const {
        return progress_layer_ >= 0;
    }
    int get_max_layer_index() const;

    // ====== Async Geometry Loading ======

    void set_prebuilt_geometry(std::unique_ptr<RibbonGeometry> geometry,
                               const std::string& filename);
    void set_prebuilt_coarse_geometry(std::unique_ptr<RibbonGeometry> geometry);

    /// Release CPU geometry and GPU VBOs. Keeps GL context alive for future loads.
    void release_geometry();

    // ====== Statistics ======

    size_t get_segments_rendered() const {
        return triangles_rendered_ / 2;
    }
    size_t get_geometry_color_count() const;
    /// Itemized heap and VRAM this renderer holds, for A/B measurement. Replaces
    /// a get_memory_usage() that returned a bare total, had no caller anywhere,
    /// and silently omitted the readback buffer.
    helix::gcode::RenderMemoryReport memory_report() const;

    /// Emit memory_report() at debug level, tagged with what just happened.
    void log_memory_report(const char* when) const;
    size_t get_triangle_count() const;

  private:
    // ====== GL Resource Management ======

    bool init_gl();
#if !LV_USE_SDL
    bool try_egl_display(void* native_display, const char* label);
#endif
    bool compile_shaders();
    bool create_fbo(int width, int height);
    void destroy_fbo();
    void destroy_gl();

    // ====== Geometry Upload ======

    struct LayerVBO {
        GLBufferHandle vbo;
        size_t vertex_count = 0;
    };

    void upload_geometry(const RibbonGeometry& geom, std::vector<LayerVBO>& vbos);

    /// Upload a time-budgeted batch of layers. Returns true when all layers are done.
    bool upload_geometry_chunk(const RibbonGeometry& geom, std::vector<LayerVBO>& vbos,
                               size_t& next_layer, size_t total_layers);

    void free_vbos(std::vector<LayerVBO>& vbos);

    // ====== Internal Rendering ======

    void render_to_fbo(const ParsedGCodeFile& gcode, const GCodeCamera& camera);
    void draw_layers(const std::vector<LayerVBO>& vbos, int layer_start, int layer_end,
                     float color_scale, float alpha);
    void blit_to_lvgl(lv_layer_t* layer, const lv_area_t* widget_coords);
    void draw_cached_to_lvgl(lv_layer_t* layer, const lv_area_t* widget_coords);

    /// Crash-loop breaker (Layer 2). arm_gpu_guard() writes a persistent guard
    /// file immediately before the first real GPU draw; clear_gpu_guard()
    /// removes it after the first successful frame. If the process dies inside
    /// the driver mid-draw the file survives, and the next startup promotes it
    /// to a persistent /display/gpu_3d_blocked. Each is a one-shot per session.
    void arm_gpu_guard();
    void clear_gpu_guard();

    /// Build the model-view-projection matrix the GLES geometry pass applies:
    /// -90° model rotation about Z plus the optional vertical content offset.
    /// Shared by render_to_fbo, render_brackets_3d, and pick_object so they
    /// can't drift — drift caused #22 (clicks landing on the wrong object).
    glm::mat4 build_mvp(const GCodeCamera& camera) const;
    /// Lazily compile/link the simple line shader used for selection brackets.
    bool init_line_program();
    /// Draw 3D corner brackets around highlighted objects into the FBO. Called
    /// at the end of render_to_fbo so brackets become part of the rendered
    /// image that gets blitted to LVGL. Matches the deleted TinyGL impl.
    void render_brackets_3d(const ParsedGCodeFile& gcode, const glm::mat4& mvp);

    /// Lazily compile/link the tag program used by the selection silhouette.
    bool init_shell_program();

    /**
     * @brief Mark the highlighted objects' visible pixels in the framebuffer alpha.
     *
     * The silhouette has to follow the real toolpath: an object's bounding box is
     * a box, and EXCLUDE_OBJECT_DEFINE POLYGON= is the slicer's CONVEX HULL (a cat
     * is described as an octagon), so neither can trace a concave shape. The mesh
     * is the only thing that carries the true contour, and
     * RibbonGeometry::object_runs is what makes a per-object subset of it drawable.
     *
     * This does NOT draw the rim. It re-draws the object's own triangles at the
     * depth the lit pass already established, with the color mask set to alpha
     * only, so every pixel where the object is VISIBLE ends up holding
     * kSelectedAlpha. stroke_selection_rim() derives the white contour from that
     * after readback, exactly as the software renderer does.
     *
     * The predecessor was an inverted hull: push the mesh out along its normals,
     * cull front faces, let the lit pass overpaint the middle. That is
     * dilate-and-overpaint, and it needs a watertight mesh to work. A toolpath is
     * a stack of separate tubes, so on a sloped wall the pushed shell of one ring
     * shows through the gap above it and keeps showing through - a test cone was
     * mostly white by layer 130. Deriving the contour from the rendered pixels has
     * no such failure mode, and it costs one depth-only draw instead of a full
     * second rasterization of the object.
     *
     * @param mvp_dequant The lit pass's u_mvp — MVP with dequantization folded in.
     * @param layer_start,layer_end Solid layer range only; ghost layers are faded
     *        context and a full-strength rim there would read as a solid object.
     */
    void render_selection_tag(const ParsedGCodeFile& gcode, const glm::mat4& mvp_dequant,
                              int layer_start, int layer_end);

    // ====== Frame Skip ======

    struct CachedRenderState {
        float azimuth = -999.0f;
        float elevation = -999.0f;
        float distance = -999.0f;
        float zoom_level = -999.0f;
        glm::vec3 target{-999.0f};
        int progress_layer = -2;
        int layer_start = -2;
        int layer_end = -2;
        size_t highlight_count = 0;
        size_t highlight_set_hash = 0; // distinguishes different single-object selections
        size_t exclude_count = 0;
        glm::vec4 filament_color{-1.0f};
        uint8_t ghost_opacity = 0;
        bool operator==(const CachedRenderState& o) const;
        bool operator!=(const CachedRenderState& o) const {
            return !(*this == o);
        }
    };

    // ====== GL Backend State ======

#if LV_USE_SDL
    // SDL GL backend (desktop)
    void* sdl_gl_window_ = nullptr;  // SDL_Window*
    void* sdl_gl_context_ = nullptr; // SDL_GLContext
#else
    // EGL backend (Pi/embedded)
    void* egl_display_ = nullptr; // EGLDisplay
    void* egl_context_ = nullptr; // EGLContext
    void* egl_surface_ = nullptr; // EGLSurface (PBuffer for non-surfaceless drivers)
    void* gbm_device_ = nullptr;  // struct gbm_device*
    int drm_fd_ = -1;             // DRM file descriptor (owned by us)
#endif
    bool gl_initialized_ = false;
    bool gl_init_failed_ = false;   // Prevents repeated init attempts
    bool gl_render_failed_ = false; // Set on a fatal GL draw error; sticky for the session

    // ====== GPU crash fallback (issues #966 / #1084 / #1085) ======
    bool gpu_checked_ = false;       // GL_RENDERER denylist evaluated once per session
    bool gpu_guard_armed_ = false;   // Crash-loop guard file written before first GPU draw
    bool gpu_guard_cleared_ = false; // Guard file removed after first successful frame

    // ====== Shader State ======

    unsigned int program_ = 0; // GLuint
    // Unlit line program for selection-bracket overlays.
    unsigned int line_program_ = 0;
    int line_u_mvp_ = -1;
    int line_u_color_ = -1;
    int line_a_position_ = -1;
    unsigned int line_vbo_ = 0;
    // Flat-color shell program for the selection silhouette. Consumes the same
    // packed vertex layout as the lit program, so it draws straight from the
    // layer VBOs with no extra buffer.
    unsigned int shell_program_ = 0;
    int shell_u_mvp_ = -1;
    int shell_u_color_ = -1;
    int shell_a_position_ = -1;
    // Uniform locations
    int u_mvp_ = -1;
    int u_normal_matrix_ = -1;
    int u_light_dir_ = -1;
    int u_light_color_ = -1;
    int u_ambient_ = -1;
    int u_base_color_ = -1;
    int u_specular_intensity_ = -1;
    int u_specular_shininess_ = -1;
    int u_model_view_ = -1;
    int u_base_alpha_ = -1;
    int u_lift_strength_ = -1;
    // Attribute locations
    int a_position_ = -1;
    int a_normal_ = -1;
    int a_color_ = -1;
    int u_use_vertex_color_ = -1;
    int u_color_scale_ = -1;

    // ====== FBO State ======

    GLFramebufferHandle fbo_;
    GLRenderbufferHandle color_rbo_;
    GLRenderbufferHandle depth_rbo_;
    int fbo_width_ = 0;
    int fbo_height_ = 0;

    // ====== Output Buffer ======

    lv_draw_buf_t* draw_buf_ = nullptr;
    int draw_buf_width_ = 0;
    int draw_buf_height_ = 0;

    // ====== Viewport ======

    int viewport_width_ = 800;
    int viewport_height_ = 480;
    bool interaction_mode_ = false;

    // ====== Geometry ======

    std::unique_ptr<RibbonGeometry> geometry_;
    RibbonGeometry* active_geometry_ = nullptr;
    std::string current_filename_;

    std::vector<LayerVBO> layer_vbos_;
    bool geometry_uploaded_ = false;
    size_t upload_next_layer_ = 0;   ///< Next layer to upload (incremental)
    size_t upload_total_layers_ = 0; ///< Total layers needing upload

    // ====== Configuration ======

    GCodeColorPalette palette_; ///< Tool color palette for per-vertex coloring

    /// geometry_->color_palette exactly as the builder produced it, captured the
    /// first time set_tool_color_overrides() writes into it. Empty means "no
    /// override has been applied to the CURRENT geometry", so it is reset
    /// whenever the geometry is replaced or released - it describes that
    /// palette and no other. Guarded by palette_mutex_.
    std::vector<uint32_t> baked_color_palette_;

    /// Selection colors from ui_xml/gcode_tokens.xml, refreshed in reset_colors().
    /// The defaults cover a frame drawn before that first refresh, and the
    /// headless tests, which register no tokens.
    selection::Palette sel_palette_;
    std::mutex palette_mutex_; ///< Guards geometry color palette reads/writes
    glm::vec4 filament_color_{DEFAULT_FILAMENT_COLOR};
    float specular_intensity_ = DEFAULT_SPECULAR_INTENSITY;
    float specular_shininess_ = DEFAULT_SPECULAR_SHININESS;
    bool debug_face_colors_ = false;
    bool show_travels_ = false;
    bool show_extrusions_ = true;
    int layer_start_ = -1;
    int layer_end_ = -1;
    std::string highlighted_object_;
    /// Excluded + highlighted names and the memoized order-independent highlight
    /// hash the frame-skip comparator reads. See gcode_selection_state.h.
    SelectionState selection_;
    lv_opa_t global_opacity_ = LV_OPA_COVER;

    // ====== Ghost / Progress ======

    int progress_layer_ = -1;
    lv_opa_t ghost_opacity_ = DEFAULT_GHOST_OPACITY;
    GhostRenderMode ghost_render_mode_ = DEFAULT_GHOST_RENDER_MODE;
    float content_offset_y_percent_ = 0.0f;

    // ====== Frame Skip ======

    CachedRenderState cached_state_;
    bool frame_dirty_ = true;
    size_t triangles_rendered_ = 0;

    // ====== Readback Buffer (persistent to avoid per-frame allocation) ======

    std::vector<uint8_t> readback_buf_;

    // ====== Render Deferral (avoid blocking panel animations) ======

    int render_defer_frames_ = 0; ///< Skip N draw callbacks before first GPU render
};

} // namespace gcode
} // namespace helix

#endif // ENABLE_GLES_3D
