// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "probe_egl_cmd.h"

#include <cstdio>
#include <cstdlib>

// EGL and GBM are linked only into the DRM builds that carry a GPU path. The
// file itself always compiles: a launcher asking a binary that cannot answer
// must get a refusal, not the "ignoring unknown argument" warning and a booted
// UI. Everything below the guard is the real probe; the stub at the bottom is
// what the fbdev and non-GPU builds get.
#if defined(HELIX_DISPLAY_DRM) && (defined(HELIX_ENABLE_OPENGLES) || defined(ENABLE_GLES_3D))
#define HELIX_PROBE_EGL_AVAILABLE 1
#endif

#ifdef HELIX_PROBE_EGL_AVAILABLE

#include "gcode_gl_fallback.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <gbm.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace helix {
namespace probe {

namespace {

/// A node worth probing drives a screen. Requiring a connected connector keeps
/// the verdict about the presentation path: a render-only node can bring EGL up
/// on the GPU and still be the wrong answer, because it is not what the display
/// backend opens.
bool has_connected_connector(int fd) {
    drmModeRes* res = drmModeGetResources(fd);
    if (res == nullptr) {
        return false;
    }

    bool connected = false;
    for (int i = 0; i < res->count_connectors && !connected; ++i) {
        drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[i]);
        if (conn == nullptr) {
            continue;
        }
        connected = (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0);
        drmModeFreeConnector(conn);
    }

    drmModeFreeResources(res);
    return connected;
}

/// Everything the probe learns about one node. `renderer` is empty when the
/// context never came up, which is a different refusal from a context that came
/// up on llvmpipe, and the caller reports them differently.
struct NodeVerdict {
    bool egl_ok = false;
    std::string renderer;
    std::string failure;
};

/// Bring GBM and a GLES2 context up on an already-open DRM fd and read back
/// which renderer answered.
///
/// Deliberately never calls drmSetMaster(): the probe has to work while a
/// compositor or another HelixScreen instance owns the screen. It also creates
/// no framebuffer and sets no mode, so nothing the user is looking at changes.
NodeVerdict probe_node(int fd) {
    NodeVerdict verdict;

    struct gbm_device* gbm = gbm_create_device(fd);
    if (gbm == nullptr) {
        verdict.failure = "gbm_create_device failed";
        return verdict;
    }

    EGLDisplay dpy = EGL_NO_DISPLAY;
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_display != nullptr) {
        dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
    }
    if (dpy == EGL_NO_DISPLAY) {
        dpy = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(gbm));
    }
    if (dpy == EGL_NO_DISPLAY) {
        verdict.failure = "no EGL display for this GBM device";
        gbm_device_destroy(gbm);
        return verdict;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(dpy, &major, &minor) == EGL_FALSE) {
        verdict.failure = "eglInitialize failed";
        gbm_device_destroy(gbm);
        return verdict;
    }

    struct gbm_surface* surface = nullptr;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;

    if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
        verdict.failure = "eglBindAPI(OpenGL ES) failed";
    } else {
        const EGLint config_attr[] = {EGL_SURFACE_TYPE,
                                      EGL_WINDOW_BIT,
                                      EGL_RED_SIZE,
                                      8,
                                      EGL_GREEN_SIZE,
                                      8,
                                      EGL_BLUE_SIZE,
                                      8,
                                      EGL_RENDERABLE_TYPE,
                                      EGL_OPENGL_ES2_BIT,
                                      EGL_NONE};
        EGLConfig config;
        EGLint num_configs = 0;
        if (eglChooseConfig(dpy, config_attr, &config, 1, &num_configs) == EGL_FALSE ||
            num_configs < 1) {
            verdict.failure = "no EGL config with a window surface and GLES2";
        } else {
            const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
            ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attr);
            if (ctx == EGL_NO_CONTEXT) {
                verdict.failure = "eglCreateContext failed";
            } else if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) == EGL_FALSE) {
                // No surfaceless support, so prove the case that actually
                // matters instead: a buffer the GPU can render into and the
                // display engine can scan out.
                surface = gbm_surface_create(gbm, 640, 480, GBM_FORMAT_XRGB8888,
                                             GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
                if (surface == nullptr) {
                    verdict.failure = "no surfaceless context and no scanout-capable surface";
                } else {
                    egl_surface = eglCreateWindowSurface(
                        dpy, config, reinterpret_cast<EGLNativeWindowType>(surface), nullptr);
                    if (egl_surface == EGL_NO_SURFACE ||
                        eglMakeCurrent(dpy, egl_surface, egl_surface, ctx) == EGL_FALSE) {
                        verdict.failure = "eglMakeCurrent failed on a scanout surface";
                    }
                }
            }
        }
    }

    if (verdict.failure.empty()) {
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        if (renderer == nullptr || renderer[0] == '\0') {
            verdict.failure = "context came up but GL_RENDERER is empty";
        } else {
            verdict.renderer = renderer;
            verdict.egl_ok = true;
        }
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    if (egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(dpy, egl_surface);
    }
    if (ctx != EGL_NO_CONTEXT) {
        eglDestroyContext(dpy, ctx);
    }
    eglTerminate(dpy);
    if (surface != nullptr) {
        gbm_surface_destroy(surface);
    }
    gbm_device_destroy(gbm);
    return verdict;
}

