// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "prerender_size_class.h"

#include <string>

/**
 * @file prerendered_images.h
 * @brief Helpers for selecting pre-rendered LVGL binary images
 *
 * Pre-rendered images are PNG files converted to LVGL's .bin format at specific
 * sizes. This eliminates runtime PNG decoding and scaling, dramatically improving
 * display performance on embedded devices.
 *
 * Image sizes are generated at build time by:
 *   - make gen-images         (splash screen)
 *   - make gen-printer-images (printer images)
 *
 * Size classes and the rules that select them live in prerender_size_class.h,
 * which this header re-exports so callers need only one include.
 *
 * @see scripts/regen_images.sh
 * @see scripts/regen_printer_images.sh
 */

namespace helix {

/**
 * @brief Get path to pre-rendered splash screen logo
 *
 * Selects the size class from the resolution and falls back to the original PNG
 * when that class has no logo render.
 *
 * @param screen_width Display width in pixels
 * @param screen_height Display height in pixels
 * @return LVGL path (A:...) to the image, or empty string if none found
 */
[[nodiscard]] std::string get_prerendered_splash_path(int screen_width, int screen_height);

/**
 * @brief Get path to pre-rendered printer image
 *
 * Selects the appropriate pre-rendered size based on screen width.
 * Falls back to original PNG if pre-rendered version doesn't exist.
 *
 * @param printer_name Printer name (e.g., "creality-k1", "voron-v2")
 * @param screen_width Display width in pixels (used to pick 300px vs 150px)
 * @return LVGL path (A:...) to the image
 */
[[nodiscard]] std::string get_prerendered_printer_path(const std::string& printer_name,
                                                       int screen_width);

/**
 * @brief Check if a pre-rendered image exists
 *
 * @param path Filesystem path (without A: prefix)
 * @return true if file exists and is readable
 */
[[nodiscard]] bool prerendered_exists(const std::string& path);

// =========================================================================
// Persistent printer image cache (exact widget dimensions)
// =========================================================================

/**
 * @brief Get the directory for cached printer images at exact widget sizes
 *
 * Returns a dedicated cache directory (e.g., ~/.cache/helix/printer_images/).
 * Creates the directory if it doesn't exist.
 *
 * @return Filesystem path to the cache directory (no trailing slash)
 */
[[nodiscard]] std::string get_printer_image_cache_dir();

/**
 * @brief Get the cache file path for a printer image at exact dimensions
 *
 * The name is "<source basename>-<W>x<H>-<mtime>-<size>.bin", so it is content-
 * addressed on the source: rewriting an image in place under an existing filename
 * resolves to a different entry, and the one holding the old pixels is never
 * consulted again.
 *
 * @param source_image_path The resolved source image path (LVGL path with A: prefix)
 * @param width Exact widget width in pixels
 * @param height Exact widget height in pixels
 * @return Filesystem path to the cache .bin file
 */
[[nodiscard]] std::string get_cached_printer_image_path(const std::string& source_image_path,
                                                        int width, int height);

/**
 * @brief The source image's "<mtime>-<size>", as the cache names carry it
 *
 * Changes when the file is rewritten in place under the same path.
 *
 * @param source_image_path The LVGL source path (with or without A: prefix)
 */
[[nodiscard]] std::string printer_image_source_fingerprint(const std::string& source_image_path);

/**
 * @brief Whether a cache filename is an entry generated from a given source image
 *
 * Pure name arithmetic — touches no filesystem. A match requires the source's
 * basename followed by the "<W>x<H>-" dimension segment, which is what keeps a
 * PNG's entries apart from those of the prerendered variants beside it:
 * "creality-k1-se-" also prefixes "creality-k1-se-300-233x209-…", the cache of a
 * different source image.
 *
 * @param cache_filename Filename (no directory) of a cache entry
 * @param source_image_path The LVGL source path (with or without A: prefix)
 * @return true if the entry caches that source
 */
[[nodiscard]] bool printer_cache_entry_matches(const std::string& cache_filename,
                                               const std::string& source_image_path);

/**
 * @brief Generate a cached printer image at exact dimensions
 *
 * Loads the source image, resizes to exact width x height, converts to LVGL
 * ARGB8888 binary format, and writes atomically to the cache path.
 *
 * @param source_image_path The resolved source image path (LVGL path with A: prefix)
 * @param width Target width in pixels
 * @param height Target height in pixels
 * @param output_path Filesystem path for the output .bin file
 * @return true on success
 */
bool generate_cached_printer_image(const std::string& source_image_path, int width, int height,
                                   const std::string& output_path);

/**
 * @brief Prune old cached printer images
 *
 * If the cache directory has more than max_files entries, deletes the oldest
 * by modification time until max_keep remain.
 *
 * @param max_files Trigger pruning when file count exceeds this (default: 10)
 * @param max_keep Keep this many files after pruning (default: 5)
 */
void prune_printer_image_cache(int max_files = 10, int max_keep = 5);

} // namespace helix
