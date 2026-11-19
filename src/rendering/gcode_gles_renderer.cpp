// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_gles_renderer_internal.h"

#ifdef ENABLE_GLES_3D

namespace helix {
namespace gcode {

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
// GLSL Shaders
// ============================================================

static const char* kVertexShaderDecls = R"(
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
)";

/// The octahedral normal decoder, kept as ONE piece of GLSL text and spliced into
/// every vertex shader that needs it (the lit pass and the selection shell). A
/// second copy could drift, and a shell extruded along a differently-decoded
/// normal would not wrap the surface it is meant to outline.
const char* kOctDecodeGlsl = R"(
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
)";

static const char* kVertexShaderMain = R"(
    void main() {
        gl_Position = u_mvp * vec4(a_position, 1.0);
        vec3 normal = oct_decode(a_normal);
        // Stacked 0.2 mm layers are sub-pixel at preview sizes: a tube's bright top and dark
        // side alias into a moire, so walls shade as one continuous surface per direction.
        if (abs(normal.z) < 0.9) {
            normal = normalize(vec3(normal.x, normal.y, 0.0));
        }
        v_normal = normalize(u_normal_matrix * normal);
        v_position = (u_model_view * vec4(a_position, 1.0)).xyz;
        v_base_color = mix(u_base_color.rgb, a_color.rgb, u_use_vertex_color) * u_color_scale;
    }
)";

static const char* kFragmentShaderSource = R"(
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
    uniform float u_lift_strength;

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

        // Headroom-proportional lift, the same idea as apply_shading() on the
        // 2D path and for the same reason. base * diffuse is a MULTIPLY: a black
        // filament has nothing to multiply, so all form collapses and the only
        // signal left is the specular term, which peaks around a quarter
        // intensity. The object reads as a flat silhouette.
        //
        // Adding light proportional to what the channel has left (1 - base)
        // gives a dark filament the range it lacks while a light one, having
        // almost no headroom, is left essentially as it was.
        float lit = clamp((diffuse.r + diffuse.g + diffuse.b) / 3.0, 0.0, 1.0);
        vec3 lift = (vec3(1.0) - v_base_color) * lit * u_lift_strength;

        vec3 color = v_base_color * diffuse + lift + vec3(spec * u_specular_intensity);
        gl_FragColor = vec4(color, u_base_alpha);
    }
)";

// ============================================================
// Construction / Destruction
// ============================================================

GCodeGLESRenderer::GCodeGLESRenderer() {
    // Selection cues from ui_xml/gcode_tokens.xml, resolved once here on the
    // main thread — same place and same reason as GCodeLayerRenderer's ctor.
    // The tokens are registered in the theme phase of startup, long before any
    // panel builds the viewer widget that owns this renderer, and the GL passes
    // consume the palette as uniforms rather than re-reading the registry.
    reset_colors();
    spdlog::debug("[GCode GLES] GCodeGLESRenderer created");
}

GCodeGLESRenderer::~GCodeGLESRenderer() {
    // The worker draws with this object's state and GL context: join it before
    // either goes away. GL teardown then runs here, on the thread that set it up.
    worker_.stop();
    destroy_gl();
    release_sdl_video(); // a shader failure in init_gl() leaves destroy_gl() nothing

    // draw_buf_ is handed to the parallel render thread via dsc.src in
    // draw_cached_to_lvgl — same UAF pattern as GCodeLayerRenderer::cache_buf_
    // (#929).
    helix::safe_draw_buf_destroy(draw_buf_, "gles_draw_buf");

    spdlog::trace("[GCode GLES] GCodeGLESRenderer destroyed");
}

// ============================================================
// GL Initialization
// ============================================================

#if !LV_USE_SDL
// Open and initialize a native EGL display, then create the renderer's context
// on it. The renderer owns that display and terminates it on teardown.
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

    if (!create_egl_context(display, label, /*owns_display=*/true)) {
        eglTerminate(display);
        return false;
    }
    return true;
}

// Create the renderer's context on an initialized display, returning true on
// success. On success, egl_display_, egl_context_, and optionally egl_surface_
// are set. Never terminates @p display: on failure the caller decides that.
bool GCodeGLESRenderer::create_egl_context(void* egl_display, const char* label,
                                           bool owns_display) {
    auto display = static_cast<EGLDisplay>(egl_display);
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
        return false;
    }

    // Create context
    EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    auto context = eglCreateContext(display, egl_config, EGL_NO_CONTEXT, ctx_attribs);
    if (context == EGL_NO_CONTEXT) {
        spdlog::debug("[GCode GLES] {} — context creation failed: 0x{:X}", label, eglGetError());
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
    owns_egl_display_ = owns_display;
    spdlog::info("[GCode GLES] Context ready via {} ({})", label,
                 has_surfaceless ? "surfaceless" : "PBuffer");
    return true;
}
#endif // !LV_USE_SDL

