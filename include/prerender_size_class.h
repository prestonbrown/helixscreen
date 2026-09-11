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
 * ## The names are the layout system's names
 *
 * The classes are the `UiBreakpoint` tiers from `include/ui_breakpoint.h`, and
 * `get_splash_3d_size_name()` resolves them by calling `breakpoint_for()`. One
 * ladder, one axis, one vocabulary: `medium` is an 800x480 panel whether you
 * read it in an asset filename or in a layout override.
 *
 * | Canvas   | Class       |
 * |----------|-------------|
 * | 480x272  | `micro`     |
 * | 480x320  | `tiny`      |
 * | 480x400  | `small`     |
 * | 800x480  | `medium`    |
 * | 1024x600 | `large`     |
 * | 1280x720 | `xlarge`    |
 * | 1920x440 | `ultrawide` |
 *
 * Two departures from a plain tier lookup, both deliberate. A wide, short bar
 * display gets `ultrawide` rather than the tier its narrow axis would give it,
 * because its canvas shape is unlike anything else at that tier. And nothing is
 * composited above `xlarge`, so a larger panel is clamped to it and scales.
 *
 * Only a subset of classes has a 2D logo render; a lookup that misses falls back
 * to the source PNG, so asking for one costs nothing.
 */

namespace helix {

/**
 * @brief Size class for the 2D splash logo
 *
 * The same ladder as the 3D splash. Fewer classes have a logo render, and a
 * caller that finds none falls back to scaling the PNG.
 *
 * @param screen_width Display width in pixels
 * @param screen_height Display height in pixels
 * @return A UiBreakpoint tier name, or "ultrawide"
 */
[[nodiscard]] const char* get_splash_size_name(int screen_width, int screen_height);

/**
 * @brief Size class for the composited full-screen 3D splash
 *
 * @param screen_width Display width in pixels
 * @param screen_height Display height in pixels
 * @return "micro", "tiny", "small", "medium", "large", "xlarge" or "ultrawide"
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
 * Keyed on width rather than the narrow axis: this sizes a widget's artwork
 * against the horizontal room it is given, not a full-screen canvas.
 *
 * @param screen_width Display width in pixels
 * @return 300 for 800x480 and wider, 150 below that
 */
[[nodiscard]] int get_printer_image_size(int screen_width);

} // namespace helix
