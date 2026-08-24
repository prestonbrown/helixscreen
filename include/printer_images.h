// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "data_root_resolver.h"
#include "lvgl/lvgl.h"
#include "prerendered_images.h"
#include "printer_detector.h"

#include <filesystem>
#include <string>

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

namespace PrinterImages {

/// Base path for printer images (LVGL filesystem prefix)
inline constexpr const char* IMAGE_BASE_PATH = "A:assets/images/printers/";

/// Base path for pre-rendered printer images (faster on embedded)
inline constexpr const char* PRERENDERED_BASE_PATH = "A:assets/images/printers/prerendered/";

/// Default fallback image for unknown/unmapped printers (generic CoreXY)
inline constexpr const char* DEFAULT_IMAGE = "A:assets/images/printers/generic-corexy.png";

/// Default image filename (without path)
inline constexpr const char* DEFAULT_IMAGE_FILENAME = "generic-corexy.png";

/// Resolve the generic fallback image through the asset root. Identity on
/// desktop (asset_root "."); on ESP32 (asset_root "/assets") it prepends the
/// mount that a raw "A:assets/..." literal misses. Used wherever DEFAULT_IMAGE
/// would otherwise be returned raw.
inline std::string default_printer_image_path() {
    return helix::asset_component_uri("assets/images/printers/generic-corexy.png");
}

/// Pre-rendered image size for wizard/home (300px width, maintains aspect ratio)
inline constexpr int PRERENDERED_SIZE = 300;

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
    return !fs_path.empty() && std::filesystem::exists(fs_path);
}

/**
 * @brief Get pre-rendered image path for a filename
 *
 * Converts a PNG filename (e.g., "voron-v2.png") to its pre-rendered
 * binary equivalent (e.g., "voron-v2-300.bin").
 *
 * @param image_filename Original filename from database (e.g., "voron-v2.png")
 * @return Full LVGL path to pre-rendered image, or empty string if not found
 */
inline std::string get_prerendered_path(const std::string& image_filename) {
    // Convert "name.png" to "name-300.bin"
    size_t dot_pos = image_filename.rfind('.');
    if (dot_pos == std::string::npos) {
        return "";
    }

    std::string basename = image_filename.substr(0, dot_pos);
    std::string prerendered_name = basename + "-" + std::to_string(PRERENDERED_SIZE) + ".bin";
    std::string full_path = std::string(PRERENDERED_BASE_PATH) + prerendered_name;

    if (image_file_exists(full_path)) {
        return full_path;
    }
    return "";
}

/**
 * @brief Get image path for a printer name (from database)
 *
 * Looks up the image in the printer database JSON and constructs the full
 * LVGL path. Prefers pre-rendered .bin files for performance on embedded
 * devices, falls back to PNG if not available.
 *
 * @param printer_name Printer name (e.g., "Voron 2.4", "FlashForge Adventurer 5M")
 * @return Full LVGL path to printer image
 */
inline std::string get_image_path_for_name(const std::string& printer_name) {
    // Look up image filename from database
    std::string image_filename = PrinterDetector::get_image_for_printer(printer_name);

    if (!image_filename.empty()) {
        // Try pre-rendered binary first (much faster on embedded)
        std::string prerendered = get_prerendered_path(image_filename);
        if (!prerendered.empty()) {
            return prerendered;
        }

        // Fall back to original PNG (routed through the asset root so the
        // /assets mount is applied on ESP32; identity on desktop).
        std::string full_path =
            helix::asset_component_uri("assets/images/printers/" + image_filename);
        if (image_file_exists(full_path)) {
            return full_path;
        }
    }

    // Fall back to default
    return default_printer_image_path();
}

/**
 * @brief Get the best available printer image for a printer type name
 *
 * Performs the full smart image lookup:
 * 1. Looks up image filename from printer database
 * 2. Strips .png extension to get base name
 * 3. Selects responsive size based on current screen width (300px or 150px)
 * 4. Prefers pre-rendered .bin (faster on embedded), falls back to PNG
 * 5. Falls back to generic CoreXY image if nothing found
 *
 * This is the recommended function for displaying printer images.
 *
 * @param printer_type Printer type name from config (e.g., "Voron 2.4", "FlashForge Adventurer 5M")
 * @return Full LVGL path to the best available image
 */
inline std::string get_best_printer_image(const std::string& printer_type) {
    if (printer_type.empty()) {
        return default_printer_image_path();
    }

    // Look up image filename from printer database
    std::string image_filename = PrinterDetector::get_image_for_printer(printer_type);
    if (image_filename.empty()) {
        return default_printer_image_path();
    }

    // Strip .png extension to get base name for prerendered lookup
    std::string base_name = image_filename;
    if (base_name.size() > 4 && base_name.substr(base_name.size() - 4) == ".png") {
        base_name = base_name.substr(0, base_name.size() - 4);
    }

    // Get screen width for responsive image sizing
    lv_display_t* disp = lv_display_get_default();
    int screen_width = disp ? lv_display_get_horizontal_resolution(disp) : 800;

    // Use prerendered .bin if available (screen-responsive), else fallback to PNG
    return helix::get_prerendered_printer_path(base_name, screen_width);
}

/**
 * @brief Get image path for a printer type index
 *
 * Converts index to printer name, then looks up image in database.
 * Falls back to DEFAULT_IMAGE if not found or file doesn't exist.
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

} // namespace PrinterImages
