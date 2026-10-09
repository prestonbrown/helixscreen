// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(HELIX_PLATFORM_ESP32)
#include "esp_psram_thumbnail.h"
#endif

// Forward declarations for factory methods (avoid header coupling)
struct FileInfo;
struct UsbGcodeFile;

/**
 * @brief Print history status for file list display
 *
 * Status values in priority order (for display):
 * - CURRENTLY_PRINTING: Active print (blue clock icon)
 * - COMPLETED: Last print succeeded (green checkmark with count)
 * - FAILED: Last print failed (red error icon)
 * - CANCELLED: Last print was cancelled by user (orange warning icon)
 * - NEVER_PRINTED: No history record (empty/blank)
 */
enum class FileHistoryStatus {
    NEVER_PRINTED = 0,  ///< No history record
    CURRENTLY_PRINTING, ///< Matches active print filename
    COMPLETED,          ///< Last print completed successfully
    FAILED,             ///< Last print failed (error)
    CANCELLED           ///< Last print was cancelled by user
};

/**
 * @brief File data for print selection
 *
 * Holds file metadata and display strings for print file list/card/detail views.
 */
struct PrintFileData {
    std::string filename;
    std::string local_path;     ///< Absolute path on this host for a file Moonraker cannot see
                                ///< (a USB stick); empty for Moonraker's own files
    std::string thumbnail_path; ///< Pre-scaled .bin path for cards (fast rendering)
    std::string original_thumbnail_url; ///< Moonraker relative URL (for detail view PNG lookup)
    size_t file_size_bytes;             ///< File size in bytes
    std::string uuid;                   ///< Slicer UUID from metadata (empty if not available)
    time_t modified_timestamp;          ///< Last modified timestamp
    int print_time_minutes;             ///< Print time in minutes
    float filament_grams;               ///< Filament weight in grams
    std::string filament_type; ///< Filament type (e.g., "PLA", "PETG", "ABS") — first tool only
    std::string
        filament_name; ///< Full filament name (e.g., "PolyMaker PolyLite ABS") — first tool only
    std::vector<std::string>
        filament_types; ///< Per-tool filament types parsed from semicolon-separated metadata
    std::vector<std::string>
        filament_names; ///< Per-tool filament names parsed from semicolon-separated metadata
    uint64_t gcode_end_byte = 0; ///< Offset where the G-code body ends (0 = not reported).
                                 ///< Everything past it is the slicer footer, which carries
                                 ///< the per-tool usage + color lines.
    uint32_t layer_count = 0;    ///< Total layer count from slicer
    double object_height = 0.0;  ///< Object height in mm
    double layer_height = 0.0;   ///< Layer height in mm (e.g., 0.24)
    bool is_dir = false;         ///< True if this is a directory
    std::vector<std::string>
        filament_colors; ///< Hex colors per tool (e.g., ["#ED1C24", "#00C1AE"])

    // Formatted strings (cached for performance)
    std::string size_str;
    std::string modified_str;
    std::string print_time_str;
    std::string filament_str;
    std::string layer_count_str;  ///< Formatted layer count string
    std::string print_height_str; ///< Formatted print height string
    std::string layer_height_str; ///< Formatted layer height string (e.g., "0.24 mm")

    // Metadata loading state (travels with file during sorting)
    bool metadata_fetched = false; ///< True if metadata has been loaded

    // Print history status (from PrintHistoryManager)
    FileHistoryStatus history_status = FileHistoryStatus::NEVER_PRINTED;
    int success_count = 0; ///< Number of successful prints (shown as "N ✓")

#if defined(HELIX_PLATFORM_ESP32)
    // PSRAM-resident thumbnail (Task 11 R2). ESP32 has no disk thumbnail
    // cache (Task 10 R6), so a fetched thumbnail lives here instead of at
    // thumbnail_path. shared_ptr so list sort/merge copies share one PSRAM
    // allocation rather than re-copying image bytes.
    std::shared_ptr<helix::ui::EspPsramThumbnail> esp_thumbnail;
    /// A fetch was started for this file while its card has been on screen:
    /// in flight, done, or failed. Cleared when the card leaves the screen
    /// holding nothing, so a failed thumbnail is tried again only when the card
    /// is next shown.
    bool esp_thumbnail_tried = false;
    /// Set to cancel the fetch started for this card, should it leave the screen
    /// first: the lane drops the request unsent, or the decode is skipped.
    std::shared_ptr<std::atomic<bool>> esp_fetch_cancel;
    /// Its fetch failed and was started once more while the card is shown.
    bool esp_fetch_retried = false;
    /// The print-select sync tick at which its card was last on screen.
    uint32_t esp_thumbnail_shown = 0;
#endif

