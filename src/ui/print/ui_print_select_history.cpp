// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_history.h"

#include "core/gcode_filename.h"

namespace helix::ui {

std::string PrintSelectHistoryIntegration::extract_basename(const std::string& path) {
    return helix::gcode::get_filename_basename(path);
}

void PrintSelectHistoryIntegration::merge_history_into_files(
    std::vector<PrintFileData>& files,
    const std::unordered_map<std::string, PrintHistoryStats>& stats_by_filename,
    const std::string& current_print_filename) {
    // Extract basename from current print filename
    std::string current_print_basename = extract_basename(current_print_filename);

    for (auto& file : files) {
        // Skip directories
        if (file.is_dir) {
            continue;
        }

        std::string basename = extract_basename(file.filename);

        // Check if currently printing this file
        if (!current_print_basename.empty() && basename == current_print_basename) {
            file.history_status = FileHistoryStatus::CURRENTLY_PRINTING;
            continue;
        }

        // Look up in history stats
        auto it = stats_by_filename.find(basename);
        if (it == stats_by_filename.end()) {
            file.history_status = FileHistoryStatus::NEVER_PRINTED;
            file.success_count = 0;
            continue;
        }

        const auto& stats = it->second;

        // Validate match with UUID or size if available
        bool match_confirmed = false;
        if (!file.uuid.empty() && !stats.uuid.empty()) {
            match_confirmed = (file.uuid == stats.uuid);
        } else if (file.file_size_bytes > 0 && stats.size_bytes > 0) {
            match_confirmed = (file.file_size_bytes == stats.size_bytes);
        } else {
            // No UUID or size to validate - accept basename match
            match_confirmed = true;
        }

        if (!match_confirmed) {
            file.history_status = FileHistoryStatus::NEVER_PRINTED;
            file.success_count = 0;
            continue;
        }

        // Set status based on last print result
        file.success_count = stats.success_count;
        switch (stats.last_status) {
        case PrintJobStatus::COMPLETED:
            file.history_status = FileHistoryStatus::COMPLETED;
            break;
        case PrintJobStatus::CANCELLED:
            file.history_status = FileHistoryStatus::CANCELLED;
            break;
        case PrintJobStatus::ERROR:
            file.history_status = FileHistoryStatus::FAILED;
            break;
        case PrintJobStatus::IN_PROGRESS:
            file.history_status = FileHistoryStatus::CURRENTLY_PRINTING;
            break;
        default:
            file.history_status = FileHistoryStatus::NEVER_PRINTED;
            break;
        }
    }
}

} // namespace helix::ui
