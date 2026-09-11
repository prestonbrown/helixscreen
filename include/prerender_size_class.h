// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file prerender_size_class.h
 * @brief Screen resolution to pre-rendered asset size class
 *
 * Pure rules, no I/O: a resolution goes in, the name of an asset size class
 * comes out. Both helix-screen and the standalone helix-splash binary link this
 * translation unit, so the class a device asks for cannot differ between the
 * two. `scripts/gen_splash_3d.py` and `scripts/regen_images.sh` generate files
 * named for these same classes, and `assets/config/platforms.json` records the
 * panel geometry each platform feeds in.
 *
 * ## These are not UI breakpoints
 *
 * The tiers in `include/ui_breakpoint.h` are a different ladder that reuses
 * several of the same words for different resolutions:
 *
 * | Resolution | This ladder | UiBreakpoint |
 * |------------|-------------|--------------|
 * | 480x272    | `tiny`      | Micro        |
 * | 480x320    | `tiny`      | Tiny         |
 * | 480x400    | `tiny_alt`  | Small        |
 * | 800x480    | `small`     | Medium       |
 * | 1024x600   | `medium`    | Large        |
 * | 1280x720   | `large`     | XLarge       |
 *
 * The ladders disagree because they answer different questions. This one keys
 * off the wide axis and names a file on disk; `breakpoint_for()` keys off the
 * narrow axis because layout has to fit content into the cramped dimension.
 * Convert between them through a resolution, never by matching names.
 */

namespace helix {

/**
 * @brief Size class for the 2D splash logo
 *
 * @param screen_width Display width in pixels
 * @return "tiny" (480x320 class), "small" (800x480), "medium" (1024x600),
 *         or "large" (1280x720+)
 */
[[nodiscard]] const char* get_splash_size_name(int screen_width);

/**
 * @brief Size class for the composited full-screen 3D splash
 *
 * Uses height as well as width, which separates the 480x400 class from the
 * 480x320 one and catches panels that are wide but very short.
 *
 * @param screen_width Display width in pixels
 * @param screen_height Display height in pixels
 * @return "tiny", "tiny_alt", "small", "medium", "large", or "ultrawide"
 */
[[nodiscard]] const char* get_splash_3d_size_name(int screen_width, int screen_height);

/**
 * @brief Composited height of a 3D splash size class
 *
 * A pre-rendered 3D splash is a full-screen canvas, so a class taller than the
 * panel overdraws. Callers compare this against the real screen height and fall
 * back to runtime scaling when it does not fit.
 *
 * @param size_name Class name from get_splash_3d_size_name()
 * @return Height in pixels, or 0 if the name is unknown
 */
[[nodiscard]] int get_splash_3d_target_height(const char* size_name);

/**
 * @brief Pre-rendered printer image size for a screen width
 *
 * @param screen_width Display width in pixels
 * @return 300 for 800x480 and wider, 150 below that
 */
[[nodiscard]] int get_printer_image_size(int screen_width);

} // namespace helix
