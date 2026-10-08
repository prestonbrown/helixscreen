// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_file_data.h"

#include "ui_format_utils.h"

#include "moonraker_types.h"
#include "usb_backend.h"

using helix::ui::format_filament_weight;
using helix::ui::format_file_size;
using helix::ui::format_modified_date;
using helix::ui::format_print_time;

// ============================================================================
// FACTORY METHODS
// ============================================================================

PrintFileData PrintFileData::from_moonraker_file(const FileInfo& file,
                                                 const std::string& default_thumbnail) {
    PrintFileData data;
    data.filename = file.filename;
    data.is_dir = file.is_dir;
    data.file_size_bytes = file.size;
    data.modified_timestamp = static_cast<time_t>(file.modified);
    data.thumbnail_path = default_thumbnail;
    data.print_time_minutes = 0;
    data.filament_grams = 0.0f;
    data.metadata_fetched = false;

    // Format display strings using centralized helpers
    data.size_str = format_file_size(data.file_size_bytes);
    data.modified_str = format_modified_date(data.modified_timestamp);
    data.print_time_str = format_print_time(data.print_time_minutes);
    data.filament_str = format_filament_weight(data.filament_grams);

    // Metadata fields not available until fetch - use consistent placeholder
    data.layer_count_str = "";
    data.print_height_str = "";
    data.original_thumbnail_url = "";

    return data;
}

PrintFileData PrintFileData::from_usb_file(const UsbGcodeFile& file,
                                           const std::string& default_thumbnail) {
    PrintFileData data;
    data.filename = file.filename;
    data.local_path = file.path;
    data.is_dir = false;
    data.file_size_bytes = file.size_bytes;
    data.modified_timestamp = static_cast<time_t>(file.modified_time);
    data.thumbnail_path = default_thumbnail;

    // USB files don't have Moonraker metadata
    data.print_time_minutes = 0;
    data.filament_grams = 0.0f;
    data.metadata_fetched = false; // Could fetch from G-code parsing later

    // Format display strings using centralized helpers
    data.size_str = format_file_size(data.file_size_bytes);
    data.modified_str = format_modified_date(data.modified_timestamp);

    // USB files show "--" for metadata we can't get from Moonraker
    data.print_time_str = "--";
    data.filament_str = "--";
    data.layer_count_str = "--";
    data.print_height_str = "--";
    data.original_thumbnail_url = "";

    return data;
}

PrintFileData PrintFileData::make_directory(const std::string& name, const std::string& icon_path,
                                            bool is_parent) {
    PrintFileData data;
    data.filename = name;
    data.is_dir = true;
    data.thumbnail_path = icon_path;
    data.file_size_bytes = 0;
    data.modified_timestamp = 0;
    data.print_time_minutes = 0;
    data.filament_grams = 0.0f;
    data.metadata_fetched = true; // Directories don't need metadata

    // Directory display strings
    data.size_str = is_parent ? "" : "Folder";
    data.modified_str = "";
    data.print_time_str = "";
    data.filament_str = "";

    return data;
}

namespace helix {

UsbCopyTarget choose_usb_copy_target(const std::string& filename, uint64_t size,
                                     const std::map<std::string, uint64_t>& existing) {
    const size_t dot = filename.rfind('.');
    const std::string stem =
        dot == std::string::npos || dot == 0 ? filename : filename.substr(0, dot);
    const std::string ext = stem.size() == filename.size() ? "" : filename.substr(stem.size());

    // Every taken name either matches or is skipped, so this ends by
    // existing.size() + 1.
    for (size_t n = 1;; ++n) {
        std::string candidate = n == 1 ? filename : stem + " (" + std::to_string(n) + ")" + ext;
        const auto it = existing.find(candidate);
        if (it == existing.end()) {
            return {std::move(candidate), false};
        }
        if (it->second == size) {
            return {std::move(candidate), true};
        }
    }
}

} // namespace helix
