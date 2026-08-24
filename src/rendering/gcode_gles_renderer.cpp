// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_gles_renderer.h"

#ifdef ENABLE_GLES_3D

#include "data_root_resolver.h"
#include "gcode_gl_fallback.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>

// GL backend selection: SDL_GL on desktop (LV_USE_SDL), EGL+GBM on embedded
#if LV_USE_SDL
#include <SDL.h>
#else
#include <EGL/egl.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>
#endif

// GLES2 function declarations and common headers (both paths)
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace helix {
namespace gcode {

namespace {

/// Matrix that turns a raw quantized int16 position back into millimetres:
/// `mm = q / scale_factor + min_bounds`. Composed into u_mvp / u_model_view so
/// the GPU can consume PackedVertex positions directly and the vertex shader
/// needs no knowledge of quantization. See PackedVertex in
/// include/gcode_geometry_builder.h.
glm::mat4 dequant_matrix(const QuantizationParams& q) {
    // A zero scale would produce a degenerate matrix and collapse the model to a
    // point; fall back to identity-scale rather than emit NaNs.
    const float inv = (q.scale_factor != 0.0f) ? (1.0f / q.scale_factor) : 1.0f;
    return glm::translate(glm::mat4(1.0f), q.min_bounds) *
           glm::scale(glm::mat4(1.0f), glm::vec3(inv));
}

} // namespace

// ============================================================
// RAII GL Handle Destructors
// ============================================================

GLBufferHandle::~GLBufferHandle() {
    if (id) {
        glDeleteBuffers(1, &id);
    }
}

GLFramebufferHandle::~GLFramebufferHandle() {
    if (id) {
        glDeleteFramebuffers(1, &id);
    }
}

GLRenderbufferHandle::~GLRenderbufferHandle() {
    if (id) {
        glDeleteRenderbuffers(1, &id);
    }
}

// ============================================================
// GL Error Checking
// ============================================================

/// Check for GL errors after significant GPU operations.
/// Returns true if no error, false on error (with spdlog output).
static inline bool check_gl_error(const char* operation) {
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        spdlog::error("[GCode GLES] GL error after {}: 0x{:04X}", operation, err);
        return false;
    }
    return true;
}

// ============================================================
// GL Context Save/Restore (RAII)
// ============================================================
// The LVGL display backend may have a GL context bound on this thread.
// We must save, bind ours, and restore on scope exit.

#if LV_USE_SDL

class SdlGlContextGuard {
  public:
    SdlGlContextGuard(void* our_window, void* our_context) {
        saved_context_ = SDL_GL_GetCurrentContext();
        saved_window_ = SDL_GL_GetCurrentWindow();

        int rc = SDL_GL_MakeCurrent(static_cast<SDL_Window*>(our_window),
                                    static_cast<SDL_GLContext>(our_context));
        if (rc != 0) {
            spdlog::error("[GCode GLES] SDL_GL_MakeCurrent failed: {}", SDL_GetError());
            // Restore previous context on failure
            if (saved_context_) {
                SDL_GL_MakeCurrent(saved_window_, saved_context_);
            }
        } else {
            ok_ = true;
            our_window_ = our_window;
        }
    }

    ~SdlGlContextGuard() {
        if (!ok_)
            return;
        // Restore previous context (LVGL's SDL renderer)
        if (saved_context_) {
            SDL_GL_MakeCurrent(saved_window_, saved_context_);
        } else {
            // No prior context — unbind ours
            SDL_GL_MakeCurrent(static_cast<SDL_Window*>(our_window_), nullptr);
        }
    }

    bool ok() const {
        return ok_;
    }

    SdlGlContextGuard(const SdlGlContextGuard&) = delete;
    SdlGlContextGuard& operator=(const SdlGlContextGuard&) = delete;

  private:
    SDL_GLContext saved_context_ = nullptr;
    SDL_Window* saved_window_ = nullptr;
    void* our_window_ = nullptr;
    bool ok_ = false;
};

#else // !LV_USE_SDL — EGL backend

class EglContextGuard {
  public:
    EglContextGuard(void* our_display, void* our_surface, void* our_context) {
        saved_display_ = eglGetCurrentDisplay();
        saved_context_ = eglGetCurrentContext();
        saved_draw_ = eglGetCurrentSurface(EGL_DRAW);
        saved_read_ = eglGetCurrentSurface(EGL_READ);

        // Release current context so we can bind ours
        if (saved_context_ != EGL_NO_CONTEXT) {
            eglMakeCurrent(saved_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }

        auto surface = our_surface ? static_cast<EGLSurface>(our_surface) : EGL_NO_SURFACE;
        ok_ = eglMakeCurrent(static_cast<EGLDisplay>(our_display), surface, surface,
                             static_cast<EGLContext>(our_context));
        if (!ok_) {
            spdlog::error("[GCode GLES] eglMakeCurrent failed: 0x{:X}", eglGetError());
            // Restore previous context on failure
            if (saved_context_ != EGL_NO_CONTEXT) {
                eglMakeCurrent(saved_display_, saved_draw_, saved_read_, saved_context_);
            }
        }
    }

    ~EglContextGuard() {
        if (!ok_)
            return;
        // Release our context
        auto display = eglGetCurrentDisplay();
        if (display != EGL_NO_DISPLAY) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        // Restore previous context (SDL's)
        if (saved_context_ != EGL_NO_CONTEXT) {
            eglMakeCurrent(saved_display_, saved_draw_, saved_read_, saved_context_);
        }
    }

    bool ok() const {
        return ok_;
    }

    EglContextGuard(const EglContextGuard&) = delete;
    EglContextGuard& operator=(const EglContextGuard&) = delete;

  private:
    EGLDisplay saved_display_ = EGL_NO_DISPLAY;
    EGLContext saved_context_ = EGL_NO_CONTEXT;
    EGLSurface saved_draw_ = EGL_NO_SURFACE;
    EGLSurface saved_read_ = EGL_NO_SURFACE;
    bool ok_ = false;
};

#endif // LV_USE_SDL

// ============================================================
// GLSL Shaders
// ============================================================

static const char* VERTEX_SHADER_SOURCE = R"(
    // Per-pixel Phong shading with camera-following light.
    // Vertex format is packed (see PackedVertex):
    //   a_position : vec3 float
    //   a_color    : vec4 unorm8  (alpha unused)
    //   a_normal   : vec2 snorm8  (octahedral-encoded unit normal)
    uniform mat4 u_mvp;
    uniform mat4 u_model_view;
    uniform mat3 u_normal_matrix;
    uniform vec4 u_base_color;
    uniform float u_use_vertex_color;
    uniform float u_color_scale;

    attribute vec3 a_position;
    attribute vec4 a_color;
    attribute vec2 a_normal;

    varying vec3 v_normal;
    varying vec3 v_position;
    varying vec3 v_base_color;

    // Decode an octahedral-encoded normal (Meyer et al., 2010).
    vec3 oct_decode(vec2 e) {
        vec3 n;
        n.x = e.x;
        n.y = e.y;
        n.z = 1.0 - abs(e.x) - abs(e.y);
        if (n.z < 0.0) {
            vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
            n.xy = (1.0 - abs(n.yx)) * s;
        }
        return normalize(n);
    }

    void main() {
        gl_Position = u_mvp * vec4(a_position, 1.0);
        vec3 normal = oct_decode(a_normal);
        v_normal = normalize(u_normal_matrix * normal);
        v_position = (u_model_view * vec4(a_position, 1.0)).xyz;
        v_base_color = mix(u_base_color.rgb, a_color.rgb, u_use_vertex_color) * u_color_scale;
    }
)";

static const char* FRAGMENT_SHADER_SOURCE = R"(
    precision mediump float;
    varying vec3 v_normal;
    varying vec3 v_position;
    varying vec3 v_base_color;

    uniform vec3 u_light_dir[2];
    uniform vec3 u_light_color[2];
    uniform vec3 u_ambient;
    uniform float u_specular_intensity;
    uniform float u_specular_shininess;
    uniform float u_base_alpha;

    void main() {
        vec3 n = normalize(v_normal);
        vec3 view_dir = normalize(-v_position);

        // Diffuse from two lights
        vec3 diffuse = u_ambient;
        for (int i = 0; i < 2; i++) {
            float NdotL = max(dot(n, u_light_dir[i]), 0.0);
            diffuse += u_light_color[i] * NdotL;
        }

        // Blinn-Phong specular from both lights
        float spec = 0.0;
        for (int i = 0; i < 2; i++) {
            vec3 half_dir = normalize(u_light_dir[i] + view_dir);
            spec += pow(max(dot(n, half_dir), 0.0), u_specular_shininess);
        }

        vec3 color = v_base_color * diffuse + vec3(spec * u_specular_intensity);
        gl_FragColor = vec4(color, u_base_alpha);
    }
)";

// ============================================================
// Lighting Constants
// ============================================================

// Fixed fill light direction (front-right)
static constexpr glm::vec3 LIGHT_FRONT_DIR{0.6985074f, 0.1397015f, 0.6985074f};

// ============================================================
// Construction / Destruction
// ============================================================

GCodeGLESRenderer::GCodeGLESRenderer() {
    spdlog::debug("[GCode GLES] GCodeGLESRenderer created");
}

