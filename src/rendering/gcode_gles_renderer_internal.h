// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gcode_gles_renderer.h"

#ifdef ENABLE_GLES_3D

#include "color_utils.h"
#include "core/data_root_resolver.h"
#include "gcode_gl_fallback.h"
#include "gcode_projection.h"
#include "gcode_render_schedule.h"
#include "gcode_selection_style.h"
#include "lv_draw_buf_guard.h"
#include "runtime_config.h"
#include "scoped_guard_file.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>

// GL backend selection: SDL_GL on desktop (LV_USE_SDL), EGL+GBM on embedded
#if LV_USE_SDL
#include <SDL.h>
#else
#include "gpu_libs.h"

#include <EGL/egl.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>
#endif

// GLES2 is resolved at runtime on every backend, never linked
#include "gles2_loader.h"
using namespace helix::gles2;
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace helix {
namespace gcode {

/// Check for GL errors after significant GPU operations.
/// Returns true if no error, false on error (with spdlog output).
inline bool check_gl_error(const char* operation) {
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

/// Compiles one shader stage; returns 0 on failure.
GLuint compile_shader(GLenum type, const char* source);

/// GLSL helper shared by the main and shell vertex shaders.
extern const char* kOctDecodeGlsl;

} // namespace gcode
} // namespace helix

#endif // ENABLE_GLES_3D
