// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/data_root_resolver.h"
#include "lvgl/lvgl.h"
#include "prerender_size_class.h"
#include "prerendered_images.h"
#include "printer_detector.h"

#include <string>
#include <sys/stat.h>

/**
 * @file printer_images.h
 * @brief Printer type to image path mapping
 *
 * Provides image path lookups for printer types using the unified printer database
 * (config/printer_database.json). Falls back to generic CoreXY image when a printer
 * image is not found or the file doesn't exist on disk.
 *
 * Image paths are stored in the database as just filenames (e.g., "voron-v2.png").
 * This header adds the full LVGL path prefix "A:assets/images/printers/".
 */

namespace helix::printer_images {

/// Base path for printer images (LVGL filesystem prefix)
inline constexpr const char* kImageBasePath = "A:assets/images/printers/";

/// Base path for pre-rendered printer images (faster on embedded)
inline constexpr const char* kPrerenderedBasePath = "A:assets/images/printers/prerendered/";

/// Default fallback image for unknown/unmapped printers (generic CoreXY)
inline constexpr const char* kDefaultImage = "A:assets/images/printers/generic-corexy.png";

/// Default image filename (without path)
inline constexpr const char* kDefaultImageFilename = "generic-corexy.png";

/// Resolve the generic fallback image through the asset root. Identity on
/// desktop (asset_root "."); on ESP32 (asset_root "/assets") it prepends the
/// mount that a raw "A:assets/..." literal misses. Used wherever kDefaultImage
/// would otherwise be returned raw.
inline std::string default_printer_image_path() {
    return helix::asset_component_uri("assets/images/printers/generic-corexy.png");
}

/// The display width the app is actually running at, or a desktop-ish default
/// before one exists. Every prerendered lookup sizes off this: packaging keeps
/// exactly the tier this width selects and deletes the other, so a hardcoded
/// size asks for a file the device does not have.
inline int current_screen_width() {
    lv_display_t* disp = lv_display_get_default();
    return disp ? lv_display_get_horizontal_resolution(disp) : 800;
}

/**
 * @brief Get printer name from type index
 *
 * Uses the dynamic list built from the printer database.
 *
 * @param printer_type_index Index from printer type list
 * @return Printer name string (e.g., "Voron 2.4")
 */
inline std::string get_printer_name(int printer_type_index) {
    return PrinterDetector::get_list_name_at(printer_type_index);
}

/**
 * @brief Convert LVGL path (A:...) to filesystem path
 *
 * @param lvgl_path Path with "A:" prefix
 * @return Filesystem path without prefix
 */
inline std::string lvgl_to_fs_path(const char* lvgl_path) {
    if (lvgl_path && lvgl_path[0] == 'A' && lvgl_path[1] == ':') {
        return std::string(lvgl_path + 2); // Skip "A:" prefix
    }
    return lvgl_path ? lvgl_path : "";
}

/**
 * @brief Check if a file exists at the given LVGL path
 *
 * @param lvgl_path Path with "A:" prefix
 * @return true if file exists, false otherwise
 */
inline bool image_file_exists(const std::string& lvgl_path) {
    std::string fs_path = lvgl_to_fs_path(lvgl_path.c_str());
    if (fs_path.empty())
        return false;
    // Resolved against the asset root so the /assets mount is applied on
    // firmware; identity on desktop. A plain stat, not helix::fs: this header
    // is included too widely to bring that namespace into files that alias
    // std::filesystem as `fs`. Any stat failure is "absent" (the ESP32 VFS
    // reports a missing path as ENODATA).
    struct stat st {};
    return ::stat(helix::asset_path(fs_path).c_str(), &st) == 0;
}

/**
 * @brief Get pre-rendered image path for a filename
 *
 * Converts a PNG filename (e.g., "voron-v2.png") to its pre-rendered
 * binary equivalent (e.g., "voron-v2-300.bin").
 *
 * @param image_filename Original filename from database (e.g., "voron-v2.png")
 * @param screen_width Display width; selects the tier. Defaults to the live
 *        display, and is a parameter so a caller without one (a test, a probe
 *        before the display exists) can name the width it means.
 * @return Full LVGL path to pre-rendered image, or empty string if not found
 */
inline std::string get_prerendered_path(const std::string& image_filename,
                                        int screen_width = current_screen_width()) {
    // Convert "name.png" to "name-300.bin"
    size_t dot_pos = image_filename.rfind('.');
    if (dot_pos == std::string::npos) {
        return "";
    }

    std::string basename = image_filename.substr(0, dot_pos);
    const int size = helix::get_printer_image_size(screen_width);
    std::string prerendered_name = basename + "-" + std::to_string(size) + ".bin";
    std::string full_path = std::string(kPrerenderedBasePath) + prerendered_name;

    if (image_file_exists(full_path)) {
        return full_path;
    }
    return "";
}

/**
 * @brief The best available render of one database image file
 *
 * Strips the .png extension and resolves the screen-sized prerendered .bin,
 * falling back to the PNG, then to the generic CoreXY image.
 *
 * @param image_filename Filename from the printer database (e.g. "voron-v2.png"); "" for none
 * @param screen_width Display width; selects the tier
 * @return Full LVGL path to the best available image
 */
inline std::string get_best_image_for_file(const std::string& image_filename,
                                           int screen_width = current_screen_width()) {
    if (image_filename.empty()) {
        return default_printer_image_path();
    }
    std::string base_name = image_filename;
    if (base_name.size() > 4 && base_name.substr(base_name.size() - 4) == ".png") {
        base_name = base_name.substr(0, base_name.size() - 4);
    }
    return helix::get_prerendered_printer_path(base_name, screen_width);
}

/**
 * @brief Get image path for a printer name (from database)
 *
 * The name's plain database `image`, through get_best_image_for_file(). It
 * ignores `image_variants`: what the connected printer shows, variants
 * included, is PrinterImageManager::get_detected_image_path().
 *
 * @param printer_name Printer name (e.g., "Voron 2.4", "FlashForge Adventurer 5M")
 * @return Full LVGL path to printer image; the generic image for an unknown name
 */
inline std::string get_image_path_for_name(const std::string& printer_name,
                                           int screen_width = current_screen_width()) {
    return get_best_image_for_file(PrinterDetector::get_image_for_printer(printer_name),
                                   screen_width);
}

/**
 * @brief Get image path for a printer type index
 *
 * Converts index to printer name, then looks up image in database.
 * Falls back to kDefaultImage if not found or file doesn't exist.
 *
 * @param printer_type_index Index from printer type list
 * @return Full LVGL path to printer image
 */
inline std::string get_image_path(int printer_type_index) {
    std::string printer_name = get_printer_name(printer_type_index);
    return get_image_path_for_name(printer_name);
}

/**
 * @brief Get validated image path for a printer type, with fallback
 *
 * This is the primary function to use for displaying printer images.
 * It handles all lookup and validation logic internally.
 *
 * @param printer_type_index Index from printer type list
 * @return Full LVGL path to printer image (guaranteed to exist or be default)
 *
 * @note Returns a std::string that manages its own memory. The caller
 *       must keep the string alive while using the path.
 */
inline std::string get_validated_image_path(int printer_type_index) {
    return get_image_path(printer_type_index);
}

} // namespace helix::printer_images
