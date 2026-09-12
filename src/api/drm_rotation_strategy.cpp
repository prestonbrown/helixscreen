// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "drm_rotation_strategy.h"

// DRM_MODE_ROTATE_0 is (1<<0) = 0x1 — the identity rotation
static constexpr uint64_t DRM_ROT_0 = (1 << 0);

DrmRotationStrategy choose_drm_rotation_strategy(uint64_t requested_drm_rot,
                                                 uint64_t supported_mask) {
    // No rotation needed
    if (requested_drm_rot == DRM_ROT_0) {
        return DrmRotationStrategy::NONE;
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
    if (strategy == DrmRotationStrategy::SOFTWARE) {
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
    return false;
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