GCodeGLESRenderer::~GCodeGLESRenderer() {
    destroy_gl();

    if (draw_buf_) {
        // draw_buf_ is handed to the parallel render thread via dsc.src in
        // draw_cached_to_lvgl (line ~1313/1397) — same UAF pattern as
        // GCodeLayerRenderer::cache_buf_ (#929). Wait for in-flight draw tasks
        // before freeing.
        lv_draw_wait_for_finish();
        lv_draw_buf_destroy(draw_buf_);
        draw_buf_ = nullptr;
    }

    spdlog::trace("[GCode GLES] GCodeGLESRenderer destroyed");
}

// ============================================================
// GL Initialization
// ============================================================

#if !LV_USE_SDL
// Try to set up EGL with a given display, returning true on success.
// On success, egl_display_, egl_context_, and optionally egl_surface_ are set.
bool GCodeGLESRenderer::try_egl_display(void* native_display, const char* label) {
    auto display = eglGetDisplay(static_cast<EGLNativeDisplayType>(native_display));
    if (!display || display == EGL_NO_DISPLAY) {
        spdlog::debug("[GCode GLES] {} — no display", label);
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(display, &major, &minor)) {
        spdlog::debug("[GCode GLES] {} — eglInitialize failed: 0x{:X}", label, eglGetError());
        return false;
    }
    spdlog::info("[GCode GLES] EGL {}.{} via {}", major, minor, label);

    eglBindAPI(EGL_OPENGL_ES_API);

    // Check surfaceless support
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    bool has_surfaceless =
        extensions && strstr(extensions, "EGL_KHR_surfaceless_context") != nullptr;

    // Choose config (try surfaceless first, then PBuffer)
    EGLConfig egl_config = nullptr;
    EGLint num_configs = 0;

    if (has_surfaceless) {
        EGLint attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
        eglChooseConfig(display, attribs, &egl_config, 1, &num_configs);
    }
    if (num_configs == 0) {
        EGLint attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE,
                            EGL_PBUFFER_BIT, EGL_NONE};
        eglChooseConfig(display, attribs, &egl_config, 1, &num_configs);
        has_surfaceless = false;
    }
    if (num_configs == 0) {
        spdlog::debug("[GCode GLES] {} — no suitable config", label);
        eglTerminate(display);
        return false;
    }

    // Create context
    EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    auto context = eglCreateContext(display, egl_config, EGL_NO_CONTEXT, ctx_attribs);
    if (context == EGL_NO_CONTEXT) {
        spdlog::debug("[GCode GLES] {} — context creation failed: 0x{:X}", label, eglGetError());
        eglTerminate(display);
        return false;
    }

    // Create PBuffer if needed
    EGLSurface surface = EGL_NO_SURFACE;
    if (!has_surfaceless) {
        EGLint pbuf_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surface = eglCreatePbufferSurface(display, egl_config, pbuf_attribs);
        if (surface == EGL_NO_SURFACE) {
            spdlog::debug("[GCode GLES] {} — PBuffer creation failed: 0x{:X}", label,
                          eglGetError());
            eglDestroyContext(display, context);
            eglTerminate(display);
            return false;
        }
    }

    // Save the current EGL state (SDL may have a context bound on this thread)
    EGLDisplay saved_display = eglGetCurrentDisplay();
    EGLContext saved_context = eglGetCurrentContext();
    EGLSurface saved_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface saved_read = eglGetCurrentSurface(EGL_READ);
    bool had_previous_context = (saved_context != EGL_NO_CONTEXT);
    spdlog::debug("[GCode GLES] {} — prior EGL context: {} (display={})", label,
                  had_previous_context ? "yes" : "no", saved_display ? "valid" : "none");

    // Release the current context so we can bind ours
    if (had_previous_context) {
        eglMakeCurrent(saved_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    // Verify eglMakeCurrent actually works with our new context
    EGLSurface test_surface = (surface != EGL_NO_SURFACE) ? surface : EGL_NO_SURFACE;
    if (!eglMakeCurrent(display, test_surface, test_surface, context)) {
        spdlog::debug("[GCode GLES] {} — eglMakeCurrent failed: 0x{:X}", label, eglGetError());
        // Restore previous context
        if (had_previous_context)
            eglMakeCurrent(saved_display, saved_draw, saved_read, saved_context);
        if (surface != EGL_NO_SURFACE)
            eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
        eglTerminate(display);
        return false;
    }

    // Release our context (compile_shaders will re-acquire it)
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    // Restore SDL's context
    if (had_previous_context) {
        eglMakeCurrent(saved_display, saved_draw, saved_read, saved_context);
    }

    // Success — store state
    egl_display_ = display;
    egl_context_ = context;
    egl_surface_ = (surface != EGL_NO_SURFACE) ? static_cast<void*>(surface) : nullptr;
    spdlog::info("[GCode GLES] Context ready via {} ({})", label,
                 has_surfaceless ? "surfaceless" : "PBuffer");
    return true;
}
#endif // !LV_USE_SDL

bool GCodeGLESRenderer::init_gl() {
    if (gl_initialized_)
        return true;
    if (gl_init_failed_)
        return false;

#if LV_USE_SDL
    // Desktop path: use SDL_GL_CreateContext with a hidden window.
    // This avoids SDL_Init(SDL_INIT_VIDEO) on Wayland+AMD poisoning EGL operations.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    auto* window =
        SDL_CreateWindow("helix-gles-offscreen", 0, 0, 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) {
        spdlog::warn("[GCode GLES] SDL_CreateWindow failed: {}", SDL_GetError());
        gl_init_failed_ = true;
        return false;
    }

    auto gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        spdlog::warn("[GCode GLES] SDL_GL_CreateContext failed: {}", SDL_GetError());
        SDL_DestroyWindow(window);
        gl_init_failed_ = true;
        return false;
    }

    sdl_gl_window_ = window;
    sdl_gl_context_ = gl_ctx;

    spdlog::info("[GCode GLES] SDL GL context ready — GL_VERSION: {}, GL_RENDERER: {}",
                 reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    // Unbind our context (compile_shaders will re-acquire via guard)
    SDL_GL_MakeCurrent(window, nullptr);

#else  // !LV_USE_SDL — EGL backend
    // EGL initialization with fallback chain:
    // 1. GBM/DRM (Pi, embedded — surfaceless FBO rendering)
    // 2. Default EGL display (desktop Linux with X11/Wayland — PBuffer)
    bool egl_ok = false;

    // Path 1: Try GBM/DRM render nodes first (don't need DRM master, works alongside compositor)
    // Then try card nodes (needed on Pi where render nodes may not exist)
    static const char* DRM_DEVICES[] = {"/dev/dri/renderD128", "/dev/dri/renderD129",
                                        "/dev/dri/card1", "/dev/dri/card0", nullptr};
    for (int i = 0; DRM_DEVICES[i] && !egl_ok; ++i) {
        int fd = open(DRM_DEVICES[i], O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        auto* gbm = gbm_create_device(fd);
        if (!gbm) {
            close(fd);
            continue;
        }

        if (try_egl_display(gbm, DRM_DEVICES[i])) {
            drm_fd_ = fd;
            gbm_device_ = gbm;
            egl_ok = true;
        } else {
            gbm_device_destroy(gbm);
            close(fd);
        }
    }

    // Path 2: Default EGL display (Mesa on X11/Wayland)
    if (!egl_ok) {
        if (try_egl_display(EGL_DEFAULT_DISPLAY, "EGL_DEFAULT_DISPLAY")) {
            egl_ok = true;
        }
    }

    if (!egl_ok) {
        spdlog::warn("[GCode GLES] All EGL paths failed — GPU rendering unavailable");
        gl_init_failed_ = true;
        return false;
    }
#endif // LV_USE_SDL

    // Compile shaders (will acquire GL context internally via guard)
    if (!compile_shaders()) {
        gl_init_failed_ = true;
        destroy_gl();
        return false;
    }

    gl_initialized_ = true;
    return true;
}

static GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    check_gl_error("glCompileShader");

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        spdlog::error("[GCode GLES] Shader compile error: {}", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool GCodeGLESRenderer::compile_shaders() {
#if LV_USE_SDL
    SdlGlContextGuard guard(sdl_gl_window_, sdl_gl_context_);
#else
    EglContextGuard guard(egl_display_, egl_surface_, egl_context_);
#endif
    if (!guard.ok())
        return false;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SHADER_SOURCE);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER_SOURCE);
    if (!vs || !fs) {
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        return false;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    check_gl_error("glLinkProgram");

    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        spdlog::error("[GCode GLES] Program link error: {}", log);
        glDeleteProgram(program_);
        program_ = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!program_)
        return false;

    // Cache uniform/attribute locations
    u_mvp_ = glGetUniformLocation(program_, "u_mvp");
    u_normal_matrix_ = glGetUniformLocation(program_, "u_normal_matrix");
    u_light_dir_ = glGetUniformLocation(program_, "u_light_dir");
    u_light_color_ = glGetUniformLocation(program_, "u_light_color");
    u_ambient_ = glGetUniformLocation(program_, "u_ambient");
    u_base_color_ = glGetUniformLocation(program_, "u_base_color");
    u_specular_intensity_ = glGetUniformLocation(program_, "u_specular_intensity");
    u_specular_shininess_ = glGetUniformLocation(program_, "u_specular_shininess");
    u_model_view_ = glGetUniformLocation(program_, "u_model_view");
    u_base_alpha_ = glGetUniformLocation(program_, "u_base_alpha");
    a_position_ = glGetAttribLocation(program_, "a_position");
    a_normal_ = glGetAttribLocation(program_, "a_normal");
    a_color_ = glGetAttribLocation(program_, "a_color");
    u_use_vertex_color_ = glGetUniformLocation(program_, "u_use_vertex_color");
    u_color_scale_ = glGetUniformLocation(program_, "u_color_scale");

    if (a_position_ < 0 || a_normal_ < 0) {
        spdlog::error("[GCode GLES] Required attribute not found: a_position={}, a_normal={}",
                      a_position_, a_normal_);
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    spdlog::debug("[GCode GLES] Shaders compiled and linked (program={})", program_);
    return true;
}

bool GCodeGLESRenderer::create_fbo(int width, int height) {
    if (fbo_.id && fbo_width_ == width && fbo_height_ == height) {
        return true; // Already correct size
    }

    destroy_fbo();

    glGenFramebuffers(1, &fbo_.id);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_.id);
    if (!check_gl_error("glGenFramebuffers/glBindFramebuffer")) {
        destroy_fbo();
        return false;
    }

    // Color renderbuffer — use GL_RGBA8 (8 bits per channel) to match the
    // GL_RGBA/GL_UNSIGNED_BYTE format used by glReadPixels in blit_to_lvgl().
    // GL_RGBA4 would cause precision loss (4 bits stored, 8 bits read back).
    // GL_RGBA8 is available via OES_rgb8_rgba8 on GLES2 and natively on desktop GL.
    glGenRenderbuffers(1, &color_rbo_.id);
    glBindRenderbuffer(GL_RENDERBUFFER, color_rbo_.id);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, width, height);
    if (!check_gl_error("glRenderbufferStorage(color)")) {
        destroy_fbo();
        return false;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_rbo_.id);
    check_gl_error("glFramebufferRenderbuffer(color)");

    // Depth renderbuffer (16-bit)
    glGenRenderbuffers(1, &depth_rbo_.id);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rbo_.id);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
    if (!check_gl_error("glRenderbufferStorage(depth)")) {
        destroy_fbo();
        return false;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rbo_.id);
    check_gl_error("glFramebufferRenderbuffer(depth)");

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        spdlog::error("[GCode GLES] FBO incomplete: 0x{:X}", status);
        destroy_fbo();
        return false;
    }

    fbo_width_ = width;
    fbo_height_ = height;
    spdlog::debug("[GCode GLES] FBO created: {}x{}", width, height);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void GCodeGLESRenderer::destroy_fbo() {
    // RAII handles call glDelete* in their destructors via move-assignment
    depth_rbo_ = GLRenderbufferHandle();
    color_rbo_ = GLRenderbufferHandle();
    fbo_ = GLFramebufferHandle();
    fbo_width_ = 0;
    fbo_height_ = 0;
}

