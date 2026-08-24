// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "backdrop_blur.h"

#include "config.h"
#include "data_root_resolver.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#ifdef ENABLE_GLES_3D
#if LV_USE_SDL
// SDL desktop: CPU blur only (no GL context juggling needed)
#else
// DRM+EGL embedded: GPU blur path
#define BACKDROP_BLUR_GPU 1
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>
#endif
#endif

namespace helix::ui {

// ============================================================================
// Circuit Breaker
// ============================================================================

static bool s_blur_disabled = false;

namespace detail {

void reset_circuit_breaker() {
    s_blur_disabled = false;
}

bool is_blur_disabled() {
    return s_blur_disabled;
}

} // namespace detail

// ============================================================================
// CPU Box Blur (all platforms)
// ============================================================================

namespace detail {

void box_blur_argb8888(uint8_t* data, int width, int height, int iterations) {
    if (!data || width < 1 || height < 1 || iterations <= 0) {
        return;
    }

    const int stride = width * 4;
    std::vector<uint8_t> tmp(static_cast<size_t>(width) * height * 4);

    for (int iter = 0; iter < iterations; iter++) {
        // Horizontal pass: data -> tmp
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int sum[4] = {0, 0, 0, 0};
                int count = 0;
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx;
                    if (nx >= 0 && nx < width) {
                        int idx = y * stride + nx * 4;
                        sum[0] += data[idx + 0];
                        sum[1] += data[idx + 1];
                        sum[2] += data[idx + 2];
                        sum[3] += data[idx + 3];
                        count++;
                    }
                }
                int out_idx = y * stride + x * 4;
                tmp[out_idx + 0] = static_cast<uint8_t>(sum[0] / count);
                tmp[out_idx + 1] = static_cast<uint8_t>(sum[1] / count);
                tmp[out_idx + 2] = static_cast<uint8_t>(sum[2] / count);
                tmp[out_idx + 3] = static_cast<uint8_t>(sum[3] / count);
            }
        }

        // Vertical pass: tmp -> data
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int sum[4] = {0, 0, 0, 0};
                int count = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    int ny = y + dy;
                    if (ny >= 0 && ny < height) {
                        int idx = ny * stride + x * 4;
                        sum[0] += tmp[idx + 0];
                        sum[1] += tmp[idx + 1];
                        sum[2] += tmp[idx + 2];
                        sum[3] += tmp[idx + 3];
                        count++;
                    }
                }
                int out_idx = y * stride + x * 4;
                data[out_idx + 0] = static_cast<uint8_t>(sum[0] / count);
                data[out_idx + 1] = static_cast<uint8_t>(sum[1] / count);
                data[out_idx + 2] = static_cast<uint8_t>(sum[2] / count);
                data[out_idx + 3] = static_cast<uint8_t>(sum[3] / count);
            }
        }
    }
}

void downscale_2x_argb8888(const uint8_t* src, uint8_t* dst, int src_width, int src_height,
                           int src_stride) {
    if (!src || !dst || src_width < 2 || src_height < 2) {
        return;
    }

    const int dst_width = src_width / 2;
    const int dst_height = src_height / 2;

    for (int dy = 0; dy < dst_height; dy++) {
        for (int dx = 0; dx < dst_width; dx++) {
            int sx = dx * 2;
            int sy = dy * 2;

            // Average 2x2 block (use src_stride for row offset, not width*4)
            const uint8_t* p00 = src + sy * src_stride + sx * 4;
            const uint8_t* p10 = p00 + 4;
            const uint8_t* p01 = p00 + src_stride;
            const uint8_t* p11 = p01 + 4;

            int out_idx = (dy * dst_width + dx) * 4;
            dst[out_idx + 0] = static_cast<uint8_t>((p00[0] + p10[0] + p01[0] + p11[0]) / 4);
            dst[out_idx + 1] = static_cast<uint8_t>((p00[1] + p10[1] + p01[1] + p11[1]) / 4);
            dst[out_idx + 2] = static_cast<uint8_t>((p00[2] + p10[2] + p01[2] + p11[2]) / 4);
            dst[out_idx + 3] = static_cast<uint8_t>((p00[3] + p10[3] + p01[3] + p11[3]) / 4);
        }
    }
}

