// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lvgl/lvgl.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief Manages custom and shipped printer images for the home panel
 *
 * Handles image selection, import/conversion from PNG/JPEG to LVGL .bin format,
 * and resolution of the active image path. Supports both shipped images
 * (bundled with the app) and custom user-imported images.
 *
 * Image IDs use a namespace prefix:
 *   - "shipped:voron-v2"    — bundled printer image
 *   - "custom:my-printer"   — user-imported image
 *   - ""                    — auto-detect from printer type (default)
 */
class PrinterImageManager {
  public:
    static PrinterImageManager& instance();

    PrinterImageManager(const PrinterImageManager&) = delete;
    PrinterImageManager& operator=(const PrinterImageManager&) = delete;

    /// Initialize with config directory (creates custom_images/ subdir)
    void init(const std::string& config_dir = "config");

    // --- Active image resolution ---

    /// Get the active image ID from config ("shipped:name", "custom:name", or "" for auto)
    std::string get_active_image_id() const;

    /// Get the LVGL image path for the active image.
    /// Returns "" if auto-detect (caller uses existing printer_type logic).
    /// screen_width determines 300px vs 150px variant.
    std::string get_active_image_path(int screen_width);

    /// Set active image ID and persist to config.
    /// Must be called from the UI thread (fires lv_subject notification).
    void set_active_image(const std::string& id);

    /** Format a filename stem into a human-readable display name.
     *  Replaces dashes/underscores with spaces, except dashes between
     *  two digits become dots (e.g., "voron-0-2" → "voron 0.2"). */
    static std::string format_display_name(const std::string& stem);

    // --- Browsing ---

    struct ImageInfo {
        std::string id;           // "shipped:voron-v2" or "custom:my-printer"
        std::string display_name; // "Voron 2.4" or "my-printer"
        std::string preview_path; // LVGL path for thumbnail preview
    };

    /// @param screen_width Display width; picks which prerendered tier the
    ///        previews point at. Packaging ships only one.
    std::vector<ImageInfo> get_shipped_images(int screen_width) const;
    std::vector<ImageInfo> get_custom_images() const;

    /// Get raw files in custom_images/ that failed import (no corresponding .bin)
    std::vector<ImageInfo> get_invalid_custom_images() const;

    /// Auto-import any raw PNG/JPEG files in custom_images/ that lack .bin variants
    int auto_import_raw_images();

    /// Scan directory for importable PNG/JPEG files
    std::vector<std::string> scan_for_images(const std::string& dir) const;

    // --- Import + conversion ---

    struct ImportResult {
        bool success = false;
        std::string id;    // "custom:name" on success
        std::string error; // Error message on failure
    };

    /// Import and convert a PNG/JPEG to LVGL .bin format (synchronous)
    ImportResult import_image(const std::string& source_path);

    /// Async version — callback on completion
    void import_image_async(const std::string& source_path,
                            std::function<void(ImportResult)> callback);

    // --- Cleanup ---

    bool delete_custom_image(const std::string& name);
    std::string get_custom_dir() const {
        return custom_dir_;
    }

    /// Subject that increments each time the active image changes (observe for refresh)
    lv_subject_t* get_image_changed_subject() {
        return &image_changed_subject_;
    }

    void deinit_subjects();

  private:
    PrinterImageManager() = default;
    ~PrinterImageManager() = default;

    std::string custom_dir_;               // e.g., "config/custom_images/"
    lv_subject_t image_changed_subject_{}; // Version counter bumped on set_active_image()
    bool subjects_initialized_ = false;

    struct ValidationResult {
        bool valid = false;
        std::string error;
        int width = 0;
        int height = 0;
    };

    ValidationResult validate_image(const std::string& path) const;
    bool convert_to_bin(const uint8_t* pixels, int w, int h, const std::string& output_path,
                        int target_size);
};

} // namespace helix