    // ========================================================================
    // FACTORY METHODS
    // ========================================================================

    /**
     * @brief Create PrintFileData from Moonraker FileInfo
     *
     * Populates basic file info (filename, size, modified time) and sets
     * placeholder values for metadata fields. The thumbnail_path is set to
     * the default placeholder.
     *
     * @param file FileInfo from Moonraker file listing API
     * @param default_thumbnail Path to default/placeholder thumbnail
     * @return Initialized PrintFileData with formatted strings
     */
    static PrintFileData from_moonraker_file(const FileInfo& file,
                                             const std::string& default_thumbnail);

    /**
     * @brief Create PrintFileData from USB G-code file
     *
     * USB files don't have Moonraker metadata, so print_time, filament, etc.
     * are set to defaults. Formatted strings use "--" for unavailable fields.
     *
     * @param file UsbGcodeFile from USB manager scan
     * @param default_thumbnail Path to default/placeholder thumbnail
     * @return Initialized PrintFileData with formatted strings
     */
    static PrintFileData from_usb_file(const UsbGcodeFile& file,
                                       const std::string& default_thumbnail);

    /**
     * @brief Create a directory entry
     *
     * @param name Directory name (e.g., ".." for parent, "folder_name" for subdirs)
     * @param icon_path Path to folder icon
     * @param is_parent True if this is the parent directory entry ".."
     * @return Initialized PrintFileData for directory display
     */
    static PrintFileData make_directory(const std::string& name, const std::string& icon_path,
                                        bool is_parent = false);
};

namespace helix {

/// Where a USB file's copy goes in Moonraker's copy folder.
struct UsbCopyTarget {
    std::string name;   ///< Filename inside the copy folder
    bool reuse = false; ///< A same-size file is already there: print it, upload nothing
};

/**
 * @brief Name a USB file's copy without replacing a different file
 *
 * Tries @p filename, then "<stem> (2).<ext>", "<stem> (3).<ext>", ...: the
 * first name that is free is uploaded to, and the first that already holds a
 * file of @p size is reused. Size is the identity test, since the copy folder
 * is only ever filled from sticks.
 *
 * @param existing Filenames already in the copy folder, with their sizes
 */
UsbCopyTarget choose_usb_copy_target(const std::string& filename, uint64_t size,
                                     const std::map<std::string, uint64_t>& existing);

} // namespace helix

/**
 * @brief Decide whether to carry forward cached metadata from a previous file listing
 * into a fresh Moonraker listing during the print-select panel's merge step.
 *
 * Pure function — lives in this lightweight header so it can be unit-tested without
 * pulling in the full PrintSelectPanel header (which transitively includes LVGL/XML).
 *
 * Rules:
 * - Only candidates are entries whose previous fetch claimed success
 *   (metadata_fetched == true). Everything else needs a fresh fetch anyway.
 * - Drop the cache if the file was re-sliced (size changed).
 * - Drop the cache on panel activation if the metadata named no thumbnail at all:
 *   neither a local thumbnail_path nor an original_thumbnail_url. This self-heals
 *   files whose upload-time metadata extraction failed transiently in Moonraker
 *   (JSON-RPC -32601 "Metadata not available"): without this one-shot retry,
 *   metadata_fetched stays true forever and the card shows the placeholder
 *   permanently even after Moonraker recovers. A URL with no local path is not a
 *   failed extraction: a platform without a thumbnail disk cache never sets the
 *   path, and dropping those entries throws away the thumbnails they hold.
 *
 * @param old_entry Cached entry from previous file_list_
 * @param new_file_size File size from the fresh Moonraker listing
 * @param retry_missing_thumbnails True on panel activation: drop cached entries
 *                                 whose metadata named no thumbnail so they get
 *                                 one retry this visit
 * @return true to carry forward cached metadata, false to let it re-fetch fresh
 */
