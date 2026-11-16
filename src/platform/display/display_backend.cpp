// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HelixScreen - Display Backend Factory Implementation

#include "display_backend.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

// Platform-specific includes for availability checks
#include <sys/stat.h>
#include <unistd.h>

#ifdef HELIX_DISPLAY_DRM
#include "gpu_libs.h"

#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

void DisplayBackend::configure_touch_gestures(lv_indev_t* indev) {
#if LV_USE_GESTURE_RECOGNITION
    if (!indev) {
        return;
    }
    lv_indev_set_pinch_up_threshold(indev, 1.15f);
    lv_indev_set_pinch_down_threshold(indev, 0.85f);
    lv_indev_set_rotation_rad_threshold(indev, 3.14f);
#else
    (void)indev;
#endif
}

int DisplayBackend::detect_panel_orientation() {
#ifdef __linux__
    // Method 1: Parse /proc/cmdline for video=*:panel_orientation=*
    // Works on any Linux regardless of DRM linkage
    {
        int cmdline_orientation = detect_panel_orientation_from_cmdline();
        if (cmdline_orientation >= 0) {
            spdlog::info("[DisplayBackend] Panel orientation from cmdline: {}°",
                         cmdline_orientation);
            return cmdline_orientation;
        }
    }

#ifdef HELIX_DISPLAY_DRM
    // Method 2: Query DRM connector "panel orientation" property directly
    // More reliable but requires libdrm
    if (helix::gpu_libs::available(helix::gpu_libs::Lib::Drm)) {
        const char* devices[] = {"/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2"};
        for (const char* dev : devices) {
            int fd = open(dev, O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                continue;

            drmModeRes* resources = drmModeGetResources(fd);
            if (!resources) {
                close(fd);
                continue;
            }

            for (int i = 0; i < resources->count_connectors; i++) {
                drmModeConnector* conn = drmModeGetConnector(fd, resources->connectors[i]);
                if (!conn || conn->connection != DRM_MODE_CONNECTED) {
                    if (conn)
                        drmModeFreeConnector(conn);
                    continue;
                }

                // Search connector properties for "panel orientation"
                drmModeObjectProperties* props =
                    drmModeObjectGetProperties(fd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR);
                if (props) {
                    for (uint32_t p = 0; p < props->count_props; p++) {
                        drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[p]);
                        if (!prop)
                            continue;

                        if (strcmp(prop->name, "panel orientation") == 0) {
                            uint64_t val = props->prop_values[p];
                            // Values: Normal=0, Upside Down=1, Left Side Up=2, Right Side Up=3
                            int degrees = -1;
                            switch (val) {
                            case 0:
                                degrees = 0;
                                break;
                            case 1:
                                degrees = 180;
                                break;
                            case 2:
                                degrees = 90;
                                break;
                            case 3:
                                degrees = 270;
                                break;
                            }
                            spdlog::info("[DisplayBackend] Panel orientation from DRM: {} ({}°)",
                                         val, degrees);
                            drmModeFreeProperty(prop);
                            drmModeFreeObjectProperties(props);
                            drmModeFreeConnector(conn);
                            drmModeFreeResources(resources);
                            close(fd);
                            return degrees;
                        }
                        drmModeFreeProperty(prop);
                    }
                    drmModeFreeObjectProperties(props);
                }
                drmModeFreeConnector(conn);
            }
            drmModeFreeResources(resources);
            close(fd);
        }
    }
#endif // HELIX_DISPLAY_DRM
#endif // __linux__

    spdlog::debug("[DisplayBackend] No panel orientation detected");
    return -1;
}

std::unique_ptr<DisplayBackend> DisplayBackend::create(DisplayBackendType type) {
    switch (type) {
#ifdef HELIX_DISPLAY_SDL
    case DisplayBackendType::SDL:
        return std::make_unique<DisplayBackendSDL>();
#endif

#ifdef HELIX_DISPLAY_FBDEV
    case DisplayBackendType::FBDEV:
        return std::make_unique<DisplayBackendFbdev>();
#endif

#ifdef HELIX_DISPLAY_DRM
    case DisplayBackendType::DRM:
        return std::make_unique<DisplayBackendDRM>();
#endif

    case DisplayBackendType::AUTO:
        return create_auto();

    default:
        spdlog::error("[DisplayBackend] Type {} not compiled in",
                      display_backend_type_to_string(type));
        return nullptr;
    }
}