void GCodeGLESRenderer::destroy_gl() {
    if (!gl_initialized_)
        return;

#if LV_USE_SDL
    // Make our context current for GL resource cleanup
    if (sdl_gl_window_ && sdl_gl_context_) {
        SDL_GLContext saved_ctx = SDL_GL_GetCurrentContext();
        SDL_Window* saved_win = SDL_GL_GetCurrentWindow();

        SDL_GL_MakeCurrent(static_cast<SDL_Window*>(sdl_gl_window_),
                           static_cast<SDL_GLContext>(sdl_gl_context_));

        free_vbos(layer_vbos_);
        destroy_fbo();

        if (program_) {
            glDeleteProgram(program_);
            program_ = 0;
        }
        if (line_program_) {
            glDeleteProgram(line_program_);
            line_program_ = 0;
        }
        if (line_vbo_) {
            glDeleteBuffers(1, &line_vbo_);
            line_vbo_ = 0;
        }

        // Unbind before destroying
        SDL_GL_MakeCurrent(static_cast<SDL_Window*>(sdl_gl_window_), nullptr);

        SDL_GL_DeleteContext(static_cast<SDL_GLContext>(sdl_gl_context_));
        sdl_gl_context_ = nullptr;

        SDL_DestroyWindow(static_cast<SDL_Window*>(sdl_gl_window_));
        sdl_gl_window_ = nullptr;

        // Restore previous context
        if (saved_ctx) {
            SDL_GL_MakeCurrent(saved_win, saved_ctx);
        }
    }

#else  // !LV_USE_SDL — EGL backend
    // Save SDL's EGL state
    EGLDisplay saved_display = eglGetCurrentDisplay();
    EGLContext saved_context = eglGetCurrentContext();
    EGLSurface saved_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface saved_read = eglGetCurrentSurface(EGL_READ);

    // Make our context current for GL cleanup
    if (egl_display_ && egl_context_) {
        if (saved_context != EGL_NO_CONTEXT)
            eglMakeCurrent(saved_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglMakeCurrent(static_cast<EGLDisplay>(egl_display_), static_cast<EGLSurface>(egl_surface_),
                       static_cast<EGLSurface>(egl_surface_),
                       static_cast<EGLContext>(egl_context_));
    }

    free_vbos(layer_vbos_);
    destroy_fbo();

    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    if (line_program_) {
        glDeleteProgram(line_program_);
        line_program_ = 0;
    }
    if (line_vbo_) {
        glDeleteBuffers(1, &line_vbo_);
        line_vbo_ = 0;
    }

    if (egl_display_ && egl_context_) {
        eglMakeCurrent(static_cast<EGLDisplay>(egl_display_), EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroyContext(static_cast<EGLDisplay>(egl_display_),
                          static_cast<EGLContext>(egl_context_));
        egl_context_ = nullptr;
    }

    if (egl_display_ && egl_surface_) {
        eglDestroySurface(static_cast<EGLDisplay>(egl_display_),
                          static_cast<EGLSurface>(egl_surface_));
        egl_surface_ = nullptr;
    }

    if (egl_display_) {
        eglTerminate(static_cast<EGLDisplay>(egl_display_));
        egl_display_ = nullptr;
    }

    // Restore SDL's EGL state
    if (saved_context != EGL_NO_CONTEXT) {
        eglMakeCurrent(saved_display, saved_draw, saved_read, saved_context);
    }

    if (gbm_device_) {
        gbm_device_destroy(static_cast<struct gbm_device*>(gbm_device_));
        gbm_device_ = nullptr;
    }

    if (drm_fd_ >= 0) {
        close(drm_fd_);
        drm_fd_ = -1;
    }
#endif // LV_USE_SDL

    gl_initialized_ = false;
    geometry_uploaded_ = false;
    upload_next_layer_ = 0;
    upload_total_layers_ = 0;
    spdlog::debug("[GCode GLES] GL resources destroyed");
}

// ============================================================
// Geometry Upload
// ============================================================

void GCodeGLESRenderer::upload_geometry(const RibbonGeometry& geom, std::vector<LayerVBO>& vbos) {
    // Lock palette during read to prevent data races with set_tool_color_overrides
    std::lock_guard<std::mutex> lock(palette_mutex_);

    free_vbos(vbos);

    if (geom.strips.empty() || geom.vertices.empty()) {
        return;
    }

    // Determine number of layers
    size_t num_layers = geom.layer_strip_ranges.empty() ? 1 : geom.layer_strip_ranges.size();

    vbos.resize(num_layers);

    constexpr size_t VERTEX_STRIDE = PackedVertex::stride();

    // Reuse upload buffer across layers (sized to largest layer)
    std::vector<uint8_t> buf;

    for (size_t layer = 0; layer < num_layers; ++layer) {
        size_t first_strip = 0;
        size_t strip_count = geom.strips.size();

        if (!geom.layer_strip_ranges.empty()) {
            auto [fs, sc] = geom.layer_strip_ranges[layer];
            first_strip = fs;
            strip_count = sc;
        }

        if (strip_count == 0) {
            vbos[layer].vbo = GLBufferHandle();
            vbos[layer].vertex_count = 0;
            continue;
        }

        // Use pre-computed buffers if available (prepared on background thread)
        if (!geom.prepared_buffers.empty() && layer < geom.prepared_buffers.size() &&
            geom.prepared_buffers[layer].vertex_count > 0) {
            const auto& prepared = geom.prepared_buffers[layer];
            GLBufferHandle vbo_handle;
            glGenBuffers(1, &vbo_handle.id);
            glBindBuffer(GL_ARRAY_BUFFER, vbo_handle.id);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(prepared.vertex_count * VERTEX_STRIDE),
                         prepared.data.data(), GL_STATIC_DRAW);
            bool buf_ok = check_gl_error("glBufferData (prepared)");
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            if (!buf_ok) {
                spdlog::error("[GCode GLES] VBO creation failed for layer {} (prepared)", layer);
                vbos[layer].vbo = GLBufferHandle();
                vbos[layer].vertex_count = 0;
                continue;
            }

            vbos[layer].vbo = std::move(vbo_handle);
            vbos[layer].vertex_count = prepared.vertex_count;
            continue;
        }

        // Each strip = 4 vertices → 2 triangles → 6 vertices (for GL_TRIANGLES)
        size_t total_verts = strip_count * 6;
        size_t buf_bytes = total_verts * VERTEX_STRIDE;
        if (buf.size() < buf_bytes) {
            buf.resize(buf_bytes);
        }

        geom.expand_strips(first_strip, strip_count, reinterpret_cast<PackedVertex*>(buf.data()));

        GLBufferHandle vbo_handle;
        glGenBuffers(1, &vbo_handle.id);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_handle.id);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(total_verts * VERTEX_STRIDE),
                     buf.data(), GL_STATIC_DRAW);
        bool buf_ok = check_gl_error("glBufferData");
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        if (!buf_ok) {
            spdlog::error("[GCode GLES] VBO creation failed for layer {}", layer);
            vbos[layer].vbo = GLBufferHandle();
            vbos[layer].vertex_count = 0;
            continue;
        }

        vbos[layer].vbo = std::move(vbo_handle);
        vbos[layer].vertex_count = total_verts;
    }

    spdlog::debug("[GCode GLES] Uploaded {} layers, {} total strips to VBOs", num_layers,
                  geom.strips.size());
}