inline bool should_carry_forward_print_file_metadata(const PrintFileData& old_entry,
                                                     size_t new_file_size,
                                                     bool retry_missing_thumbnails) {
    if (!old_entry.metadata_fetched) {
        return false;
    }
    if (new_file_size != old_entry.file_size_bytes) {
        return false;
    }
    if (retry_missing_thumbnails && old_entry.thumbnail_path.empty() &&
        old_entry.original_thumbnail_url.empty()) {
        return false;
    }
    return true;
}

/**
 * @brief Carry cached metadata from the previous listing into a fresh one
 *
 * Each fresh entry whose file already had metadata takes the whole cached entry,
 * keeping only the listing's size and modified time; one the cache cannot vouch
 * for (see should_carry_forward_print_file_metadata) is set to fetch again. A
 * cached entry is handed out once: a name listed twice fetches fresh for the
 * second, rather than reading an entry already moved out.
 *
 * @param files Fresh listing, updated in place
 * @param previous Previous file list; its entries are moved from
 * @param retry_missing_thumbnails See should_carry_forward_print_file_metadata
 */
namespace helix {
inline void carry_forward_print_file_metadata(std::vector<PrintFileData>& files,
                                              std::vector<PrintFileData>& previous,
                                              bool retry_missing_thumbnails) {
    std::unordered_map<std::string, PrintFileData> cached;
    for (auto& f : previous) {
        if (f.metadata_fetched) {
            cached.emplace(f.filename, std::move(f));
        }
    }
    for (auto& f : files) {
        auto it = cached.find(f.filename);
        if (it == cached.end()) {
            continue;
        }
        const time_t modified = f.modified_timestamp;
        const size_t size = f.file_size_bytes;
        if (should_carry_forward_print_file_metadata(it->second, size, retry_missing_thumbnails)) {
#if defined(HELIX_PLATFORM_ESP32)
            // A re-upload of the same size is a different picture.
            if (it->second.modified_timestamp != modified) {
                if (it->second.esp_fetch_cancel) {
                    it->second.esp_fetch_cancel->store(true);
                    it->second.esp_fetch_cancel.reset();
                }
                it->second.esp_thumbnail.reset();
                it->second.esp_thumbnail_tried = false;
                it->second.esp_fetch_retried = false;
            }
#endif
            f = std::move(it->second);
            f.modified_timestamp = modified;
            f.file_size_bytes = size;
        } else {
            // The provider may have kept metadata_fetched / thumbnail_path on the
            // fresh entry; left set, the next fetch would skip it.
            f.metadata_fetched = false;
            f.thumbnail_path.clear();
        }
        cached.erase(it);
    }
}
} // namespace helix

/**
 * @brief Truncate a sorted print-file list to the newest max_files entries
 *        (ESP32 print-select cap, Task 11 R1).
 *
 * Assumes the list has already been sorted directories-first with newest
 * files first (see PrintSelectFileSorter::apply_sort — the is_dir tiebreak
 * there is checked before the sort-direction flip, so std::sort always
 * groups every directory entry ahead of every file entry, regardless of
 * which column/direction the user picked). Under that invariant, walking
 * the list and counting only non-directory entries finds the exact index
 * where the (max_files+1)-th file starts; every directory a user could
 * still navigate into precedes that index, so resizing there drops only
 * older files, never a directory.
 *
 * Pure function — kept in this lightweight header (not ui_panel_print_select.cpp)
 * so tests can inject the cap value instead of relying on the compiled-in
 * ESP32 constant.
 *
 * @param files Sorted file list (dirs first, then files newest-first)
 * @param max_files Maximum non-directory entries to keep
 * @return true if any files were dropped (caller may surface a "more files
 *         on the printer" affordance)
 */
inline bool cap_print_file_list_to_newest(std::vector<PrintFileData>& files, size_t max_files) {
    size_t files_seen = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].is_dir) {
            continue;
        }
        if (files_seen == max_files) {
            files.resize(i);
            return true;
        }
        ++files_seen;
    }
    return false;
}