/// The nodes to try, in the order the display backend would try them.
///
/// Mirrors DisplayBackendDRM's device policy: HELIX_DRM_DEVICE wins, otherwise
/// /dev/dri/card* sorted. The config override that backend also honours is
/// deliberately not read here — the probe runs before config init so it can
/// answer on a board where HelixScreen already holds the config lock.
std::vector<std::string> candidate_nodes() {
    const char* env_device = std::getenv("HELIX_DRM_DEVICE");
    if (env_device != nullptr && env_device[0] != '\0') {
        return {env_device};
    }

    std::vector<std::string> nodes;
    DIR* dir = opendir("/dev/dri");
    if (dir == nullptr) {
        return nodes;
    }
    const struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "card", 4) == 0) {
            nodes.emplace_back(std::string("/dev/dri/") + entry->d_name);
        }
    }
    closedir(dir);
    std::sort(nodes.begin(), nodes.end());
    return nodes;
}

} // namespace

int run_probe_egl() {
    const std::vector<std::string> nodes = candidate_nodes();
    if (nodes.empty()) {
        printf("no DRM card nodes to probe\n");
        return 1;
    }

    std::string last_reason;

    for (const std::string& node : nodes) {
        int fd = open(node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            last_reason = node + ": cannot open";
            continue;
        }
        if (!has_connected_connector(fd)) {
            last_reason = node + ": no connected display";
            close(fd);
            continue;
        }

        const NodeVerdict verdict = probe_node(fd);
        close(fd);

        if (!verdict.egl_ok) {
            last_reason = node + ": " + verdict.failure;
            continue;
        }
        if (gcode::gl_renderer_is_software(verdict.renderer.c_str())) {
            // Accepting this would spend CPU to save CPU: every pixel still
            // gets rasterized by the CPU, plus the cost of handing buffers to
            // a GPU that is not there.
            last_reason = node + ": software renderer (" + verdict.renderer + ")";
            continue;
        }

        printf("%s: %s\n", node.c_str(), verdict.renderer.c_str());
        return 0;
    }

    printf("no GPU presentation available - %s\n", last_reason.c_str());
    return 1;
}

} // namespace probe
} // namespace helix

#else // HELIX_PROBE_EGL_AVAILABLE

namespace helix {
namespace probe {

int run_probe_egl() {
    printf("built without EGL support\n");
    return 1;
}

} // namespace probe
} // namespace helix

#endif // HELIX_PROBE_EGL_AVAILABLE