bool GCodeGLESRenderer::upload_geometry_chunk(const RibbonGeometry& geom,
                                              std::vector<LayerVBO>& vbos, size_t& next_layer,
                                              size_t total_layers) {
    // Time budget: 8ms per frame for uploads
    constexpr auto TIME_BUDGET = std::chrono::milliseconds(8);
    auto start = std::chrono::steady_clock::now();

    // Lock palette during read to prevent data races with set_tool_color_overrides
    std::lock_guard<std::mutex> lock(palette_mutex_);

    constexpr size_t VERTEX_STRIDE = PackedVertex::stride();
    // Reuse CPU buffer for layers that don't have prepared data
    std::vector<uint8_t> buf;

    while (next_layer < total_layers) {
        size_t layer = next_layer;

        size_t first_strip = 0;
        size_t strip_count = geom.strips.size();

        if (!geom.layer_strip_ranges.empty()) {
            auto [fs, sc] = geom.layer_strip_ranges[layer];
            first_strip = fs;
            strip_count = sc;
        }

        if (strip_count == 0) {
            vbos[layer].vbo = GLBufferHandle();
            vbos[layer].vertex_count = 0;
            ++next_layer;
            continue;
        }

        // Use pre-computed buffers if available (prepared on background thread)
        if (!geom.prepared_buffers.empty() && layer < geom.prepared_buffers.size() &&
            geom.prepared_buffers[layer].vertex_count > 0) {
            const auto& prepared = geom.prepared_buffers[layer];
            GLBufferHandle vbo_handle;
            glGenBuffers(1, &vbo_handle.id);
            glBindBuffer(GL_ARRAY_BUFFER, vbo_handle.id);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(prepared.vertex_count * VERTEX_STRIDE),
                         prepared.data.data(), GL_STATIC_DRAW);
            bool buf_ok = check_gl_error("glBufferData (prepared)");
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            if (!buf_ok) {
                spdlog::error("[GCode GLES] VBO creation failed for layer {} (prepared)", layer);
                vbos[layer].vbo = GLBufferHandle();
                vbos[layer].vertex_count = 0;
            } else {
                vbos[layer].vbo = std::move(vbo_handle);
                vbos[layer].vertex_count = prepared.vertex_count;
            }
        } else {
            // CPU fallback: expand strips inline (for color re-upload case)
            size_t total_verts = strip_count * 6;
            size_t buf_bytes = total_verts * VERTEX_STRIDE;
            if (buf.size() < buf_bytes) {
                buf.resize(buf_bytes);
            }

            geom.expand_strips(first_strip, strip_count,
                               reinterpret_cast<PackedVertex*>(buf.data()));

            GLBufferHandle vbo_handle;
            glGenBuffers(1, &vbo_handle.id);
            glBindBuffer(GL_ARRAY_BUFFER, vbo_handle.id);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(total_verts * VERTEX_STRIDE),
                         buf.data(), GL_STATIC_DRAW);
            bool buf_ok = check_gl_error("glBufferData");
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            if (!buf_ok) {
                spdlog::error("[GCode GLES] VBO creation failed for layer {}", layer);
                vbos[layer].vbo = GLBufferHandle();
                vbos[layer].vertex_count = 0;
            } else {
                vbos[layer].vbo = std::move(vbo_handle);
                vbos[layer].vertex_count = total_verts;
            }
        }

        ++next_layer;

        // Check time budget (check every layer, glBufferData can be slow)
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= TIME_BUDGET) {
            break;
        }
    }

    bool done = (next_layer >= total_layers);
    if (done) {
        spdlog::debug("[GCode GLES] Incremental upload complete: all {} layers uploaded",
                      total_layers);
    }
    return done;
}

void GCodeGLESRenderer::free_vbos(std::vector<LayerVBO>& vbos) {
    // RAII handles (GLBufferHandle) call glDeleteBuffers in their destructors
    vbos.clear();
}

// ============================================================
// Main Render Entry Point
// ============================================================

void GCodeGLESRenderer::render(lv_layer_t* layer, const ParsedGCodeFile& gcode,
                               const GCodeCamera& camera, const lv_area_t* widget_coords) {
    // Initialize GL on first render
    if (!gl_initialized_) {
        if (!init_gl()) {
            return; // GPU not available
        }
    }

    // Sticky bail: once a fatal GL error or a denylisted GPU has disabled the
    // GPU path this session, never issue another draw. The viewer polls
    // render_failed() and falls back to the pure-CPU 2D renderer.
    if (gl_render_failed_)
        return;

    // No geometry loaded
    if (!geometry_)
        return;

        // Acquire our GL context (saves and restores LVGL's)
#if LV_USE_SDL
    SdlGlContextGuard guard(sdl_gl_window_, sdl_gl_context_);
#else
    EglContextGuard guard(egl_display_, egl_surface_, egl_context_);
#endif
    if (!guard.ok())
        return;

    // Layer 1 — proactive GL_RENDERER denylist (issues #966 / #1084 / #1085).
    // Evaluated exactly once, now that the GL context is current so GL_RENDERER
    // is valid. Known-bad Mali/Panfrost GPUs SIGSEGV inside glDrawArrays, which
    // the reactive glGetError() guard cannot catch — bail before the first draw.
    if (!gpu_checked_) {
        gpu_checked_ = true;
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        spdlog::info("[GCode GLES] GL_RENDERER: {}", renderer ? renderer : "(null)");
        if (gl_renderer_is_denylisted(renderer)) {
            spdlog::error("[GCode GLES] GPU '{}' is denylisted (known to fault inside the driver) "
                          "— disabling GPU rendering, falling back to 2D",
                          renderer ? renderer : "(null)");
            gl_render_failed_ = true;
            return;
        }
    }

    // Incremental VBO upload: upload a time-budgeted batch of layers per frame
    if (!geometry_uploaded_ && geometry_) {
        // Initialize incremental upload on first frame
        if (upload_total_layers_ == 0) {
            free_vbos(layer_vbos_); // Free old VBOs inside GL context
            size_t num_layers =
                geometry_->layer_strip_ranges.empty() ? 1 : geometry_->layer_strip_ranges.size();
            layer_vbos_.resize(num_layers);
            upload_total_layers_ = num_layers;
            spdlog::info("[GCode GLES] Starting incremental VBO upload: {} layers", num_layers);
        }

        bool done = upload_geometry_chunk(*geometry_, layer_vbos_, upload_next_layer_,
                                          upload_total_layers_);
        if (done) {
            geometry_uploaded_ = true;
            upload_next_layer_ = 0;
            upload_total_layers_ = 0;
            // Free pre-computed interleaved buffers — all data is now in GPU VBOs.
            // The CPU fallback path (for tool color re-upload) re-expands from
            // the compact vertices/strips directly, so these aren't needed.
            size_t freed = 0;
            for (auto& pb : geometry_->prepared_buffers) {
                freed += pb.data.capacity(); // data is std::vector<uint8_t> — capacity is bytes
                pb.data.clear();
                pb.data.shrink_to_fit();
            }
            geometry_->prepared_buffers.clear();
            geometry_->prepared_buffers.shrink_to_fit();
            if (freed > 0) {
                spdlog::info("[GCode GLES] Freed {} MB of upload buffers after VBO upload",
                             freed / (1024 * 1024));
            }
            // Defer first GPU render by a few frames to avoid blocking panel animations
            render_defer_frames_ = 3;
        } else {
            // Still uploading -- show cached buffer if available, otherwise skip frame
            if (draw_buf_) {
                draw_cached_to_lvgl(layer, widget_coords);
            }
            return;
        }
    }

    // If deferring, draw the cached buffer and count down.
    // If no cached buffer exists, skip the defer entirely — nothing to show.
    if (render_defer_frames_ > 0) {
        if (draw_buf_) {
            render_defer_frames_--;
            draw_cached_to_lvgl(layer, widget_coords);
            return;
        }
        render_defer_frames_ = 0;
    }
    // Build current render state for frame-skip check
    CachedRenderState current_state;
    current_state.azimuth = camera.get_azimuth();
    current_state.elevation = camera.get_elevation();
    current_state.distance = camera.get_distance();
    current_state.zoom_level = camera.get_zoom_level();
    current_state.target = camera.get_target();
    current_state.progress_layer = progress_layer_;
    current_state.layer_start = layer_start_;
    current_state.layer_end = layer_end_;
    current_state.highlight_count = highlighted_objects_.size();
    current_state.highlight_set_hash = highlighted_objects_hash_;
    current_state.exclude_count = excluded_objects_.size();
    current_state.filament_color = filament_color_;
    current_state.ghost_opacity = ghost_opacity_;

    // Skip GPU render if state unchanged and we have a valid cached framebuffer.
    // draw_cached_to_lvgl skips glReadPixels — just blits the existing draw_buf_.
    if (!frame_dirty_ && current_state == cached_state_ && draw_buf_) {
        draw_cached_to_lvgl(layer, widget_coords);
        return;
    }

    cached_state_ = current_state;
    frame_dirty_ = false;

    auto t0 = std::chrono::high_resolution_clock::now();

    // Layer 2 — crash-loop breaker. Arm the guard file immediately before the
    // real GPU draw; if the driver hard-faults inside render_to_fbo the process
    // dies with the file still present and the next startup promotes it to a
    // persistent block. Cleared right after the first successful blit.
    arm_gpu_guard();

    // Render to FBO
    render_to_fbo(gcode, camera);

    auto t1 = std::chrono::high_resolution_clock::now();

    // Read pixels from FBO and blit to LVGL
    blit_to_lvgl(layer, widget_coords);

    clear_gpu_guard();

    auto t2 = std::chrono::high_resolution_clock::now();

    // guard destructor restores LVGL's GL context

    auto gpu_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
    auto blit_ms = std::chrono::duration<float, std::milli>(t2 - t1).count();
    spdlog::trace("[GCode GLES] gpu={:.1f}ms, blit={:.1f}ms, triangles={}", gpu_ms, blit_ms,
                  triangles_rendered_);
}

