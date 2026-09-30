// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "drm_rotation_strategy.h"

// DRM_MODE_ROTATE_0 is (1<<0) = 0x1 — the identity rotation
static constexpr uint64_t DRM_ROT_0 = (1 << 0);
// DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_270, the angles that swap width and height
static constexpr uint64_t DRM_ROT_QUARTER_TURNS = (1 << 1) | (1 << 3);

DrmRotationStrategy choose_drm_rotation_strategy(uint64_t requested_drm_rot,
                                                 uint64_t supported_mask, bool egl_presentation) {
    // No rotation needed
    if (requested_drm_rot == DRM_ROT_0) {
        return DrmRotationStrategy::NONE;
    }

    // The EGL backend presents LVGL's completed display texture with
    // lv_opengles_render_display(), whose vertex buffer follows the display
    // rotation. This keeps scanout at the panel's native mode and performs the
    // quarter turn on the GPU without a CPU transpose or an MDP rotator.
    if (egl_presentation) {
        return DrmRotationStrategy::GPU;
    }

    // The plane is programmed with SRC and CRTC rectangles at the panel's own
    // width and height, and LVGL keeps laying out at that resolution because
    // the plane path clears LVGL's rotation. A quarter turn fits neither, so it
    // never reaches the plane whatever the mask advertises.
    if (requested_drm_rot & DRM_ROT_QUARTER_TURNS) {
        return DrmRotationStrategy::SOFTWARE;
    }

    // Hardware supports the requested angle
    if (supported_mask & requested_drm_rot) {
        return DrmRotationStrategy::HARDWARE;
    }

    // Hardware doesn't support it — fall back to software
    return DrmRotationStrategy::SOFTWARE;
}

// NAMESPACE_OK: matches choose_drm_rotation_strategy, this file's existing global-scope function
LvglRotationAction lvgl_rotation_action_for(DrmRotationStrategy strategy) {
    if (strategy == DrmRotationStrategy::GPU || strategy == DrmRotationStrategy::SOFTWARE) {
        return LvglRotationAction::APPLY_REQUESTED;
    }
    return LvglRotationAction::CLEAR_TO_ZERO;
}

// NAMESPACE_OK: matches choose_drm_rotation_strategy, this file's existing global-scope function
bool drm_rotation_needs_full_render(DrmRotationStrategy strategy) {
    return strategy == DrmRotationStrategy::SOFTWARE;
}

// NAMESPACE_OK: matches choose_drm_rotation_strategy, this file's existing global-scope function
bool plane_may_own_rotation() {
    return true;
}

// NAMESPACE_OK: matches choose_drm_rotation_strategy, this file's existing global-scope function
PointerXY rotate_pointer_for_plane(PointerXY p, int degrees, int32_t panel_w, int32_t panel_h) {
    switch (degrees) {
    case 90:
        return {panel_h - p.y - 1, p.x};
    case 180:
        return {panel_w - p.x - 1, panel_h - p.y - 1};
    case 270:
        return {p.y, panel_w - p.x - 1};
    default:
        return p;
    }
}

// NAMESPACE_OK: matches rotate_pointer_for_plane, this file's existing global-scope function
PointerXY unrotate_pointer_for_display(PointerXY p, int degrees, int32_t panel_w, int32_t panel_h) {
    switch (degrees) {
    case 90:
        return {p.y, panel_h - p.x - 1};
    case 180:
        return {panel_w - p.x - 1, panel_h - p.y - 1};
    case 270:
        return {panel_w - p.y - 1, p.x};
    default:
        return p;
    }
}