void darken_argb8888_inplace(uint8_t* data, int width, int height, int stride,
                             lv_opa_t dim_opacity) {
    if (!data || width < 1 || height < 1) {
        return;
    }

    // Scale factor: (255 - dim_opacity). We multiply each RGB channel by this
    // and divide by 255 to darken the image as if a black overlay with
    // dim_opacity were composited on top.
    const uint32_t scale = 255 - dim_opacity;

    for (int y = 0; y < height; y++) {
        uint8_t* row = data + y * stride;
        for (int x = 0; x < width; x++) {
            uint8_t* pixel = row + x * 4;
            // LVGL ARGB8888 byte order: B=0, G=1, R=2, A=3
            pixel[0] = static_cast<uint8_t>((pixel[0] * scale) / 255);
            pixel[1] = static_cast<uint8_t>((pixel[1] * scale) / 255);
            pixel[2] = static_cast<uint8_t>((pixel[2] * scale) / 255);
            pixel[3] = 255; // Fully opaque
        }
    }
}

} // namespace detail

// ============================================================================
// GPU Blur (DRM+EGL only)
// ============================================================================

#ifdef BACKDROP_BLUR_GPU

namespace {

// Cached GL state for blur pipeline
struct GpuBlurState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    struct gbm_device* gbm = nullptr;
    int drm_fd = -1;

    GLuint program = 0;
    GLuint vbo = 0;
    GLuint fbo[2] = {};
    GLuint tex[2] = {};

    GLint u_texture = -1;
    GLint u_texel_size = -1;
    GLint u_direction = -1;
    GLint a_position = -1;

    bool initialized = false;
    // Sticky after init_gpu_blur() returns false. Without this every backdrop
    // (modal open, overlay push) re-attempts EGL init and logs "Could not
    // initialize EGL" again on devices that don't have a usable GL stack
    // (Pi DSI dumb buffers, headless render nodes locked down by perms, etc.).
    bool init_failed = false;
};

static GpuBlurState s_gpu;

static const char* BLUR_VERTEX_SHADER = R"(
    attribute vec2 a_position;
    varying vec2 v_uv;
    void main() {
        v_uv = a_position * 0.5 + 0.5;
        gl_Position = vec4(a_position, 0.0, 1.0);
    }
)";

static const char* BLUR_FRAGMENT_SHADER = R"(
    precision mediump float;
    uniform sampler2D u_texture;
    uniform vec2 u_texel_size;
    uniform vec2 u_direction;
    varying vec2 v_uv;

    void main() {
        // 9-tap Gaussian (sigma ~2.5)
        float weight[5];
        weight[0] = 0.2270270270;
        weight[1] = 0.1945945946;
        weight[2] = 0.1216216216;
        weight[3] = 0.0540540541;
        weight[4] = 0.0162162162;

        vec3 result = texture2D(u_texture, v_uv).rgb * weight[0];
        for (int i = 1; i < 5; i++) {
            vec2 offset = u_direction * u_texel_size * float(i);
            result += texture2D(u_texture, v_uv + offset).rgb * weight[i];
            result += texture2D(u_texture, v_uv - offset).rgb * weight[i];
        }
        gl_FragColor = vec4(result, 1.0);
    }
)";

static bool check_gl(const char* op) {
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        spdlog::error("[Backdrop Blur] GL error after {}: 0x{:04X}", op, err);
        return false;
    }
    return true;
}

static GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        spdlog::error("[Backdrop Blur] Shader compile error: {}", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool init_gpu_blur() {
    if (s_gpu.initialized)
        return true;

    // Persistent crash-loop block: a prior session that hard-faulted inside the
    // Mali/EGL init promoted /display/gpu_blur_blocked. Honor it before touching
    // any DRM device and stay on the CPU blur path for the whole session.
    if (Config::get_instance()->get<bool>("/display/gpu_blur_blocked", false)) {
        spdlog::warn("[Backdrop Blur] GPU blur blocked by /display/gpu_blur_blocked (prior driver "
                     "crash) — using CPU blur");
        s_gpu.init_failed = true;
        return false;
    }

    // DRM device paths to try
    static constexpr const char* DRM_DEVICES[] = {"/dev/dri/renderD128", "/dev/dri/card1",
                                                  "/dev/dri/card0"};

    // Arm the crash-loop guard immediately before the risky Mali/EGL init. If the
    // driver hard-faults below (an in-driver SIGSEGV that returns no error we can
    // check), the file survives and Application promotes it to a persistent block
    // on the next launch.
    const std::string guard_path = helix::writable_path("gpu_blur_guard");
    {
        std::ofstream guard(guard_path, std::ios::out | std::ios::trunc);
        if (guard.is_open()) {
            guard << "1";
            spdlog::debug("[Backdrop Blur] Armed GPU crash-loop guard: {}", guard_path);
        } else {
            spdlog::warn("[Backdrop Blur] Could not write GPU crash-loop guard: {}", guard_path);
        }
    }

    // Remove the guard on ANY normal return — success OR a graceful failure (no
    // usable GL device, EGL init error, shader/link failure). Only an in-driver
    // SIGSEGV, which unwinds no stack, leaves the file behind so the next launch
    // can promote it to a persistent block. A graceful failure must NOT promote:
    // the caller already sets init_failed and falls back to CPU blur this session.
    struct GuardCleaner {
        const std::string& path;
        ~GuardCleaner() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } guard_cleaner{guard_path};

    for (const char* path : DRM_DEVICES) {
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        auto* gbm = gbm_create_device(fd);
        if (!gbm) {
            close(fd);
            continue;
        }

        auto display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(gbm));
        if (display == EGL_NO_DISPLAY) {
            gbm_device_destroy(gbm);
            close(fd);
            continue;
        }

        EGLint major, minor;
        if (!eglInitialize(display, &major, &minor)) {
            gbm_device_destroy(gbm);
            close(fd);
            continue;
        }

        eglBindAPI(EGL_OPENGL_ES_API);

        EGLint config_attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
        EGLConfig config;
        EGLint num_configs;
        if (!eglChooseConfig(display, config_attribs, &config, 1, &num_configs) ||
            num_configs == 0) {
            eglTerminate(display);
            gbm_device_destroy(gbm);
            close(fd);
            continue;
        }

        EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
        if (context == EGL_NO_CONTEXT) {
            eglTerminate(display);
            gbm_device_destroy(gbm);
            close(fd);
            continue;
        }

        // Try surfaceless first, then PBuffer
        EGLSurface egl_surface = EGL_NO_SURFACE;
        const char* exts = eglQueryString(display, EGL_EXTENSIONS);
        bool has_surfaceless = exts && strstr(exts, "EGL_KHR_surfaceless_context") != nullptr;

        if (has_surfaceless) {
            if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
                has_surfaceless = false;
            }
        }

        if (!has_surfaceless) {
            EGLint pbuf_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
            egl_surface = eglCreatePbufferSurface(display, config, pbuf_attribs);
            if (egl_surface == EGL_NO_SURFACE ||
                !eglMakeCurrent(display, egl_surface, egl_surface, context)) {
                if (egl_surface != EGL_NO_SURFACE)
                    eglDestroySurface(display, egl_surface);
                eglDestroyContext(display, context);
                eglTerminate(display);
                gbm_device_destroy(gbm);
                close(fd);
                continue;
            }
        }

        s_gpu.display = display;
        s_gpu.context = context;
        s_gpu.surface = egl_surface;
        s_gpu.gbm = gbm;
        s_gpu.drm_fd = fd;

        spdlog::info("[Backdrop Blur] EGL context ready via {}", path);
        break;
    }

    if (s_gpu.context == EGL_NO_CONTEXT) {
        spdlog::warn("[Backdrop Blur] Could not initialize EGL — falling back to CPU blur");
        return false;
    }

    // Helper: clean up partially-initialized GPU state on failure
    auto cleanup_on_fail = []() {
        if (s_gpu.program) {
            glDeleteProgram(s_gpu.program);
            s_gpu.program = 0;
        }
        if (s_gpu.vbo) {
            glDeleteBuffers(1, &s_gpu.vbo);
            s_gpu.vbo = 0;
        }
        glDeleteFramebuffers(2, s_gpu.fbo);
        glDeleteTextures(2, s_gpu.tex);
        s_gpu.fbo[0] = s_gpu.fbo[1] = 0;
        s_gpu.tex[0] = s_gpu.tex[1] = 0;

        eglMakeCurrent(s_gpu.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (s_gpu.surface != EGL_NO_SURFACE)
            eglDestroySurface(s_gpu.display, s_gpu.surface);
        eglDestroyContext(s_gpu.display, s_gpu.context);
        eglTerminate(s_gpu.display);
        if (s_gpu.gbm)
            gbm_device_destroy(s_gpu.gbm);
        if (s_gpu.drm_fd >= 0)
            close(s_gpu.drm_fd);
        s_gpu = {};
    };

    // Compile shaders
    GLuint vs = compile_shader(GL_VERTEX_SHADER, BLUR_VERTEX_SHADER);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, BLUR_FRAGMENT_SHADER);
    if (!vs || !fs) {
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        cleanup_on_fail();
        return false;
    }

    s_gpu.program = glCreateProgram();
    glAttachShader(s_gpu.program, vs);
    glAttachShader(s_gpu.program, fs);
    glLinkProgram(s_gpu.program);

    GLint ok = 0;
    glGetProgramiv(s_gpu.program, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!ok) {
        char log[512];
        glGetProgramInfoLog(s_gpu.program, sizeof(log), nullptr, log);
        spdlog::error("[Backdrop Blur] Program link error: {}", log);
        cleanup_on_fail();
        return false;
    }

    s_gpu.u_texture = glGetUniformLocation(s_gpu.program, "u_texture");
    s_gpu.u_texel_size = glGetUniformLocation(s_gpu.program, "u_texel_size");
    s_gpu.u_direction = glGetUniformLocation(s_gpu.program, "u_direction");
    s_gpu.a_position = glGetAttribLocation(s_gpu.program, "a_position");

    // Fullscreen quad VBO
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenBuffers(1, &s_gpu.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_gpu.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    // Create 2 FBOs + textures for ping-pong blur
    glGenFramebuffers(2, s_gpu.fbo);
    glGenTextures(2, s_gpu.tex);

    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, s_gpu.tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindFramebuffer(GL_FRAMEBUFFER, s_gpu.fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_gpu.tex[i],
                               0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (!check_gl("init_gpu_blur setup")) {
        cleanup_on_fail();
        return false;
    }

    // Release context (will re-acquire via guard when blurring)
    eglMakeCurrent(s_gpu.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    s_gpu.initialized = true;

    // Pipeline is up — the Mali/EGL init survived. The GuardCleaner above removes
    // the crash-loop guard as this function returns (normally), so a clean run
    // doesn't look like a mid-init fault on the next launch.
    spdlog::debug("[Backdrop Blur] GPU blur pipeline initialized");
    return true;
}

static void destroy_gpu_blur() {
    if (!s_gpu.initialized || s_gpu.display == EGL_NO_DISPLAY)
        return;

    // Acquire context for cleanup
    auto surface = s_gpu.surface != EGL_NO_SURFACE ? s_gpu.surface : EGL_NO_SURFACE;
    eglMakeCurrent(s_gpu.display, surface, surface, s_gpu.context);

    if (s_gpu.vbo)
        glDeleteBuffers(1, &s_gpu.vbo);
    glDeleteFramebuffers(2, s_gpu.fbo);
    glDeleteTextures(2, s_gpu.tex);
    if (s_gpu.program)
        glDeleteProgram(s_gpu.program);

    eglMakeCurrent(s_gpu.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (s_gpu.surface != EGL_NO_SURFACE)
        eglDestroySurface(s_gpu.display, s_gpu.surface);
    eglDestroyContext(s_gpu.display, s_gpu.context);
    eglTerminate(s_gpu.display);

    if (s_gpu.gbm)
        gbm_device_destroy(s_gpu.gbm);
    if (s_gpu.drm_fd >= 0)
        close(s_gpu.drm_fd);

    s_gpu = {};
    spdlog::debug("[Backdrop Blur] GPU resources cleaned up");
}

/// Run 2-pass Gaussian blur on the GPU. Returns true on success.
/// Input: ARGB8888 buffer. Output: same buffer, blurred.
static bool gpu_blur(uint8_t* data, int width, int height) {
    if (s_gpu.init_failed)
        return false;
    if (!s_gpu.initialized && !init_gpu_blur()) {
        s_gpu.init_failed = true;
        return false;
    }

    // Save and restore previous EGL context
    auto saved_display = eglGetCurrentDisplay();
    auto saved_context = eglGetCurrentContext();
    auto saved_draw = eglGetCurrentSurface(EGL_DRAW);
    auto saved_read = eglGetCurrentSurface(EGL_READ);

    if (saved_context != EGL_NO_CONTEXT) {
        eglMakeCurrent(saved_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    auto egl_surface = s_gpu.surface != EGL_NO_SURFACE ? s_gpu.surface : EGL_NO_SURFACE;
    if (!eglMakeCurrent(s_gpu.display, egl_surface, egl_surface, s_gpu.context)) {
        spdlog::error("[Backdrop Blur] Failed to acquire EGL context for blur");
        if (saved_context != EGL_NO_CONTEXT)
            eglMakeCurrent(saved_display, saved_draw, saved_read, saved_context);
        return false;
    }

    // Upload source data to texture 0
    glBindTexture(GL_TEXTURE_2D, s_gpu.tex[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    // Also allocate texture 1 at the same size
    glBindTexture(GL_TEXTURE_2D, s_gpu.tex[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glUseProgram(s_gpu.program);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUniform1i(s_gpu.u_texture, 0);
    glUniform2f(s_gpu.u_texel_size, 1.0f / width, 1.0f / height);

    glBindBuffer(GL_ARRAY_BUFFER, s_gpu.vbo);
    glEnableVertexAttribArray(static_cast<GLuint>(s_gpu.a_position));
    glVertexAttribPointer(static_cast<GLuint>(s_gpu.a_position), 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    // 2 iterations (4 passes) for stronger blur
    for (int iter = 0; iter < 2; iter++) {
        // Horizontal: tex[0] -> fbo[1]/tex[1]
        glBindFramebuffer(GL_FRAMEBUFFER, s_gpu.fbo[1]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_gpu.tex[0]);
        glUniform2f(s_gpu.u_direction, 1.0f, 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // Vertical: tex[1] -> fbo[0]/tex[0]
        glBindFramebuffer(GL_FRAMEBUFFER, s_gpu.fbo[0]);
        glBindTexture(GL_TEXTURE_2D, s_gpu.tex[1]);
        glUniform2f(s_gpu.u_direction, 0.0f, 1.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // Read back result from tex[0] (bound to fbo[0])
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data);

    bool ok = check_gl("gpu_blur");

    // Cleanup state
    glDisableVertexAttribArray(static_cast<GLuint>(s_gpu.a_position));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Restore previous EGL context
    eglMakeCurrent(s_gpu.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (saved_context != EGL_NO_CONTEXT) {
        eglMakeCurrent(saved_display, saved_draw, saved_read, saved_context);
    }

    return ok;
}

} // anonymous namespace

#endif // BACKDROP_BLUR_GPU

// ============================================================================
// Snapshot + Blur Pipeline
// ============================================================================

/// Event callback to free the draw_buf when the image widget is deleted.
static void on_backdrop_image_deleted(lv_event_t* e) {
    auto* buf = static_cast<lv_draw_buf_t*>(lv_event_get_user_data(e));
    if (buf) {
        lv_draw_buf_destroy(buf);
        spdlog::trace("[Backdrop Blur] Freed backdrop draw buffer");
    }
}

lv_obj_t* create_blurred_backdrop(lv_obj_t* parent, lv_opa_t dim_opacity) {
    if (s_blur_disabled) {
        return nullptr;
    }

    if (!parent) {
        spdlog::warn("[Backdrop Blur] Null parent — disabling blur permanently");
        s_blur_disabled = true;
        return nullptr;
    }

#if LV_COLOR_DEPTH == 16
    // RGB565 devices: skip blur — use darkened snapshot instead.
    // lv_snapshot_take with ARGB8888 works on all color depths.
    return create_darkened_backdrop(parent, dim_opacity);
#endif

    // Step 1: Snapshot current screen
    lv_obj_t* screen = lv_screen_active();
    if (!screen) {
        spdlog::warn("[Backdrop Blur] No active screen — disabling blur permanently");
        s_blur_disabled = true;
        return nullptr;
    }

    lv_draw_buf_t* snapshot = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);
    if (!snapshot) {
        spdlog::warn("[Backdrop Blur] Snapshot failed — disabling blur permanently");
        s_blur_disabled = true;
        return nullptr;
    }

    int snap_w = static_cast<int>(snapshot->header.w);
    int snap_h = static_cast<int>(snapshot->header.h);
    auto* snap_data = static_cast<uint8_t*>(snapshot->data);

    spdlog::debug("[Backdrop Blur] Snapshot {}x{}", snap_w, snap_h);

    // Step 2: Downscale 2x
    int blur_w = snap_w / 2;
    int blur_h = snap_h / 2;

    if (blur_w < 2 || blur_h < 2) {
        // Too small to downscale — blur on original
        blur_w = snap_w;
        blur_h = snap_h;
    }

    // Allocate downscaled buffer
    std::vector<uint8_t> blur_buf;
    uint8_t* blur_data;

    if (blur_w != snap_w) {
        blur_buf.resize(static_cast<size_t>(blur_w) * blur_h * 4);
        detail::downscale_2x_argb8888(snap_data, blur_buf.data(), snap_w, snap_h,
                                      static_cast<int>(snapshot->header.stride));
        blur_data = blur_buf.data();
    } else {
        // No downscale; copy into tight-stride buffer since box_blur uses width*4
        uint32_t snap_stride = snapshot->header.stride;
        uint32_t tight_stride = static_cast<uint32_t>(snap_w) * 4;
        blur_buf.resize(static_cast<size_t>(tight_stride) * snap_h);
        if (snap_stride == tight_stride) {
            std::memcpy(blur_buf.data(), snap_data, blur_buf.size());
        } else {
            for (int y = 0; y < snap_h; y++) {
                std::memcpy(blur_buf.data() + y * tight_stride, snap_data + y * snap_stride,
                            tight_stride);
            }
        }
        blur_data = blur_buf.data();
    }

    // Step 3: Blur
    bool blurred = false;

#ifdef BACKDROP_BLUR_GPU
    blurred = gpu_blur(blur_data, blur_w, blur_h);
    if (!blurred) {
        spdlog::debug("[Backdrop Blur] GPU blur failed, falling back to CPU");
    }
#endif

    if (!blurred) {
        detail::box_blur_argb8888(blur_data, blur_w, blur_h, 3);
    }

    // Apply dimming directly to blur pixels (eliminates per-frame opacity overlay)
    detail::darken_argb8888_inplace(blur_data, blur_w, blur_h, blur_w * 4, dim_opacity);

    // Step 4: Create lv_draw_buf for the blurred result at display resolution.
    // LVGL's lv_image will scale the smaller buffer up automatically when we
    // set it as source on a full-screen image widget.
    lv_draw_buf_t* result_buf = lv_draw_buf_create(
        static_cast<uint32_t>(blur_w), static_cast<uint32_t>(blur_h), LV_COLOR_FORMAT_ARGB8888, 0);
    if (!result_buf) {
        spdlog::warn("[Backdrop Blur] Failed to allocate result buffer — disabling blur");
        lv_draw_buf_destroy(snapshot);
        s_blur_disabled = true;
        return nullptr;
    }

    // Copy row-by-row: blur_data uses tight stride (blur_w * 4) but result_buf
    // uses LVGL's aligned stride (rounded up to LV_DRAW_BUF_STRIDE_ALIGN).
    // A flat memcpy shifts every row when the strides differ, causing diagonal smearing.
    {
        uint32_t tight_stride = static_cast<uint32_t>(blur_w) * 4;
        uint32_t aligned_stride = result_buf->header.stride;
        if (tight_stride == aligned_stride) {
            std::memcpy(result_buf->data, blur_data, static_cast<size_t>(blur_w) * blur_h * 4);
        } else {
            auto* dst = static_cast<uint8_t*>(result_buf->data);
            for (int y = 0; y < blur_h; y++) {
                std::memcpy(dst + y * aligned_stride, blur_data + y * tight_stride, tight_stride);
            }
        }
    }

    // Done with snapshot
    lv_draw_buf_destroy(snapshot);

    // Step 5: Create image widget
    lv_obj_t* img = lv_image_create(parent);
    lv_obj_set_size(img, LV_PCT(100), LV_PCT(100));
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    lv_image_set_src(img, result_buf);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);

    // Free draw buffer when image is deleted
    lv_obj_add_event_cb(img, on_backdrop_image_deleted, LV_EVENT_DELETE, result_buf);

    spdlog::debug("[Backdrop Blur] Created blurred backdrop ({}x{} blur, dim_opacity={})", blur_w,
                  blur_h, dim_opacity);
    return img;
}

lv_obj_t* create_darkened_backdrop(lv_obj_t* parent, lv_opa_t dim_opacity) {
    if (!parent) {
        spdlog::warn("[Backdrop Darken] Null parent");
        return nullptr;
    }

    // Step 1: Snapshot current screen
    lv_obj_t* screen = lv_screen_active();
    if (!screen) {
        spdlog::warn("[Backdrop Darken] No active screen");
        return nullptr;
    }

    lv_draw_buf_t* snapshot = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);
    if (!snapshot) {
        spdlog::warn("[Backdrop Darken] Snapshot failed");
        return nullptr;
    }

    int snap_w = static_cast<int>(snapshot->header.w);
    int snap_h = static_cast<int>(snapshot->header.h);
    auto* snap_data = static_cast<uint8_t*>(snapshot->data);
    int snap_stride = static_cast<int>(snapshot->header.stride);

    spdlog::debug("[Backdrop Darken] Snapshot {}x{} (stride={})", snap_w, snap_h, snap_stride);

    // Step 2: Darken the snapshot pixels in-place
    detail::darken_argb8888_inplace(snap_data, snap_w, snap_h, snap_stride, dim_opacity);

    // Step 3: Create lv_draw_buf and copy (row-by-row for stride alignment)
    lv_draw_buf_t* result_buf = lv_draw_buf_create(
        static_cast<uint32_t>(snap_w), static_cast<uint32_t>(snap_h), LV_COLOR_FORMAT_ARGB8888, 0);
    if (!result_buf) {
        spdlog::warn("[Backdrop Darken] Failed to allocate result buffer");
        lv_draw_buf_destroy(snapshot);
        return nullptr;
    }

    {
        uint32_t src_stride = static_cast<uint32_t>(snap_stride);
        uint32_t dst_stride = result_buf->header.stride;
        uint32_t row_bytes = static_cast<uint32_t>(snap_w) * 4;
        auto* dst = static_cast<uint8_t*>(result_buf->data);

        if (src_stride == dst_stride) {
            std::memcpy(dst, snap_data, static_cast<size_t>(dst_stride) * snap_h);
        } else {
            for (int y = 0; y < snap_h; y++) {
                std::memcpy(dst + y * dst_stride, snap_data + y * src_stride, row_bytes);
            }
        }
    }

    // Done with snapshot
    lv_draw_buf_destroy(snapshot);

    // Step 4: Create image widget
    lv_obj_t* img = lv_image_create(parent);
    lv_obj_set_size(img, LV_PCT(100), LV_PCT(100));
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    lv_image_set_src(img, result_buf);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);

    // Free draw buffer when image is deleted
    lv_obj_add_event_cb(img, on_backdrop_image_deleted, LV_EVENT_DELETE, result_buf);

    spdlog::debug("[Backdrop Darken] Created darkened backdrop ({}x{}, dim_opacity={})", snap_w,
                  snap_h, dim_opacity);
    return img;
}

void backdrop_blur_cleanup() {
#ifdef BACKDROP_BLUR_GPU
    destroy_gpu_blur();
#endif
    // Backdrop blur disabled pending stability testing
    s_blur_disabled = true;
    spdlog::debug("[Backdrop Blur] Cleanup complete");
}

} // namespace helix::ui