// ============================================================
// GPU crash-loop breaker (Layer 2, issues #966 / #1084 / #1085)
// ============================================================

void GCodeGLESRenderer::arm_gpu_guard() {
    if (gpu_guard_armed_)
        return;
    gpu_guard_armed_ = true;

    const std::string path = helix::writable_path("gpu_3d_guard");
    std::ofstream guard(path, std::ios::out | std::ios::trunc);
    if (guard.is_open()) {
        guard << "1";
        spdlog::debug("[GCode GLES] Armed GPU crash-loop guard: {}", path);
    } else {
        spdlog::warn("[GCode GLES] Could not write GPU crash-loop guard: {}", path);
    }
}

void GCodeGLESRenderer::clear_gpu_guard() {
    if (!gpu_guard_armed_ || gpu_guard_cleared_)
        return;
    gpu_guard_cleared_ = true;

    const std::string path = helix::writable_path("gpu_3d_guard");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        spdlog::warn("[GCode GLES] Could not remove GPU crash-loop guard {}: {}", path,
                     ec.message());
    } else {
        spdlog::debug("[GCode GLES] Cleared GPU crash-loop guard after first successful frame");
    }
}

// ============================================================
// FBO Rendering
// ============================================================

void GCodeGLESRenderer::render_to_fbo(const ParsedGCodeFile& gcode, const GCodeCamera& camera) {
    int render_w = viewport_width_;
    int render_h = viewport_height_;
    if (render_w < 1)
        render_w = 1;
    if (render_h < 1)
        render_h = 1;

    // Create/resize FBO
    if (!create_fbo(render_w, render_h)) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_.id);
    glViewport(0, 0, render_w, render_h);

    // Neutral gray background — light and dark filaments both contrast well
    glClearColor(BACKGROUND_GRAY, BACKGROUND_GRAY, BACKGROUND_GRAY_BLUE, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    // Select active geometry
    auto* active_vbos = &layer_vbos_;
    active_geometry_ = geometry_.get();

    if (!active_geometry_ || active_vbos->empty()) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    // Use shader program
    glUseProgram(program_);

    glm::mat4 mvp = build_mvp(camera);

    // Normal matrix (inverse transpose of upper-left 3x3 of model-view).
    glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0, 0, 1));
    glm::mat4 view = camera.get_view_matrix();
    glm::mat3 normal_mat = glm::transpose(glm::inverse(glm::mat3(view * model)));

    // Vertex positions arrive as raw quantized int16. Dequantization is affine
    // (mm = q / scale_factor + min_bounds), so it composes into the matrices we
    // already upload instead of costing a uniform and a shader edit. Applied on
    // the right so it runs first, before model/view/projection.
    //
    // Normals are unaffected: they are a separate octahedral attribute, and this
    // transform is a uniform positive scale plus a translation, which cannot
    // skew a normal or flip winding.
    const glm::mat4 dequant = dequant_matrix(active_geometry_->quantization);

    // Set uniforms
    glm::mat4 mvp_dequant = mvp * dequant;
    glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, glm::value_ptr(mvp_dequant));
    glUniformMatrix3fv(u_normal_matrix_, 1, GL_FALSE, glm::value_ptr(normal_mat));

    glm::mat4 model_view = view * model * dequant;
    glUniformMatrix4fv(u_model_view_, 1, GL_FALSE, glm::value_ptr(model_view));

    // Light 0: Camera-following directional light (tracks camera position)
    glm::vec3 cam_pos = camera.get_camera_position();
    glm::vec3 cam_target = camera.get_target();
    glm::vec3 cam_light_world = glm::normalize(cam_pos - cam_target);

    // Light 1: Fixed fill light from front-right (prevents black shadows)
    // Both transformed to view space (normals are in view space via u_normal_matrix)
    glm::mat3 view_model_rot = glm::mat3(view * model);
    glm::vec3 light_dirs[2] = {glm::normalize(view_model_rot * cam_light_world),
                               glm::normalize(view_model_rot * LIGHT_FRONT_DIR)};
    glm::vec3 light_colors[2] = {glm::vec3(CAMERA_LIGHT_INTENSITY), // Camera light: primary
                                 glm::vec3(FILL_LIGHT_INTENSITY)};  // Fill light: subtle
    glUniform3fv(u_light_dir_, 2, glm::value_ptr(light_dirs[0]));
    glUniform3fv(u_light_color_, 2, glm::value_ptr(light_colors[0]));

    glm::vec3 ambient{AMBIENT_INTENSITY};
    glUniform3fv(u_ambient_, 1, glm::value_ptr(ambient));

    // Material
    glUniform1f(u_specular_intensity_, specular_intensity_);
    glUniform1f(u_specular_shininess_, specular_shininess_);

    // Per-vertex color mode: use vertex colors when geometry has a color palette.
    // With per-tool AMS overrides, the palette is updated in-place so vertex colors
    // always reflect the correct AMS slot colors. Only fall back to uniform color
    // when palette has a single-tool override (legacy path).
    bool has_palette = active_geometry_ && !active_geometry_->color_palette.empty();
    bool has_vertex_colors = has_palette && !palette_.has_override;
    glUniform1f(u_use_vertex_color_, has_vertex_colors ? 1.0f : 0.0f);

    // Determine layer range
    int max_layer = static_cast<int>(active_vbos->size()) - 1;
    int draw_start = (layer_start_ >= 0) ? layer_start_ : 0;
    int draw_end = (layer_end_ >= 0) ? std::min(layer_end_, max_layer) : max_layer;

    triangles_rendered_ = 0;

    // Ghost / print progress rendering
    if (progress_layer_ >= 0 && progress_layer_ < max_layer) {
        // Pass 1: Solid layers (0 to progress_layer_)
        int solid_end = std::min(progress_layer_, draw_end);
        if (draw_start <= solid_end) {
            draw_layers(*active_vbos, draw_start, solid_end, 1.0f, 1.0f);
        }

        // Pass 2: Ghost layers (progress_layer_+1 to end) with alpha blending
        // Use elevated color_scale to lighten ghost colors (washes toward white)
        int ghost_start = std::max(progress_layer_ + 1, draw_start);
        if (ghost_start <= draw_end) {
            float alpha = ghost_opacity_ / 255.0f;
            constexpr float GHOST_LIGHTEN_SCALE = 4.0f;
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE); // Don't write ghost depth (prevents z-fighting)
            draw_layers(*active_vbos, ghost_start, draw_end, GHOST_LIGHTEN_SCALE, alpha);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
    } else {
        // Normal: all layers solid
        draw_layers(*active_vbos, draw_start, draw_end, 1.0f, 1.0f);
    }

    glUseProgram(0);

    // Selection brackets on top, using the same MVP as the geometry pass so
    // brackets stay anchored to objects under rotation/zoom. Drawn last with
    // depth test disabled inside render_brackets_3d().
    render_brackets_3d(gcode, mvp);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GCodeGLESRenderer::draw_layers(const std::vector<LayerVBO>& vbos, int layer_start,
                                    int layer_end, float color_scale, float alpha) {
    // Set uniforms for this draw batch
    glUniform4fv(u_base_color_, 1, glm::value_ptr(filament_color_));
    glUniform1f(u_color_scale_, color_scale);
    glUniform1f(u_base_alpha_, alpha);

    constexpr size_t STRIDE = PackedVertex::stride();

    // Enable vertex attributes once before the loop (a_position_ and a_normal_
    // are validated >= 0 during compile_shaders)
    glEnableVertexAttribArray(static_cast<GLuint>(a_position_));
    glEnableVertexAttribArray(static_cast<GLuint>(a_normal_));
    if (a_color_ >= 0) {
        glEnableVertexAttribArray(static_cast<GLuint>(a_color_));
    }

    for (int layer = layer_start; layer <= layer_end; ++layer) {
        if (layer < 0 || layer >= static_cast<int>(vbos.size()))
            continue;
        const auto& lv = vbos[static_cast<size_t>(layer)];
        if (!lv.vbo || lv.vertex_count == 0)
            continue;

        glBindBuffer(GL_ARRAY_BUFFER, lv.vbo);

        // Position: quantized int16, NOT normalized — the raw integer reaches the
        // shader and u_mvp / u_model_view carry the dequantization.
        glVertexAttribPointer(static_cast<GLuint>(a_position_), 3, GL_SHORT, GL_FALSE,
                              static_cast<GLsizei>(STRIDE),
                              reinterpret_cast<void*>(PackedVertex::position_offset()));

        // Normal: int8[2] octahedral, decoded in the vertex shader.
        glVertexAttribPointer(static_cast<GLuint>(a_normal_), 2, GL_BYTE, GL_TRUE,
                              static_cast<GLsizei>(STRIDE),
                              reinterpret_cast<void*>(PackedVertex::normal_offset()));

        if (a_color_ >= 0) {
            // Color: RGBA8 unorm; alpha byte is present but ignored by the shader.
            glVertexAttribPointer(static_cast<GLuint>(a_color_), 4, GL_UNSIGNED_BYTE, GL_TRUE,
                                  static_cast<GLsizei>(STRIDE),
                                  reinterpret_cast<void*>(PackedVertex::color_offset()));
        }

        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(lv.vertex_count));
        triangles_rendered_ += lv.vertex_count / 3;
    }

    glDisableVertexAttribArray(static_cast<GLuint>(a_position_));
    glDisableVertexAttribArray(static_cast<GLuint>(a_normal_));
    if (a_color_ >= 0) {
        glDisableVertexAttribArray(static_cast<GLuint>(a_color_));
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // One glGetError() per draw batch — NOT per primitive (a per-glDrawArrays
    // check would stall the GPU pipeline). On constrained Mali/Panfrost GPUs
    // (e.g. Allwinner CB1) the driver can fault under memory pressure with the
    // phong shader + ribbon VBOs; a fatal error here means we must abandon GPU
    // rendering. The viewer polls render_failed() after render() and falls back
    // to the pure-CPU 2D renderer for the rest of the session.
    GLenum draw_err = glGetError();
    if (gl_draw_error_is_fatal(draw_err)) {
        spdlog::error("[GCode GLES] Fatal GL error after glDrawArrays: 0x{:04X} — disabling GPU "
                      "rendering, falling back to 2D",
                      draw_err);
        gl_render_failed_ = true;
    }
}

