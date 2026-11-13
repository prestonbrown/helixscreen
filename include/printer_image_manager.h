// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/observer_guard.h"
#include "subject_managed_panel.h" // SubjectManager

#include <lvgl/lvgl.h>

#include <cstdint>
#include <functional>
#include <memory>
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

    /// Bring up the image-changed subject on its own. Idempotent; init() calls it too.
    /// Panels observe the subject from their constructors, so it must exist before them.
    void init_subjects();

    // --- Active image resolution ---

    /// Get the active image ID from config ("shipped:name", "custom:name", or "" for auto)
    std::string get_active_image_id() const;

    /// Get the LVGL image path for the active image.
    /// Returns "" if auto-detect (caller uses existing printer_type logic).
    /// screen_width determines 300px vs 150px variant.
    std::string get_active_image_path(int screen_width);

    /// The auto-detected image: the configured printer type's database image,
    /// or the first of its `image_variants` the detected hardware matches.
    /// Main thread only (reads AmsState's backends).
    std::string get_detected_image_path(int screen_width);

    /// The image the home printer widget shows: the active image, else
    /// get_detected_image_path().
    std::string get_displayed_image_path(int screen_width);

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

    /// Import and convert a PNG/JPEG to LVGL .bin format (synchronous). Clears
    /// the image's user tags, so main thread only.
    ImportResult import_image(const std::string& source_path);

    /// Async version — callback on completion
    void import_image_async(const std::string& source_path,
                            std::function<void(ImportResult)> callback);

    // --- Cleanup ---

    /// Removes both tiers and the image's user tags. Main thread only.
    bool delete_custom_image(const std::string& name);
    std::string get_custom_dir() const {
        return custom_dir_;
    }

    /// A subject whose change can change which image auto-detection shows.
    struct DetectionInput {
        lv_subject_t* subject;
        SubjectLifetime lifetime;
    };
    /// Every such subject: filament-system units reporting in (they register
    /// after their backend), a removable enclosure top fitted or lifted, and a
    /// chamber heater assigned by hand. Anything showing the displayed image
    /// re-resolves it when one changes.
    static std::vector<DetectionInput> detection_inputs();

    /// Bump the image-changed subject, so the home widget redraws the image and
    /// its callouts. set_active_image() calls it; so does anything that changes
    /// how the same image is drawn, like saving its tags.
    void notify_image_changed();

    /// Subject that increments each time the active image changes (observe for refresh)
    lv_subject_t* get_image_changed_subject() {
        return &image_changed_subject_;
    }

    /// Death signal for the subject this singleton owns.
    ///
    /// deinit_subjects() frees every observer node on it without bumping the
    /// ObserverGuard invalidation epoch, so outside observers must pass this to
    /// observe_*(), or their guards call lv_observer_delete() on a freed node.
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    void deinit_subjects();

  private:
    friend class PrinterImageManagerTestAccess;

    PrinterImageManager() = default;
    ~PrinterImageManager() = default;

    std::string custom_dir_;               // e.g., "config/custom_images/"
    lv_subject_t image_changed_subject_{}; // Version counter bumped on set_active_image()
    bool subjects_initialized_ = false;
    /// Owns image_changed_subject_ and the death signal
    /// get_subjects_lifetime() hands out.
    SubjectManager subjects_;

    struct ValidationResult {
        bool valid = false;
        std::string error;
        int width = 0;
        int height = 0;
    };

    ValidationResult validate_image(const std::string& path) const;

    /**
     * @brief Invalidate every cached size generated from one source image
     *
     * Removes the dimension-specific cache files in the printer image cache directory
     * that printer_cache_entry_matches() attributes to this source. For
     * "A:config/custom_images/my-printer-300.bin" that is
     * "my-printer-300-480x320-<mtime>-<size>.bin" and its siblings, and nothing
     * belonging to another source that shares the stem.
     *
     * Entries are already keyed on the source's mtime and size, so a rewrite in place
     * is never served from a stale one. This is the prompt cleanup of files that can
     * no longer be named, not a correctness requirement. Private because every
     * deletion costs a decode-and-resize and a flash write to rebuild, and a UI
     * refresh that re-resolves the same image has nothing stale to drop.
     *
     * @param source_image_path The LVGL source path (with or without A: prefix)
     * @return Number of cache files removed
     */
    static int invalidate_printer_image_cache(const std::string& source_image_path);

    bool convert_to_bin(const uint8_t* pixels, int w, int h, const std::string& output_path,
                        int target_size);
};

} // namespace helix
