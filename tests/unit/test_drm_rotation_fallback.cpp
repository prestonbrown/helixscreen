// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_drm_rotation_fallback.cpp
 * @brief Regression tests for DRM plane rotation fallback logic
 *
 * VC4 (Raspberry Pi with ultrawide panels) only supports 0/180 plane rotation.
 * Requesting 90/270 causes drmModeAtomicCommit to fail, breaking display init.
 * These tests verify that choose_drm_rotation_strategy() correctly decides
 * between hardware rotation, software fallback, or no rotation.
 *
 * DRM rotation bitmask bits (from drm_mode.h):
 *   DRM_MODE_ROTATE_0   = (1<<0) = 0x1
 *   DRM_MODE_ROTATE_90  = (1<<1) = 0x2
 *   DRM_MODE_ROTATE_180 = (1<<2) = 0x4
 *   DRM_MODE_ROTATE_270 = (1<<3) = 0x8
 */

#include "../catch_amalgamated.hpp"

// The function under test is pure logic, no DRM hardware needed.
// Include it even without HELIX_DISPLAY_DRM — the enum and function
// are deliberately kept hardware-independent for testability.
#include "drm_rotation_strategy.h"

// DRM rotation constants (mirrored from drm_mode.h so tests compile
// without libdrm headers)
static constexpr uint64_t ROT_0 = (1 << 0);   // 0x1
static constexpr uint64_t ROT_90 = (1 << 1);  // 0x2
static constexpr uint64_t ROT_180 = (1 << 2); // 0x4
static constexpr uint64_t ROT_270 = (1 << 3); // 0x8

static constexpr uint64_t MASK_ALL = ROT_0 | ROT_90 | ROT_180 | ROT_270; // 0xF
static constexpr uint64_t MASK_0_180 = ROT_0 | ROT_180;                  // 0x5 (VC4)
static constexpr uint64_t MASK_0_ONLY = ROT_0;                           // 0x1
static constexpr uint64_t MASK_NONE = 0;                                 // no rotation property

TEST_CASE("0° rotation always returns NONE", "[display][drm][rotation]") {
    // No rotation needed — no hardware or software path required
    REQUIRE(choose_drm_rotation_strategy(ROT_0, MASK_ALL) == DrmRotationStrategy::NONE);
    REQUIRE(choose_drm_rotation_strategy(ROT_0, MASK_0_180) == DrmRotationStrategy::NONE);
    REQUIRE(choose_drm_rotation_strategy(ROT_0, MASK_NONE) == DrmRotationStrategy::NONE);
}

TEST_CASE("Hardware rotation when plane supports requested angle", "[display][drm][rotation]") {
    // Full rotation support (mask=0xF), request 270° → use hardware
    REQUIRE(choose_drm_rotation_strategy(ROT_270, MASK_ALL) == DrmRotationStrategy::HARDWARE);
    REQUIRE(choose_drm_rotation_strategy(ROT_90, MASK_ALL) == DrmRotationStrategy::HARDWARE);
    REQUIRE(choose_drm_rotation_strategy(ROT_180, MASK_ALL) == DrmRotationStrategy::HARDWARE);
}

TEST_CASE("Software fallback when plane lacks 90/270", "[display][drm][rotation]") {
    // VC4 scenario: mask=0x5 (0°+180°), request 270° → must use software
    REQUIRE(choose_drm_rotation_strategy(ROT_270, MASK_0_180) == DrmRotationStrategy::SOFTWARE);
    REQUIRE(choose_drm_rotation_strategy(ROT_90, MASK_0_180) == DrmRotationStrategy::SOFTWARE);
}

TEST_CASE("Software fallback when no rotation property", "[display][drm][rotation]") {
    // No rotation property at all (mask=0), any non-zero rotation → software
    REQUIRE(choose_drm_rotation_strategy(ROT_90, MASK_NONE) == DrmRotationStrategy::SOFTWARE);
    REQUIRE(choose_drm_rotation_strategy(ROT_180, MASK_NONE) == DrmRotationStrategy::SOFTWARE);
    REQUIRE(choose_drm_rotation_strategy(ROT_270, MASK_NONE) == DrmRotationStrategy::SOFTWARE);
}

TEST_CASE("180° uses hardware when supported", "[display][drm][rotation]") {
    // VC4 supports 180° — should use hardware path
    REQUIRE(choose_drm_rotation_strategy(ROT_180, MASK_0_180) == DrmRotationStrategy::HARDWARE);
}

TEST_CASE("180° falls back to software when only 0° supported", "[display][drm][rotation]") {
    // Only 0° supported — 180° must use software
    REQUIRE(choose_drm_rotation_strategy(ROT_180, MASK_0_ONLY) == DrmRotationStrategy::SOFTWARE);
}

TEST_CASE("HARDWARE rotation clears LVGL rotation", "[display][drm][rotation]") {
    // The plane rotates the scanout. LVGL rotating as well applies the
    // transform twice, and two 180s cancel (prestonbrown/helixscreen#1275).
    REQUIRE(lvgl_rotation_action_for(DrmRotationStrategy::HARDWARE) ==
            LvglRotationAction::CLEAR_TO_ZERO);
}

TEST_CASE("SOFTWARE rotation applies the requested angle to LVGL", "[display][drm][rotation]") {
    // The dumb-buffer flush callback reads lv_display_get_rotation() to decide
    // whether to reverse the pixel array, so LVGL must carry the angle.
    REQUIRE(lvgl_rotation_action_for(DrmRotationStrategy::SOFTWARE) ==
            LvglRotationAction::APPLY_REQUESTED);
}

TEST_CASE("NONE clears LVGL rotation", "[display][drm][rotation]") {
    REQUIRE(lvgl_rotation_action_for(DrmRotationStrategy::NONE) ==
            LvglRotationAction::CLEAR_TO_ZERO);
}

TEST_CASE("Only SOFTWARE needs FULL render mode", "[display][drm][rotation]") {
    // A partial-render buffer cannot be reversed in place.
    REQUIRE(drm_rotation_needs_full_render(DrmRotationStrategy::SOFTWARE));
    REQUIRE_FALSE(drm_rotation_needs_full_render(DrmRotationStrategy::HARDWARE));
    REQUIRE_FALSE(drm_rotation_needs_full_render(DrmRotationStrategy::NONE));
}

TEST_CASE("Plane may not own rotation until touch follows it", "[display][drm][rotation]") {
    // lv_display_rotate_point() is the only touch transform in the tree, and it
    // derives solely from LVGL's own display rotation, which the HARDWARE path
    // clears. Nothing rotates touch to match a plane-rotated picture yet.
    REQUIRE_FALSE(plane_may_own_rotation());
}