// ============================================================
// LVGL Output
// ============================================================

void GCodeGLESRenderer::draw_cached_to_lvgl(lv_layer_t* layer, const lv_area_t* widget_coords) {
    // Fast path: draw the existing draw_buf_ without GPU readback.
    // Used when the frame hasn't changed (frame-skip) or during render deferral.
    if (!draw_buf_ || !draw_buf_->data)
        return;

    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
    img_dsc.src = draw_buf_;

    lv_area_t area = *widget_coords;
    lv_draw_image(layer, &img_dsc, &area);
}

void GCodeGLESRenderer::blit_to_lvgl(lv_layer_t* layer, const lv_area_t* widget_coords) {
    int widget_w = lv_area_get_width(widget_coords);
    int widget_h = lv_area_get_height(widget_coords);

    // Create or recreate draw buffer at widget size
    if (!draw_buf_ || draw_buf_width_ != widget_w || draw_buf_height_ != widget_h) {
        if (draw_buf_) {
            // draw_buf_ may still be in flight to the parallel render thread
            // (#929 cluster). Drain pending draws before freeing.
            lv_draw_wait_for_finish();
            lv_draw_buf_destroy(draw_buf_);
        }
        draw_buf_ = lv_draw_buf_create(static_cast<uint32_t>(widget_w),
                                       static_cast<uint32_t>(widget_h), LV_COLOR_FORMAT_RGB888, 0);
        if (!draw_buf_) {
            spdlog::error("[GCode GLES] Failed to create draw buffer");
            return;
        }
        draw_buf_width_ = widget_w;
        draw_buf_height_ = widget_h;
    }

    if (!fbo_.id)
        return;

    // Read pixels from FBO
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_.id);

    // Read RGBA from GPU (matches GL_RGBA8_OES renderbuffer format)
    // Reuse persistent readback buffer to avoid per-frame allocation
    size_t readback_size = static_cast<size_t>(fbo_width_ * fbo_height_ * 4);
    if (readback_buf_.size() != readback_size) {
        readback_buf_.resize(readback_size);
    }
    glReadPixels(0, 0, fbo_width_, fbo_height_, GL_RGBA, GL_UNSIGNED_BYTE, readback_buf_.data());
    check_gl_error("glReadPixels");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Convert GL RGBA → LVGL RGB888 (BGR byte order), flip Y, and scale if needed
    if (!draw_buf_->data) {
        spdlog::error("[GCode GLES] draw_buf_ data is null");
        return;
    }
    auto* dest = static_cast<uint8_t*>(draw_buf_->data);
    const auto* src = readback_buf_.data();
    bool needs_scale = (fbo_width_ != widget_w || fbo_height_ != widget_h);
    // Use actual draw buffer stride (aligned to LV_DRAW_BUF_STRIDE_ALIGN)
    uint32_t dst_stride = draw_buf_->header.stride;

    // Row-based conversion: RGBA→BGR with Y-flip
    for (int dy = 0; dy < widget_h; ++dy) {
        int sy = needs_scale ? (dy * fbo_height_ / widget_h) : dy;
        int gl_row = fbo_height_ - 1 - sy;
        const auto* src_row = src + static_cast<size_t>(gl_row * fbo_width_) * 4;
        auto* dst_row = dest + static_cast<size_t>(dy) * dst_stride;

        if (needs_scale) {
            for (int dx = 0; dx < widget_w; ++dx) {
                int sx = dx * fbo_width_ / widget_w;
                size_t si = static_cast<size_t>(sx) * 4;
                size_t di = static_cast<size_t>(dx) * 3;
                dst_row[di + 0] = src_row[si + 2]; // B
                dst_row[di + 1] = src_row[si + 1]; // G
                dst_row[di + 2] = src_row[si + 0]; // R
            }
        } else {
            // No scaling: convert entire row RGBA→BGR
            for (int dx = 0; dx < widget_w; ++dx) {
                size_t si = static_cast<size_t>(dx) * 4;
                size_t di = static_cast<size_t>(dx) * 3;
                dst_row[di + 0] = src_row[si + 2]; // B
                dst_row[di + 1] = src_row[si + 1]; // G
                dst_row[di + 2] = src_row[si + 0]; // R
            }
        }
    }

    // Draw to LVGL layer
    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
    img_dsc.src = draw_buf_;

    lv_area_t area = *widget_coords;
    lv_draw_image(layer, &img_dsc, &area);
}

// ============================================================
// CachedRenderState
// ============================================================

bool GCodeGLESRenderer::CachedRenderState::operator==(const CachedRenderState& o) const {
    // Epsilon comparisons: tighter for angles, looser for zoom/distance
    auto near_angle = [](float a, float b) { return std::abs(a - b) < ANGLE_EPSILON; };
    auto near_zoom = [](float a, float b) { return std::abs(a - b) < ZOOM_EPSILON; };
    return near_angle(azimuth, o.azimuth) && near_angle(elevation, o.elevation) &&
           near_zoom(distance, o.distance) && near_zoom(zoom_level, o.zoom_level) &&
           near_angle(target.x, o.target.x) && near_angle(target.y, o.target.y) &&
           near_angle(target.z, o.target.z) && progress_layer == o.progress_layer &&
           layer_start == o.layer_start && layer_end == o.layer_end &&
           highlight_count == o.highlight_count && highlight_set_hash == o.highlight_set_hash &&
           exclude_count == o.exclude_count && filament_color == o.filament_color &&
           ghost_opacity == o.ghost_opacity;
}

// ============================================================
// Configuration Methods
// ============================================================

