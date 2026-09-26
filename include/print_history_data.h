// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "moonraker_types.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Time filter for history dashboard queries
 */
enum class HistoryTimeFilter {
    DAY,     ///< Last 24 hours
    WEEK,    ///< Last 7 days
    MONTH,   ///< Last 30 days
    YEAR,    ///< Last 365 days
    ALL_TIME ///< No time filter
};

/**
 * @brief Print job status from Moonraker history
 */
enum class PrintJobStatus { UNKNOWN = 0, COMPLETED, CANCELLED, ERROR, IN_PROGRESS };

/**
 * @brief Single print job from Moonraker history
 *
 * Maps to server.history.list response structure.
 * @see https://moonraker.readthedocs.io/en/latest/web_api/#get-job-list
 */
struct PrintHistoryJob {
    std::string job_id;   ///< Unique job identifier
    std::string filename; ///< G-code filename
    PrintJobStatus status = PrintJobStatus::UNKNOWN;
    double start_time = 0.0;     ///< Unix timestamp
    double end_time = 0.0;       ///< Unix timestamp
    double print_duration = 0.0; ///< Seconds of actual printing
    double total_duration = 0.0; ///< Total job time including pauses
    double filament_used = 0.0;  ///< Filament in mm
    bool exists = false;         ///< File still exists on disk

    // Metadata from G-code file
    std::string filament_type; ///< PLA, PETG, ABS, etc.
    uint32_t layer_count = 0;
    double layer_height = 0.0;
    double nozzle_temp = 0.0;
    double bed_temp = 0.0;
    std::string thumbnail_path; ///< Largest thumbnail relative path (for simple consumers)
    std::vector<ThumbnailInfo> thumbnails; ///< All available thumbnails with dimensions
    std::string uuid;                      ///< Slicer-generated UUID (from metadata.uuid)
    size_t size_bytes = 0;                 ///< File size in bytes (from metadata.size)
    /// Source gcode mtime (metadata.modified), Unix timestamp. 0 when Moonraker
    /// omitted it — consumers treat 0 as "no freshness information", which is
    /// what ThumbnailCache's source_modified == 0 already means.
    double modified = 0.0;

    // Pre-formatted strings for display (set during parsing)
    std::string duration_str; ///< "2h 15m"
    std::string date_str;     ///< "Dec 1, 14:30"
    std::string filament_str; ///< "12.5m"

    // Timelapse association (Phase 5)
    std::string
        timelapse_filename;     ///< Associated timelapse file (e.g., "timelapse/print_2024...mp4")
    bool has_timelapse = false; ///< True if timelapse file was found for this job
};

/**
 * @brief Aggregated history statistics
 *
 * Maps to server.history.totals response.
 */
struct PrintHistoryTotals {
    uint64_t total_jobs = 0;
    uint64_t total_time = 0;          ///< Seconds
    double total_filament_used = 0.0; ///< mm
    uint64_t total_completed = 0;
    uint64_t total_cancelled = 0;
    uint64_t total_failed = 0;
    double longest_job = 0.0; ///< Seconds
};

/**
 * @brief Filament usage aggregated by material type (for future charts)
 */
struct FilamentUsageByType {
    std::string type; ///< "PLA", "PETG", etc.
    double usage_mm = 0.0;
    uint32_t print_count = 0;
};

/**
 * @brief Convert status string from Moonraker to enum
 * @param status Status string from Moonraker history API
 * @return Corresponding enum value, UNKNOWN if unrecognized
 *
 * Moonraker status strings:
 * - "completed" - Job finished successfully
 * - "cancelled" - User cancelled the job
 * - "error" - Print failed due to error
 * - "in_progress" / "printing" - Job currently active
 * - "klippy_shutdown" - Klipper shutdown mid-print
 * - "klippy_disconnect" - Connection lost mid-print
 * - "server_exit" - Moonraker shutdown mid-print
 * - "interrupted" - Job detected as interrupted on startup
 */
[[nodiscard]] inline PrintJobStatus parse_job_status(const std::string& status) {
    if (status == "completed")
        return PrintJobStatus::COMPLETED;
    if (status == "cancelled")
        return PrintJobStatus::CANCELLED;
    // Error states from Moonraker lifecycle events
    if (status == "error" || status == "klippy_shutdown" || status == "klippy_disconnect" ||
        status == "server_exit" || status == "interrupted")
        return PrintJobStatus::ERROR;
    // Active print states
    if (status == "in_progress" || status == "printing")
        return PrintJobStatus::IN_PROGRESS;
    return PrintJobStatus::UNKNOWN;
}

/**
 * @brief Get the display word for a status
 * @param status Job status
 * @return Untranslated label, which is also its translation tag ("Completed", "Failed", ...)
 */
[[nodiscard]] inline const char* status_to_label(PrintJobStatus status) {
    switch (status) {
    case PrintJobStatus::COMPLETED:
        return "Completed";
    case PrintJobStatus::CANCELLED:
        return "Cancelled";
    case PrintJobStatus::ERROR:
        return "Failed";
    case PrintJobStatus::IN_PROGRESS:
        return "In Progress";
    default:
        return "Unknown";
    }
}

/**
 * @brief Get icon name for status (Material Design Icons)
 * @param status Job status
 * @return Icon name for use in XML
 */
[[nodiscard]] inline const char* status_to_icon(PrintJobStatus status) {
    switch (status) {
    case PrintJobStatus::COMPLETED:
        return "check_circle";
    case PrintJobStatus::CANCELLED:
        return "close_circle";
    case PrintJobStatus::ERROR:
        return "alert";
    case PrintJobStatus::IN_PROGRESS:
        return "clock";
    default:
        return "info";
    }
}

/**
 * @brief Get style variant for status (maps to theme colors)
 * @param status Job status
 * @return Variant name for XML styling
 */
[[nodiscard]] inline const char* status_to_variant(PrintJobStatus status) {
    switch (status) {
    case PrintJobStatus::COMPLETED:
        return "success";
    case PrintJobStatus::CANCELLED:
        return "warning";
    case PrintJobStatus::ERROR:
        return "error";
    case PrintJobStatus::IN_PROGRESS:
        return "info";
    default:
        return "secondary";
    }
}
