// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file drm_rotation_strategy.h
 * @brief DRM plane rotation decision logic (hardware vs software fallback)
 *
 * Pure logic, no DRM dependencies — can be tested without hardware.
 * Used by DisplayBackendDRM::set_display_rotation() to decide whether
 * to use DRM plane rotation or LVGL matrix rotation.
 */

#pragma once

#include <cstdint>

/**
 * @brief Strategy for applying display rotation on DRM backend
 */
enum class DrmRotationStrategy {
    NONE,     ///< No rotation needed (0°)
    HARDWARE, ///< Use DRM plane rotation property
    SOFTWARE  ///< Use LVGL matrix rotation (lv_display_set_matrix_rotation)
};

/**
 * @brief Decide how to rotate the display on a DRM backend
 *
 * Examines the requested rotation against the DRM plane's supported
 * rotation bitmask to choose the best strategy:
 * - 0° always returns NONE (no rotation needed)
 * - If the plane supports the requested angle, returns HARDWARE
 * - Otherwise returns SOFTWARE (LVGL matrix rotation fallback)
 *
 * @param requested_drm_rot  DRM_MODE_ROTATE_* constant for the desired angle
 * @param supported_mask     Bitmask of supported rotations from the plane property
 *                           (0 = no rotation property exists)
 * @return Strategy to use for this rotation
 */
DrmRotationStrategy choose_drm_rotation_strategy(uint64_t requested_drm_rot,
                                                 uint64_t supported_mask);

/**
 * @brief What LVGL should be told about rotation for a given strategy
 */
enum class LvglRotationAction { // NAMESPACE_OK: matches DrmRotationStrategy, this file's existing
                                // global-scope type
    APPLY_REQUESTED,            ///< Pass the requested angle to lv_display_set_rotation()
    CLEAR_TO_ZERO,              ///< Something else rotates; LVGL must not rotate as well
};

/**
 * @brief Decide whether LVGL carries the rotation, or something else does
 *
 * DRM plane rotation happens on the scanout side, after LVGL has produced its
 * pixels. Setting LVGL's rotation as well applies the transform a second time.
 *
 * @param strategy  Result of choose_drm_rotation_strategy()
 * @return Whether LVGL receives the requested angle or zero
 */
// NAMESPACE_OK: matches choose_drm_rotation_strategy, this file's existing global-scope function
LvglRotationAction lvgl_rotation_action_for(DrmRotationStrategy strategy);

/**
 * @brief Whether the display must render whole frames for this strategy
 *
 * The software path reverses the pixel array in place in the flush callback,
 * which needs the entire buffer present.
 *
 * @param strategy  Result of choose_drm_rotation_strategy()
 * @return true when LV_DISPLAY_RENDER_MODE_FULL is required
 */
// NAMESPACE_OK: matches choose_drm_rotation_strategy, this file's existing global-scope function
bool drm_rotation_needs_full_render(DrmRotationStrategy strategy);