void GCodeGLESRenderer::set_viewport_size(int width, int height) {
    if (width == viewport_width_ && height == viewport_height_)
        return;
    viewport_width_ = width;
    viewport_height_ = height;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_interaction_mode(bool interacting) {
    if (interaction_mode_ == interacting)
        return;
    interaction_mode_ = interacting;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_filament_color(const std::string& hex_color) {
    if (hex_color.size() < 7 || hex_color[0] != '#')
        return;
    unsigned int r = 0, g = 0, b = 0;
    if (sscanf(hex_color.c_str(), "#%02x%02x%02x", &r, &g, &b) == 3) {
        filament_color_ = glm::vec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
        frame_dirty_ = true;
    }
}

void GCodeGLESRenderer::set_extrusion_color(lv_color_t color) {
    filament_color_ =
        glm::vec4(color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f, 1.0f);
    palette_.has_override = true;
    palette_.override_color = color;
    frame_dirty_ = true;
    spdlog::debug("[GCode GLES] set_extrusion_color: R={} G={} B={} → ({:.2f},{:.2f},{:.2f})",
                  color.red, color.green, color.blue, filament_color_.r, filament_color_.g,
                  filament_color_.b);
}

void GCodeGLESRenderer::set_tool_color_overrides(const std::vector<uint32_t>& ams_colors) {
    if (!geometry_ || ams_colors.empty()) {
        return;
    }

    // Lock palette during modification to prevent data races with render path
    std::lock_guard<std::mutex> lock(palette_mutex_);

    // Replace palette entries using tool→palette mapping from geometry build
    bool changed = false;
    for (size_t tool = 0; tool < ams_colors.size(); ++tool) {
        auto it = geometry_->tool_palette_map.find(static_cast<uint8_t>(tool));
        if (it == geometry_->tool_palette_map.end()) {
            continue;
        }
        uint8_t palette_idx = it->second;
        if (palette_idx < geometry_->color_palette.size() &&
            geometry_->color_palette[palette_idx] != ams_colors[tool]) {
            geometry_->color_palette[palette_idx] = ams_colors[tool];
            changed = true;
        }
    }

    if (changed) {
        // Per-tool overrides replace palette entries baked into vertex data,
        // so clear any single-color override that would bypass vertex colors.
        palette_.has_override = false;
        // Patch only the color bytes in the prepared buffers in place. Avoids
        // discarding the ~100MB pack the background thread just produced and
        // re-expanding it on the foreground thread — only the RGBA8 lanes
        // need to change.
        if (geometry_) {
            geometry_->patch_prepared_buffer_colors();
        }
        // Force VBO re-upload to push the new colors to the GPU
        // (old VBOs freed inside render() where GL context is active)
        geometry_uploaded_ = false;
        upload_next_layer_ = 0;
        upload_total_layers_ = 0;
        frame_dirty_ = true;
        spdlog::debug("[GCode GLES] Applied {} tool color overrides, triggering VBO re-upload",
                      ams_colors.size());
    }
}

void GCodeGLESRenderer::set_smooth_shading(bool /*enable*/) {
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_extrusion_width(float width_mm) {
    extrusion_width_ = width_mm;
}

void GCodeGLESRenderer::set_simplification_tolerance(float /*tolerance_mm*/) {
    // Simplification is applied during geometry build, not at render time
}

void GCodeGLESRenderer::set_specular(float intensity, float shininess) {
    specular_intensity_ = std::clamp(intensity, MIN_SPECULAR_INTENSITY, MAX_SPECULAR_INTENSITY);
    specular_shininess_ = std::clamp(shininess, MIN_SPECULAR_SHININESS, MAX_SPECULAR_SHININESS);
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_debug_face_colors(bool enable) {
    debug_face_colors_ = enable;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_show_travels(bool show) {
    show_travels_ = show;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_show_extrusions(bool show) {
    show_extrusions_ = show;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_layer_range(int start, int end) {
    layer_start_ = start;
    layer_end_ = end;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_highlighted_object(const std::string& name) {
    std::unordered_set<std::string> objects;
    if (!name.empty())
        objects.insert(name);
    set_highlighted_objects(objects);
}

void GCodeGLESRenderer::set_highlighted_objects(const std::unordered_set<std::string>& names) {
    if (highlighted_objects_ != names) {
        highlighted_objects_ = names;
        // Memoize the set hash so the per-frame cached-state build doesn't
        // iterate every name on each LVGL invalidation tick.
        size_t h = 0;
        for (const auto& n : highlighted_objects_) {
            h ^= std::hash<std::string>{}(n) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        highlighted_objects_hash_ = h;
        frame_dirty_ = true;
    }
}

void GCodeGLESRenderer::set_excluded_objects(const std::unordered_set<std::string>& names) {
    if (excluded_objects_ != names) {
        excluded_objects_ = names;
        frame_dirty_ = true;
    }
}

void GCodeGLESRenderer::set_global_opacity(lv_opa_t opacity) {
    global_opacity_ = opacity;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::reset_colors() {
    palette_.has_override = false;
    filament_color_ = DEFAULT_FILAMENT_COLOR;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::clear_cached_frame() {
    // Free the cached draw buffer so stale frames aren't blitted during render deferral
    if (draw_buf_) {
        lv_draw_buf_destroy(draw_buf_);
        draw_buf_ = nullptr;
        draw_buf_width_ = 0;
        draw_buf_height_ = 0;
    }
    render_defer_frames_ = 0;
}

RenderingOptions GCodeGLESRenderer::get_options() const {
    RenderingOptions opts;
    opts.show_extrusions = show_extrusions_;
    opts.show_travels = show_travels_;
    opts.layer_start = layer_start_;
    opts.layer_end = layer_end_;
    opts.highlighted_object = highlighted_object_;
    return opts;
}

// ============================================================
// Ghost / Print Progress
// ============================================================

void GCodeGLESRenderer::set_print_progress_layer(int current_layer) {
    if (progress_layer_ != current_layer) {
        progress_layer_ = current_layer;
        frame_dirty_ = true;
    }
}

void GCodeGLESRenderer::set_ghost_opacity(lv_opa_t opacity) {
    ghost_opacity_ = opacity;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_content_offset_y(float offset_percent) {
    content_offset_y_percent_ = std::clamp(offset_percent, -1.0f, 1.0f);
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_ghost_render_mode(GhostRenderMode mode) {
    ghost_render_mode_ = mode;
    frame_dirty_ = true;
}

int GCodeGLESRenderer::get_max_layer_index() const {
    if (geometry_)
        return static_cast<int>(geometry_->max_layer_index);
    return 0;
}

// ============================================================
// Geometry Release
// ============================================================

void GCodeGLESRenderer::release_geometry() {
    size_t freed = geometry_ ? geometry_->memory_usage() : 0;

    // Free GPU VBOs (requires GL context)
    if (gl_initialized_ && !layer_vbos_.empty()) {
#if LV_USE_SDL
        SdlGlContextGuard guard(sdl_gl_window_, sdl_gl_context_);
#else
        EglContextGuard guard(egl_display_, egl_surface_, egl_context_);
#endif
        if (guard.ok()) {
            free_vbos(layer_vbos_);
        }
    }

    // Free CPU geometry
    geometry_.reset();
    active_geometry_ = nullptr;
    current_filename_.clear();
    geometry_uploaded_ = false;
    upload_next_layer_ = 0;
    upload_total_layers_ = 0;

    // Free readback buffer
    freed += readback_buf_.capacity();
    readback_buf_.clear();
    readback_buf_.shrink_to_fit();

    if (freed > 0) {
        spdlog::info("[GCode GLES] Released geometry: {} MB freed", freed / (1024 * 1024));
    }
}

// ============================================================
// Geometry Loading
// ============================================================

void GCodeGLESRenderer::set_prebuilt_geometry(std::unique_ptr<RibbonGeometry> geometry,
                                              const std::string& filename) {
    geometry_ = std::move(geometry);
    current_filename_ = filename;
    geometry_uploaded_ = false;
    upload_next_layer_ = 0;
    upload_total_layers_ = 0;
    frame_dirty_ = true;
    spdlog::debug("[GCode GLES] Geometry set: {} strips, {} vertices",
                  geometry_ ? geometry_->strips.size() : 0,
                  geometry_ ? geometry_->vertices.size() : 0);
}

void GCodeGLESRenderer::set_prebuilt_coarse_geometry(std::unique_ptr<RibbonGeometry> /*geometry*/) {
    // Coarse LOD no longer used — GPU handles full geometry at full speed
}

// ============================================================
// Statistics
// ============================================================

size_t GCodeGLESRenderer::get_geometry_color_count() const {
    if (geometry_)
        return geometry_->color_palette.size();
    return 0;
}

size_t GCodeGLESRenderer::get_memory_usage() const {
    size_t total = sizeof(*this);
    if (geometry_) {
        total += geometry_->vertices.size() * sizeof(RibbonVertex);
        total += geometry_->strips.size() * sizeof(TriangleStrip);
        total += geometry_->strip_color_index.size() * sizeof(uint8_t);
    }
    if (draw_buf_) {
        total += static_cast<size_t>(draw_buf_width_ * draw_buf_height_ * 3);
    }
    // Approximate GPU VRAM usage (VBOs + FBO)
    for (const auto& lv : layer_vbos_) {
        if (lv.vbo) {
            total += lv.vertex_count * PackedVertex::stride();
        }
    }
    if (fbo_.id) {
        // Color RBO (RGBA8 = 4 bytes/pixel) + Depth RBO (16-bit = 2 bytes/pixel)
        total += static_cast<size_t>(fbo_width_ * fbo_height_ * 6);
    }
    return total;
}

size_t GCodeGLESRenderer::get_triangle_count() const {
    if (geometry_)
        return geometry_->extrusion_triangle_count;
    return 0;
}

glm::mat4 GCodeGLESRenderer::build_mvp(const GCodeCamera& camera) const {
    // Model: -90° CW around Z to match slicer thumbnail orientation.
    glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0, 0, 1));
    glm::mat4 proj = camera.get_projection_matrix();
    // NDC Y range is [-1, 1]; multiply by 2 so the percent maps to a full shift.
    if (std::abs(content_offset_y_percent_) > 0.001f) {
        proj[3][1] += -content_offset_y_percent_ * 2.0f;
    }
    return proj * camera.get_view_matrix() * model;
}

// ============================================================
// Selection Brackets (3D, GPU-side — drawn inside the FBO)
// ============================================================

static const char* LINE_VERTEX_SHADER = R"(
    uniform mat4 u_mvp;
    attribute vec3 a_position;
    void main() {
        gl_Position = u_mvp * vec4(a_position, 1.0);
    }
)";

static const char* LINE_FRAGMENT_SHADER = R"(
    precision mediump float;
    uniform vec4 u_color;
    void main() {
        gl_FragColor = u_color;
    }
)";

bool GCodeGLESRenderer::init_line_program() {
    if (line_program_)
        return true;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, LINE_VERTEX_SHADER);
    if (!vs)
        return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, LINE_FRAGMENT_SHADER);
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLchar log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        spdlog::error("[GCode GLES] Line program link failed: {}", log);
        glDeleteProgram(prog);
        return false;
    }

    line_program_ = prog;
    line_u_mvp_ = glGetUniformLocation(prog, "u_mvp");
    line_u_color_ = glGetUniformLocation(prog, "u_color");
    line_a_position_ = glGetAttribLocation(prog, "a_position");

    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    line_vbo_ = vbo;
    return true;
}

void GCodeGLESRenderer::render_brackets_3d(const ParsedGCodeFile& gcode, const glm::mat4& mvp) {
    if (highlighted_objects_.empty())
        return;
    if (!line_program_ && !init_line_program())
        return;

    // Collect line endpoints (one vec3 per vertex, two verts per line).
    // 8 corners * 3 axes * 2 verts = 48 vertices per object.
    std::vector<glm::vec3> verts;
    verts.reserve(highlighted_objects_.size() * 48);

    for (const auto& name : highlighted_objects_) {
        auto it = gcode.objects.find(name);
        if (it == gcode.objects.end())
            continue;
        const AABB& bbox = it->second.bounding_box;
        if (bbox.is_empty())
            continue;

        // Run the bbox through the same quantization the geometry pipeline
        // applied to vertices, so brackets sit on the rendered surface rather
        // than offset by a fraction of a quantization step.
        glm::vec3 bmin = bbox.min;
        glm::vec3 bmax = bbox.max;
        if (geometry_) {
            const auto& quant = geometry_->quantization;
            bmin = quant.dequantize_vec3(quant.quantize_vec3(bbox.min));
            bmax = quant.dequantize_vec3(quant.quantize_vec3(bbox.max));
        }

        const float dx = bmax.x - bmin.x;
        const float dy = bmax.y - bmin.y;
        const float dz = bmax.z - bmin.z;
        const float min_edge = std::min({dx, dy, dz});
        // 20% of shortest edge, capped at 5mm — matches the 2D layer renderer
        // and the deleted TinyGL impl, so 2D and 3D bracket sizing feel alike.
        const float bracket_len = std::min(min_edge * 0.2f, 5.0f);
        if (bracket_len < 0.01f)
            continue;

        AABB{bmin, bmax}.for_each_bracket_arm(bracket_len,
                                              [&](const glm::vec3& origin, const glm::vec3& tip) {
                                                  verts.push_back(origin);
                                                  verts.push_back(tip);
                                              });
    }

    if (verts.empty())
        return;

    // Draw on top of existing geometry. Brackets are unlit and uniform color.
    glDisable(GL_DEPTH_TEST);
    glUseProgram(line_program_);
    glUniformMatrix4fv(line_u_mvp_, 1, GL_FALSE, glm::value_ptr(mvp));
    // #C0C0C0 silver, fully opaque — matches the 2D bracket color.
    glUniform4f(line_u_color_, 0.75f, 0.75f, 0.75f, 1.0f);

    glBindBuffer(GL_ARRAY_BUFFER, line_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(glm::vec3)),
                 verts.data(), GL_STREAM_DRAW);

    glEnableVertexAttribArray(static_cast<GLuint>(line_a_position_));
    glVertexAttribPointer(static_cast<GLuint>(line_a_position_), 3, GL_FLOAT, GL_FALSE,
                          sizeof(glm::vec3), nullptr);

    glLineWidth(2.0f);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.size()));

    glDisableVertexAttribArray(static_cast<GLuint>(line_a_position_));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}

// ============================================================
// Object Picking (CPU-side, no GL needed)
// ============================================================

std::optional<std::string> GCodeGLESRenderer::pick_object(const glm::vec2& screen_pos,
                                                          const ParsedGCodeFile& gcode,
                                                          const GCodeCamera& camera) const {
    glm::mat4 transform = build_mvp(camera);
    float closest_distance = std::numeric_limits<float>::max();
    std::optional<std::string> picked_object;

    constexpr float PICK_THRESHOLD = PICK_THRESHOLD_PX;

    int ls = layer_start_;
    int le = (layer_end_ < 0 || layer_end_ >= static_cast<int>(gcode.layers.size()))
                 ? static_cast<int>(gcode.layers.size()) - 1
                 : layer_end_;

    for (int layer_idx = ls; layer_idx <= le; ++layer_idx) {
        if (layer_idx < 0 || layer_idx >= static_cast<int>(gcode.layers.size()))
            continue;
        const auto& layer = gcode.layers[static_cast<size_t>(layer_idx)];

        for (const auto& segment : layer.segments) {
            if (!segment.is_extrusion || !show_extrusions_)
                continue;
            if (segment.object_name_index < 0)
                continue;

            glm::vec4 start_clip = transform * glm::vec4(segment.start, 1.0f);
            glm::vec4 end_clip = transform * glm::vec4(segment.end, 1.0f);

            if (std::abs(start_clip.w) < CLIP_SPACE_W_EPSILON ||
                std::abs(end_clip.w) < CLIP_SPACE_W_EPSILON)
                continue;

            glm::vec3 start_ndc = glm::vec3(start_clip) / start_clip.w;
            glm::vec3 end_ndc = glm::vec3(end_clip) / end_clip.w;

            if (start_ndc.x < -1 || start_ndc.x > 1 || start_ndc.y < -1 || start_ndc.y > 1 ||
                end_ndc.x < -1 || end_ndc.x > 1 || end_ndc.y < -1 || end_ndc.y > 1) {
                continue;
            }

            glm::vec2 start_screen((start_ndc.x + 1) * 0.5f * viewport_width_,
                                   (1 - start_ndc.y) * 0.5f * viewport_height_);
            glm::vec2 end_screen((end_ndc.x + 1) * 0.5f * viewport_width_,
                                 (1 - end_ndc.y) * 0.5f * viewport_height_);

            glm::vec2 v = end_screen - start_screen;
            glm::vec2 w = screen_pos - start_screen;
            float len_sq = glm::dot(v, v);
            float t = (len_sq > 0.0001f) ? std::clamp(glm::dot(w, v) / len_sq, 0.0f, 1.0f) : 0.0f;
            float dist = glm::length(screen_pos - (start_screen + t * v));

            if (dist < PICK_THRESHOLD && dist < closest_distance) {
                closest_distance = dist;
                picked_object = gcode.get_object_name(segment.object_name_index);
            }
        }
    }

    // Fallback: after 3D geometry build, ParsedGCodeFile::clear_segments() frees
    // per-layer toolpath data to save memory — segment iteration above then
    // returns nothing. Project each defined object's AABB to screen space and
    // hit-test the resulting 2D bounding rect so taps still work post-build.
    if (!picked_object) {
        float closest_inside_area = std::numeric_limits<float>::max();
        for (const auto& [name, obj] : gcode.objects) {
            const auto& bbox = obj.bounding_box;
            if (bbox.is_empty())
                continue;

            const auto corners = bbox.corners();

            float min_sx = std::numeric_limits<float>::max();
            float min_sy = std::numeric_limits<float>::max();
            float max_sx = std::numeric_limits<float>::lowest();
            float max_sy = std::numeric_limits<float>::lowest();
            bool any_in_front = false;

            for (const auto& corner : corners) {
                glm::vec4 clip = transform * glm::vec4(corner, 1.0f);
                if (clip.w <= CLIP_SPACE_W_EPSILON)
                    continue; // behind the camera
                any_in_front = true;
                glm::vec3 ndc = glm::vec3(clip) / clip.w;
                float sx = (ndc.x + 1.0f) * 0.5f * viewport_width_;
                float sy = (1.0f - ndc.y) * 0.5f * viewport_height_;
                min_sx = std::min(min_sx, sx);
                min_sy = std::min(min_sy, sy);
                max_sx = std::max(max_sx, sx);
                max_sy = std::max(max_sy, sy);
            }
            if (!any_in_front)
                continue;

            if (screen_pos.x < min_sx || screen_pos.x > max_sx || screen_pos.y < min_sy ||
                screen_pos.y > max_sy)
                continue;

            // Prefer the smallest (innermost) hit when objects overlap on screen.
            float area = (max_sx - min_sx) * (max_sy - min_sy);
            if (area < closest_inside_area) {
                closest_inside_area = area;
                picked_object = name;
            }
        }
    }

    return picked_object;
}

} // namespace gcode
} // namespace helix

#endif // ENABLE_GLES_3D