namespace {

// Emitted when every compiled backend failed to initialize. Inspects the raw
// device nodes to explain *why* nothing was usable — almost always a display
// panel whose kernel driver / device-tree overlay never loaded. Without this
// the only signal is the opaque "No display backend available!", which sends
// users hunting in the wrong place (#998: BTT Pi/CB2 DSI panel on generic
// Armbian — backlight on, but no /dev/fb0 and no connected DRM connector).
void diagnose_no_display_hardware() {
    struct stat st;
    bool have_fb0 = (stat("/dev/fb0", &st) == 0);

    bool have_dri = false;
    bool have_connected_connector = false;
#ifdef HELIX_DISPLAY_DRM
    const char* devices[] = {"/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2"};
    const bool have_libdrm = helix::gpu_libs::available(helix::gpu_libs::Lib::Drm);
    for (const char* dev : devices) {
        int fd = open(dev, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        have_dri = true;
        if (!have_libdrm) {
            close(fd);
            continue;
        }
        drmModeRes* resources = drmModeGetResources(fd);
        if (resources) {
            for (int i = 0; i < resources->count_connectors && !have_connected_connector; i++) {
                drmModeConnector* conn = drmModeGetConnector(fd, resources->connectors[i]);
                if (conn) {
                    if (conn->connection == DRM_MODE_CONNECTED)
                        have_connected_connector = true;
                    drmModeFreeConnector(conn);
                }
            }
            drmModeFreeResources(resources);
        }
        close(fd);
    }
#endif

    // Hardware is present but the backend(s) still failed — permissions, a busy
    // device, or a config mismatch. The per-backend warnings already cover that;
    // don't muddy them with overlay guidance that doesn't apply.
    if (have_fb0 || have_connected_connector)
        return;

    spdlog::error("[DisplayBackend] No display hardware detected: /dev/fb0 is absent "
                  "and no DRM connector is connected{}.",
                  have_dri ? "" : " (/dev/dri has no usable card)");
    spdlog::error("[DisplayBackend] This almost always means your display panel's kernel "
                  "driver or device-tree overlay is not loaded — the panel is powered "
                  "(backlight on) but the kernel never created a framebuffer for it.");
    spdlog::error("[DisplayBackend] Fix: enable the panel overlay for your board and "
                  "reboot. On a BigTreeTech Pi/CB2 and similar, the stock BTT OS image "
                  "ships the DSI panel overlay preconfigured; a generic Armbian/Debian "
                  "image usually does not. Verify with 'ls /dev/fb0' and 'ls /sys/class/drm/'.");
}

} // namespace

std::unique_ptr<DisplayBackend> DisplayBackend::create_auto() {
    // Check environment variable override first
    const char* backend_env = std::getenv("HELIX_DISPLAY_BACKEND");
    const auto forced = helix::parse_forced_display_backend(backend_env);
    if (forced != helix::ForcedDisplayBackend::None) {
        spdlog::info("[DisplayBackend] HELIX_DISPLAY_BACKEND={} - using forced backend",
                     backend_env);
    }
    switch (forced) {
    case helix::ForcedDisplayBackend::None:
        break;
    case helix::ForcedDisplayBackend::Drm: {
#ifdef HELIX_DISPLAY_DRM
        auto backend = std::make_unique<DisplayBackendDRM>();
        if (backend->is_available()) {
            return backend;
        }
        spdlog::warn("[DisplayBackend] DRM backend forced but not available");
#else
        spdlog::warn("[DisplayBackend] DRM backend forced but not compiled in");
#endif
        break;
    }
    case helix::ForcedDisplayBackend::DrmEgl: {
#ifdef HELIX_DISPLAY_DRM
        if (helix::make_drm_egl_backend != nullptr) {
            auto backend = helix::make_drm_egl_backend();
            if (backend->is_available()) {
                return backend;
            }
            spdlog::warn("[DisplayBackend] EGL presentation forced but DRM is not available");
            break;
        }
#endif
        spdlog::warn("[DisplayBackend] EGL presentation forced but not compiled in");
        break;
    }
    case helix::ForcedDisplayBackend::Fbdev: {
#ifdef HELIX_DISPLAY_FBDEV
        auto backend = std::make_unique<DisplayBackendFbdev>();
        if (backend->is_available()) {
            return backend;
        }
        spdlog::warn("[DisplayBackend] Framebuffer backend forced but not available");
#else
        spdlog::warn("[DisplayBackend] Framebuffer backend forced but not compiled in");
#endif
        break;
    }
    case helix::ForcedDisplayBackend::Sdl: {
#ifdef HELIX_DISPLAY_SDL
        auto backend = std::make_unique<DisplayBackendSDL>();
        if (backend->is_available()) {
            return backend;
        }
        spdlog::warn("[DisplayBackend] SDL backend forced but not available");
#else
        spdlog::warn("[DisplayBackend] SDL backend forced but not compiled in");
#endif
        break;
    }
    case helix::ForcedDisplayBackend::Unknown:
        spdlog::warn("[DisplayBackend] Unknown HELIX_DISPLAY_BACKEND value: {}",
                     backend_env != nullptr ? backend_env : "");
        break;
    }
    // A forced backend that is unavailable falls through to auto-detection.

    // Auto-detection: try backends in order of preference

    // 1. Try DRM first (best performance on modern Linux with GPU)
#ifdef HELIX_DISPLAY_DRM
    {
        auto backend = std::make_unique<DisplayBackendDRM>();
        if (backend->is_available()) {
            spdlog::info("[DisplayBackend] Auto-detected: DRM/KMS");
            return backend;
        }
        spdlog::debug("[DisplayBackend] DRM backend not available");
    }
#endif

    // 2. Try framebuffer (works on most embedded Linux)
#ifdef HELIX_DISPLAY_FBDEV
    {
        auto backend = std::make_unique<DisplayBackendFbdev>();
        if (backend->is_available()) {
            spdlog::info("[DisplayBackend] Auto-detected: Framebuffer");
            return backend;
        }
        spdlog::debug("[DisplayBackend] Framebuffer backend not available");
    }
#endif

    // 3. Fall back to SDL (desktop development)
#ifdef HELIX_DISPLAY_SDL
    {
        auto backend = std::make_unique<DisplayBackendSDL>();
        if (backend->is_available()) {
            spdlog::info("[DisplayBackend] Auto-detected: SDL");
            return backend;
        }
        spdlog::debug("[DisplayBackend] SDL backend not available");
    }
#endif

    spdlog::error("[DisplayBackend] No display backend available!");
    spdlog::error("[DisplayBackend] Compiled backends: "
#ifdef HELIX_DISPLAY_SDL
                  "SDL "
#endif
#ifdef HELIX_DISPLAY_FBDEV
                  "FBDEV "
#endif
#ifdef HELIX_DISPLAY_DRM
                  "DRM "
#endif
    );

    diagnose_no_display_hardware();

    return nullptr;
}