void GCodeGLESRenderer::release_sdl_video() {
#if LV_USE_SDL
    if (sdl_video_held_)
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
#endif
    sdl_video_held_ = false;
}

bool GCodeGLESRenderer::init_gl() {
    if (gl_initialized_)
        return true;
    if (gl_init_failed_)
        return false;

    // Layer 2 covers driver init too: loading the GPU driver can fault before any
    // draw is reached. A graceful failure removes the guard on return and the
    // viewer falls back to 2D for this session; only a fault inside leaves it for
    // the next startup to promote.
    const helix::ScopedGuardFile init_guard(helix::writable_path("gpu_3d_guard"));
    if (!init_guard.armed())
        spdlog::warn("[GCode GLES] Could not write GPU crash-loop guard: {}", init_guard.path());

#if LV_USE_SDL
    // Desktop path: use SDL_GL_CreateContext with a hidden window.
    // This avoids SDL_Init(SDL_INIT_VIDEO) on Wayland+AMD poisoning EGL operations.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    // SDL_CreateWindow's own video init has no matching quit; this reference is
    // counted, so releasing it with the window leaves the display's hold alone.
    sdl_video_held_ = SDL_InitSubSystem(SDL_INIT_VIDEO) == 0;
    auto* window = sdl_video_held_ ? SDL_CreateWindow("helix-gles-offscreen", 0, 0, 1, 1,
                                                      SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN)
                                   : nullptr;
    if (!window) {
        spdlog::warn("[GCode GLES] SDL_CreateWindow failed: {}", SDL_GetError());
        release_sdl_video();
        gl_init_failed_ = true;
        return false;
    }

    auto gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        spdlog::warn("[GCode GLES] SDL_GL_CreateContext failed: {}", SDL_GetError());
        SDL_DestroyWindow(window);
        release_sdl_video();
        gl_init_failed_ = true;
        return false;
    }

    // SDL_GL_CreateContext left the context current, which resolving needs.
    if (!helix::gles2::load(SDL_GL_GetProcAddress)) {
        SDL_GL_MakeCurrent(window, nullptr);
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        release_sdl_video();
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
    // 1. The display backend's own EGLDisplay, when one is current
    // 2. GBM/DRM (Pi, embedded — surfaceless FBO rendering)
    // 3. Default EGL display (desktop Linux with X11/Wayland — PBuffer)
    using helix::gpu_libs::Lib;
    if (!helix::gpu_libs::available_all({Lib::Egl, Lib::Gles2}) ||
        !helix::gles2::load(helix::gpu_libs::gles2_proc)) {
        spdlog::info("[GCode GLES] No EGL/GLES2 libraries — GPU rendering unavailable");
        gl_init_failed_ = true;
        return false;
    }
    const bool have_gbm = helix::gpu_libs::available_all({Lib::Gbm, Lib::Drm});
    bool egl_ok = false;

    // An EGL screen already has the GPU driver loaded and a display up. Opening a
    // second device beside it loads the driver a second time, which faults inside
    // the dynamic loader on some Mesa stacks (prestonbrown/helixscreen#1742).
    if (EGLDisplay current = eglGetCurrentDisplay(); current != EGL_NO_DISPLAY) {
        egl_ok = create_egl_context(current, "the display's EGLDisplay", /*owns_display=*/false);
    }

    // Path 2: Try GBM/DRM render nodes first (don't need DRM master, works alongside compositor)
    // Then try card nodes (needed on Pi where render nodes may not exist)
    static const char* kDrmDevices[] = {"/dev/dri/renderD128", "/dev/dri/renderD129",
                                        "/dev/dri/card1", "/dev/dri/card0", nullptr};
    for (int i = 0; have_gbm && kDrmDevices[i] && !egl_ok; ++i) {
        int fd = open(kDrmDevices[i], O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        auto* gbm = gbm_create_device(fd);
        if (!gbm) {
            close(fd);
            continue;
        }

        if (try_egl_display(gbm, kDrmDevices[i])) {
            drm_fd_ = fd;
            gbm_device_ = gbm;
            egl_ok = true;
        } else {
            gbm_device_destroy(gbm);
            close(fd);
        }
    }

    // Path 3: Default EGL display (Mesa on X11/Wayland)
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

GLuint compile_shader(GLenum type, const char* source) {
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

    const std::string vertex_src =
        std::string(kVertexShaderDecls) + kOctDecodeGlsl + kVertexShaderMain;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_src.c_str());
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFragmentShaderSource);
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
    u_lift_strength_ = glGetUniformLocation(program_, "u_lift_strength");
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

    // Each scale switch rebuilds the FBO, so an out-of-memory here would otherwise repeat
    // on every frame against an exhausted driver.
    auto renderbuffer_stored = [this](const char* what) {
        const GLenum err = glGetError();
        if (err == GL_NO_ERROR) {
            return true;
        }
        spdlog::error("[GCode GLES] GL error after {}: 0x{:04X}", what, err);
        destroy_fbo();
        if (gl_draw_error_is_fatal(err)) {
            abandon_gpu(what);
        }
        return false;
    };

    destroy_fbo();

    glGenFramebuffers(1, &fbo_.id);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_.id);
    if (!check_gl_error("glGenFramebuffers/glBindFramebuffer")) {
        destroy_fbo();
        return false;
    }

    // Color renderbuffer — use GL_RGBA8 (8 bits per channel) to match the
    // GL_RGBA/GL_UNSIGNED_BYTE format used by glReadPixels in read_frame().
    // GL_RGBA4 would cause precision loss (4 bits stored, 8 bits read back).
    // GL_RGBA8 is available via OES_rgb8_rgba8 on GLES2 and natively on desktop GL.
    glGenRenderbuffers(1, &color_rbo_.id);
    glBindRenderbuffer(GL_RENDERBUFFER, color_rbo_.id);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, width, height);
    if (!renderbuffer_stored("glRenderbufferStorage(color)")) {
        return false;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_rbo_.id);
    check_gl_error("glFramebufferRenderbuffer(color)");

    // Depth renderbuffer (16-bit)
    glGenRenderbuffers(1, &depth_rbo_.id);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rbo_.id);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
    if (!renderbuffer_stored("glRenderbufferStorage(depth)")) {
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
    log_memory_report("fbo created");
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
        free_vbos(moving_vbos_);
        free_vbos(retired_vbos_);
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
        if (shell_program_) {
            glDeleteProgram(shell_program_);
            shell_program_ = 0;
        }

        // Unbind before destroying
        SDL_GL_MakeCurrent(static_cast<SDL_Window*>(sdl_gl_window_), nullptr);

        SDL_GL_DeleteContext(static_cast<SDL_GLContext>(sdl_gl_context_));
        sdl_gl_context_ = nullptr;

        SDL_DestroyWindow(static_cast<SDL_Window*>(sdl_gl_window_));
        sdl_gl_window_ = nullptr;
        release_sdl_video();

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
    free_vbos(moving_vbos_);
    free_vbos(retired_vbos_);
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
    if (shell_program_) {
        glDeleteProgram(shell_program_);
        shell_program_ = 0;
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
        if (owns_egl_display_)
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

// Creates a static VBO holding `bytes` of `data`. Returns an empty handle when GL reports an
// error, and that error in `err`.
static GLBufferHandle make_vbo(const void* data, size_t bytes, const char* what, GLenum& err) {
    GLBufferHandle vbo;
    glGenBuffers(1, &vbo.id);
    glBindBuffer(GL_ARRAY_BUFFER, vbo.id);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), data, GL_STATIC_DRAW);
    err = glGetError();
    if (err != GL_NO_ERROR) {
        spdlog::error("[GCode GLES] GL error after {}: 0x{:04X}", what, err);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return err == GL_NO_ERROR ? std::move(vbo) : GLBufferHandle();
}

bool GCodeGLESRenderer::upload_layer(const RibbonGeometry& geom, size_t layer,
                                     std::vector<uint8_t>& buf, LayerVBO& out) {
    constexpr size_t kVertexStride = PackedVertex::stride();

    out.vbo = GLBufferHandle();
    out.vertex_count = 0;

    size_t first_strip = 0;
    size_t strip_count = geom.strips.size();

    if (!geom.layer_strip_ranges.empty()) {
        auto [fs, sc] = geom.layer_strip_ranges[layer];
        first_strip = fs;
        strip_count = sc;
    }

    if (strip_count == 0) {
        return true;
    }

    // Use pre-computed buffers if available (prepared on background thread)
    if (!geom.prepared_buffers.empty() && layer < geom.prepared_buffers.size() &&
        geom.prepared_buffers[layer].vertex_count > 0) {
        const auto& prepared = geom.prepared_buffers[layer];
        GLenum err = GL_NO_ERROR;
        GLBufferHandle vbo = make_vbo(prepared.data.data(), prepared.vertex_count * kVertexStride,
                                      "glBufferData (prepared)", err);
        if (!vbo.id) {
            spdlog::error("[GCode GLES] VBO creation failed for layer {} (prepared)", layer);
            return !gl_draw_error_is_fatal(err);
        }
        out.vbo = std::move(vbo);
        out.vertex_count = prepared.vertex_count;
        return true;
    }

    // CPU fallback: expand strips inline. Each strip = 4 vertices -> 2 triangles -> 6 vertices
    // (for GL_TRIANGLES).
    size_t total_verts = strip_count * 6;
    size_t buf_bytes = total_verts * kVertexStride;
    if (buf.size() < buf_bytes) {
        buf.resize(buf_bytes);
    }

    geom.expand_strips(first_strip, strip_count, reinterpret_cast<PackedVertex*>(buf.data()));

    GLenum err = GL_NO_ERROR;
    GLBufferHandle vbo = make_vbo(buf.data(), buf_bytes, "glBufferData", err);
    if (!vbo.id) {
        spdlog::error("[GCode GLES] VBO creation failed for layer {}", layer);
        return !gl_draw_error_is_fatal(err);
    }
    out.vbo = std::move(vbo);
    out.vertex_count = total_verts;
    return true;
}

void GCodeGLESRenderer::upload_geometry(const RibbonGeometry& geom, std::vector<LayerVBO>& vbos) {
    // Lock palette during read to prevent data races with set_tool_color_overrides
    std::lock_guard<std::mutex> lock(palette_mutex_);

    free_vbos(vbos);

    if (geom.strips.empty() || geom.vertices.empty()) {
        return;
    }

    size_t num_layers = geom.layer_strip_ranges.empty() ? 1 : geom.layer_strip_ranges.size();

    vbos.resize(num_layers);

    // Reuse upload buffer across layers (sized to largest layer)
    std::vector<uint8_t> buf;
    for (size_t layer = 0; layer < num_layers; ++layer) {
        if (!upload_layer(geom, layer, buf, vbos[layer])) {
            abandon_gpu("VBO upload");
            return;
        }
    }

    spdlog::debug("[GCode GLES] Uploaded {} layers, {} total strips to VBOs", num_layers,
                  geom.strips.size());
}

void GCodeGLESRenderer::upload_moving_mesh() {
    if (!geometry_ || !geometry_->moving_mesh) {
        return;
    }
    upload_geometry(*geometry_->moving_mesh, moving_vbos_);
    mesh_triangles_ = 0;
    for (const auto& vbo : moving_vbos_) {
        mesh_triangles_ += vbo.vertex_count / 3;
    }
    // Same contract as the main path: everything is on the GPU now, and the
    // CPU fallback expand re-reads the (live, possibly overridden) palette.
    geometry_->moving_mesh->prepared_buffers.clear();
    geometry_->moving_mesh->prepared_buffers.shrink_to_fit();
    spdlog::debug("[GCode GLES] Moving mesh uploaded: {} layers, {} triangles", moving_vbos_.size(),
                  mesh_triangles_);
}

bool GCodeGLESRenderer::upload_geometry_chunk(const RibbonGeometry& geom,
                                              std::vector<LayerVBO>& vbos, size_t& next_layer,
                                              size_t total_layers) {
    // Time budget: 8ms per frame for uploads
    constexpr auto kTimeBudget = std::chrono::milliseconds(8);
    auto start = std::chrono::steady_clock::now();

    // Lock palette during read to prevent data races with set_tool_color_overrides
    std::lock_guard<std::mutex> lock(palette_mutex_);

    // Reuse CPU buffer for layers that don't have prepared data
    std::vector<uint8_t> buf;

    while (next_layer < total_layers) {
        if (!upload_layer(geom, next_layer, buf, vbos[next_layer])) {
            abandon_gpu("VBO upload");
            next_layer = 0;
            return false;
        }
        ++next_layer;

        // Check time budget (check every layer, glBufferData can be slow)
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= kTimeBudget) {
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

// A driver that has run out of memory can fault on the next allocation anyone makes, the
// display's own included, so the buffers already uploaded go back and nothing more is drawn.
void GCodeGLESRenderer::abandon_gpu(const char* during) {
    spdlog::error("[GCode GLES] Fatal GL error during {} — disabling GPU rendering, "
                  "falling back to 2D",
                  during);
    free_vbos(layer_vbos_);
    free_vbos(moving_vbos_);
    gl_render_failed_ = true;
    cancel_job();
}

void GCodeGLESRenderer::free_vbos(std::vector<LayerVBO>& vbos) {
    // RAII handles (GLBufferHandle) call glDeleteBuffers in their destructors
    vbos.clear();
}

} // namespace gcode
} // namespace helix

#endif // ENABLE_GLES_3D
