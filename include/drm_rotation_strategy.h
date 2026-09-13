// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file drm_rotation_strategy.h
 * @brief DRM plane rotation decision logic (hardware vs software fallback)
 *
 * Pure logic, no DRM dependencies — can be tested without hardware.
 * Used by DisplayBackendDRM::set_display_rotation() to decide whether the DRM
 * plane or LVGL's own rotation carries the requested angle.
 */

#pragma once

#include <cstdint>

/**
 * @brief Strategy for applying display rotation on DRM backend
 *
 * HARDWARE is reachable only on a plane whose rotation mask advertises the angle,
 * and only under the dumb-buffer driver: the EGL build compiles the plane rotation
 * entry points out, so it reports no hardware rotation at all. SOFTWARE is chosen
 * for a plane that cannot honour the request, which DisplayManager answers by
 * swapping in fbdev before this backend is asked to apply it - so on any board
 * without a capable plane, every nonzero angle still rotates through fbdev.
 */
enum class DrmRotationStrategy {
    NONE,     ///< No rotation needed (0°)
    HARDWARE, ///< Use DRM plane rotation property; LVGL's own rotation is cleared to 0
    SOFTWARE  ///< LVGL owns rotation via lv_display_set_rotation(); the DRM flush
              ///< callback reverses pixels in place (patches/lvgl-drm-flush-rotation.patch)
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
 * @brief A pointer sample in the panel's own coordinate frame
 */
struct PointerXY { // NAMESPACE_OK: matches DrmRotationStrategy, this file's global-scope types
    int32_t x;
    int32_t y;
};

/**
 * @brief Map a raw touch point onto a picture some other device rotated
 *
 * LVGL transforms pointer input from its own display rotation and early-returns
 * at zero, so a panel rotated by a scanout plane presents a rotated picture to
 * an untransformed touch frame. This applies the transform LVGL would have.
 *
 * The arithmetic deliberately mirrors `lv_display_rotate_point()`, which reads
 * the display's RAW hor_res/ver_res rather than the rotation-swapping getters -
 * so both take panel dimensions here. `tests/unit/test_display_rotation_source.cpp`
 * asserts the two agree at every angle rather than trusting that they do.
 *
 * @param p        raw point in the panel's native frame
 * @param degrees  angle the picture is presented at: 0, 90, 180 or 270
 * @param panel_w  native panel width, before rotation
 * @param panel_h  native panel height, before rotation
 * @return the point in the rotated picture's frame; unchanged for any other angle
 */
// NAMESPACE_OK: matches choose_drm_rotation_strategy, this file's existing global-scope API
PointerXY rotate_pointer_for_plane(PointerXY p, int degrees, int32_t panel_w, int32_t panel_h);

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

/**
 * @brief Whether the DRM plane is allowed to own the rotation
 *
 * Rotating the scanout plane rotates the picture but not the touch frame, since
 * LVGL transforms pointer input solely from its own display rotation and the
 * plane path clears that. `DisplayBackendDRM` closes the gap by chaining
 * rotate_pointer_for_plane() onto the pointer's read callback and reporting the
 * plane's angle from applied_rotation_degrees(), so the two transforms are
 * mutually exclusive and the touch pipeline still sees the angle the panel is
 * really at (prestonbrown/helixscreen#1275).
 *
 * A plane still only gets an angle its rotation mask advertises; everything else
 * falls to the software path.
 */
// NAMESPACE_OK: matches choose_drm_rotation_strategy, this file's existing global-scope function
bool plane_may_own_rotation();
