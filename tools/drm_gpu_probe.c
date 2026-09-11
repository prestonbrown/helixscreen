// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file drm_gpu_probe.c
 * @brief Read-only DRM/EGL capability probe for a target board
 *
 * Answers, per /dev/dri/card*, the questions that decide what a display backend
 * can actually do on a board:
 *
 *   - Which DRM driver owns the node, and whether atomic modesetting is available.
 *   - Every plane, its type, and the rotation angles its "rotation" property
 *     advertises. A mask of 0 means the plane cannot rotate at all; the absence
 *     of DRM_MODE_ROTATE_90/270 is the common case and is why software rotation
 *     exists.
 *   - Whether a GBM device and a GLES2 context can actually be created on that
 *     node, and which renderer answers. A renderer string of "llvmpipe" or
 *     "softpipe" means Mesa fell back to software and any GPU claim is false.
 *
 * Never modesets, never page-flips, and needs no DRM master, so it is safe to
 * run against a board with a display server or HelixScreen already up.
 *
 * Build natively where libdrm, gbm and EGL headers exist:
 *
 *     gcc tools/drm_gpu_probe.c -o drm_gpu_probe \
 *         -ldrm -lgbm -lEGL -lGLESv2 -I/usr/include/libdrm
 *
 * Most target boards ship the libraries but not the headers. Cross-compile in
 * the toolchain image for the platform instead, then copy the binary over:
 *
 *     docker run --rm -v "$PWD":/w -w /w helixscreen/toolchain-pi \
 *       aarch64-linux-gnu-gcc tools/drm_gpu_probe.c -o drm_gpu_probe-pi \
 *       -ldrm -lgbm -lEGL -lGLESv2 -I/usr/include/libdrm
 *
 * A board with no GL userspace at all (no libEGL/libGLESv2/libgbm) cannot run
 * the EGL half. Build with -DNO_EGL for those; the plane survey still works and
 * is the part that matters there.
 *
 *     aarch64-linux-gnu-gcc -DNO_EGL tools/drm_gpu_probe.c -o drm_gpu_probe-u1 \
 *         -ldrm -I/usr/include/libdrm
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef NO_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#endif

#define MAX_CARDS 4
#define ROT_NAMES_CAP 256

/** Print the plane inventory for one card, with each plane's rotation mask. */
static void probe_planes(int fd) {
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    printf("  atomic_cap: %s\n", drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0 ? "yes" : "no");

    drmModePlaneRes* pr = drmModeGetPlaneResources(fd);
    if (!pr) {
        printf("  no plane resources (render-only node)\n");
        return;
    }

    printf("  planes: %u\n", pr->count_planes);
    for (uint32_t i = 0; i < pr->count_planes; i++) {
        drmModePlane* plane = drmModeGetPlane(fd, pr->planes[i]);
        if (!plane) {
            continue;
        }

        const char* type = "?";
        uint64_t rot_mask = 0;
        char rot_names[ROT_NAMES_CAP] = "";

        drmModeObjectProperties* props =
            drmModeObjectGetProperties(fd, pr->planes[i], DRM_MODE_OBJECT_PLANE);
        if (props) {
            for (uint32_t p = 0; p < props->count_props; p++) {
                drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[p]);
                if (!prop) {
                    continue;
                }
                if (strcmp(prop->name, "type") == 0) {
                    switch (props->prop_values[p]) {
                    case DRM_PLANE_TYPE_PRIMARY:
                        type = "PRIMARY";
                        break;
                    case DRM_PLANE_TYPE_OVERLAY:
                        type = "overlay";
                        break;
                    case DRM_PLANE_TYPE_CURSOR:
                        type = "cursor";
                        break;
                    default:
                        break;
                    }
                } else if (strcmp(prop->name, "rotation") == 0) {
                    size_t used = 0;
                    for (int e = 0; e < prop->count_enums; e++) {
                        rot_mask |= (1ULL << prop->enums[e].value);
                        int n = snprintf(rot_names + used, sizeof(rot_names) - used, "%s ",
                                         prop->enums[e].name);
                        if (n < 0 || (size_t)n >= sizeof(rot_names) - used) {
                            break;
                        }
                        used += (size_t)n;
                    }
                }
                drmModeFreeProperty(prop);
            }
            drmModeFreeObjectProperties(props);
        }

        printf("  plane %u (%s) crtcs=0x%x rotation=%s mask=0x%llx [%s]\n", pr->planes[i], type,
               plane->possible_crtcs, rot_mask ? "yes" : "none", (unsigned long long)rot_mask,
               rot_names);
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(pr);
}

#ifndef NO_EGL
/** Bring up GBM + EGL + a GLES2 context on one card and report the renderer. */
static void probe_egl(int fd) {
    struct gbm_device* gbm = gbm_create_device(fd);
    if (!gbm) {
        printf("  gbm_create_device FAILED - no scanout allocation on this node\n");
        return;
    }
    printf("  gbm backend: %s\n", gbm_device_get_backend_name(gbm));

    EGLDisplay dpy = EGL_NO_DISPLAY;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display) {
        dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    }
    if (dpy == EGL_NO_DISPLAY) {
        dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
    }
    if (dpy == EGL_NO_DISPLAY) {
        printf("  eglGetDisplay FAILED\n");
        return;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        printf("  eglInitialize FAILED 0x%x\n", eglGetError());
        return;
    }
    printf("  EGL %d.%d vendor=%s\n", major, minor, eglQueryString(dpy, EGL_VENDOR));

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        printf("  eglBindAPI FAILED\n");
        return;
    }

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE,
        EGL_WINDOW_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
        printf("  eglChooseConfig FAILED\n");
        return;
    }

    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) {
        printf("  eglCreateContext FAILED 0x%x\n", eglGetError());
        return;
    }

    // Surfaceless works on most stacks; where it does not, a scanout-capable GBM
    // surface is the fallback, and its success is itself worth reporting.
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        struct gbm_surface* gs = gbm_surface_create(gbm, 640, 480, GBM_FORMAT_XRGB8888,
                                                    GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
        if (!gs) {
            printf("  surfaceless and gbm_surface_create both FAILED\n");
            return;
        }
        EGLSurface sfc = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)gs, NULL);
        if (sfc == EGL_NO_SURFACE || !eglMakeCurrent(dpy, sfc, sfc, ctx)) {
            printf("  eglMakeCurrent FAILED 0x%x\n", eglGetError());
            return;
        }
        printf("  gbm_surface_create(SCANOUT|RENDERING): ok\n");
    }

    printf("  GL_VENDOR   = %s\n", glGetString(GL_VENDOR));
    printf("  GL_RENDERER = %s\n", glGetString(GL_RENDERER));
    printf("  GL_VERSION  = %s\n", glGetString(GL_VERSION));
    printf("  GL_SL       = %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));

    GLint max_tex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    printf("  MAX_TEXTURE = %d\n", max_tex);
}
#else
static void probe_egl(int fd) {
    (void)fd;
    printf("  (EGL probe omitted: built with -DNO_EGL)\n");
}
#endif

int main(void) {
    int found = 0;
    for (int i = 0; i < MAX_CARDS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);

        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        found++;

        drmVersionPtr ver = drmGetVersion(fd);
        printf("== %s driver=%s\n", path, ver ? ver->name : "?");
        if (ver) {
            drmFreeVersion(ver);
        }

        probe_planes(fd);
        probe_egl(fd);
        close(fd);
        printf("\n");
    }

    if (found == 0) {
        printf("no /dev/dri/card* could be opened - wrong board, or no permission\n");
        return 1;
    }
    return 0;
}
