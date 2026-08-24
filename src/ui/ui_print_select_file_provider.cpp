// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_file_provider.h"

#include "ui_panel_print_select.h" // For PrintFileData
#include "ui_print_select_card_view.h"
#include "ui_update_queue.h"

#include "i_moonraker_api.h"
#include "print_file_data.h"
#include "thumbnail_cache.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <unordered_map>

namespace helix::ui {

// ============================================================================
// File Operations
// ============================================================================

bool PrintSelectFileProvider::is_ready() const {
    if (!api_) {
        return false;
    }

    ConnectionState state = api_->get_connection_state();
    return state == ConnectionState::CONNECTED;
}

void PrintSelectFileProvider::refresh_files(const std::string& current_path,
                                            const std::vector<PrintFileData>& existing_files) {
    if (!api_) {
        spdlog::warn("[FileProvider] Cannot refresh files: IMoonrakerAPI not initialized");
        return;
    }

    // Check if WebSocket is actually connected
    if (!is_ready()) {
        spdlog::debug("[FileProvider] Cannot refresh files: not connected");
        return;
    }

    current_path_ = current_path;

    // Per-request generation: a second refresh_files() (e.g. a stuck-refresh
    // self-heal reissue) bumps this, so a superseded response self-invalidates
    // in its callback below rather than firing on_files_ready a second time.
    const uint32_t gen = ++refresh_generation_;

    spdlog::trace("[FileProvider] Refreshing file list from Moonraker (path: '{}', gen={})...",
                  current_path.empty() ? "/" : current_path, gen);

    // Build map of existing file data to preserve thumbnails/metadata
    std::unordered_map<std::string, PrintFileData> existing_data;
    for (const auto& file : existing_files) {
        existing_data[file.filename] = file;
    }

    auto* self = this;
    auto on_ready = on_files_ready_;
    auto on_err = on_error_;
    auto path_copy = current_path;

    // Request directory contents (includes both files AND directories)
    api_->files().get_directory(
        "gcodes", current_path,
        // Success callback
        [self, existing_data = std::move(existing_data), path_copy, on_ready, on_err,
         gen](const std::vector<FileInfo>& files) {
            if (gen != self->refresh_generation_.load()) {
                spdlog::debug(
                    "[FileProvider] discarding stale get_directory response (gen {} != {})", gen,
                    self->refresh_generation_.load());
                return;
            }
            spdlog::trace("[FileProvider] Moonraker returned {} raw items for path='{}'",
                          files.size(), path_copy.empty() ? "/" : path_copy);
            for (size_t i = 0; i < files.size() && i < 30; ++i) {
                spdlog::trace("[FileProvider]   raw[{}]: '{}' (dir={}, modified={:.0f})", i,
                              files[i].filename, files[i].is_dir, files[i].modified);
            }
            if (files.size() > 30) {
                spdlog::trace("[FileProvider]   ... and {} more raw items", files.size() - 30);
            }

            std::vector<PrintFileData> file_list;

            // Add ".." parent directory entry if not at root
            if (!path_copy.empty()) {
                file_list.push_back(
                    PrintFileData::make_directory("..", self->FOLDER_UP_ICON, true));
            }

            // Convert FileInfo to PrintFileData, preserving existing data where available
            for (const auto& file : files) {
                // Skip hidden directories and files (starting with '.')
                // This includes: .helix_temp, .thumbs, .helix_print, ._macOSmetadata, etc.
                if (!file.filename.empty() && file.filename[0] == '.') {
                    continue;
                }

                // Check if we have existing data for this file
                auto it = existing_data.find(file.filename);
                if (it != existing_data.end()) {
                    // Check if file was modified (e.g., re-uploaded with same name)
                    time_t new_modified = static_cast<time_t>(file.modified);
                    if (it->second.modified_timestamp == new_modified) {
                        // Same file - preserve existing data (thumbnail, metadata, fetched state)
                        // BUT: validate thumbnail still exists (may have been invalidated)
                        PrintFileData preserved = it->second;
                        if (!preserved.thumbnail_path.empty() &&
                            preserved.thumbnail_path.rfind("A:", 0) == 0 &&
                            preserved.thumbnail_path !=
                                PrintSelectCardView::get_default_thumbnail()) {
                            // Convert LVGL path to filesystem path and check existence
                            std::string fs_path = preserved.thumbnail_path.substr(2); // Strip "A:"
                            if (!std::filesystem::exists(fs_path)) {
                                spdlog::debug(
                                    "[FileProvider] Cached thumbnail missing, will re-fetch: {}",
                                    preserved.thumbnail_path);
                                preserved.thumbnail_path =
                                    PrintSelectCardView::get_default_thumbnail();
                                preserved.metadata_fetched = false; // Re-fetch to get thumbnail
                            }
                        }
                        file_list.push_back(preserved);
                        continue;
                    }
                    // File was modified - invalidate cached thumbnails and refetch
                    spdlog::info(
                        "[FileProvider] File modified, invalidating cache: {} (old: {}, new: {})",
                        file.filename, it->second.modified_timestamp, new_modified);
                    if (!it->second.original_thumbnail_url.empty()) {
                        get_thumbnail_cache().invalidate(it->second.original_thumbnail_url);
                    }
                    // Fall through to create fresh PrintFileData
                }

                // New file or modified file - create with factory methods
                if (file.is_dir) {
                    // Directory - use folder icon
                    file_list.push_back(PrintFileData::make_directory(
                        file.filename, PrintSelectCardView::FOLDER_ICON, false));
                } else {
                    // Only show printable files (.gcode, .gco, .g, .3mf)
                    auto has_ext = [](const std::string& name, const char* ext) {
                        size_t elen = strlen(ext);
                        if (name.size() <= elen)
                            return false;
                        std::string suffix = name.substr(name.size() - elen);
                        for (auto& c : suffix)
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        return suffix == ext;
                    };
                    if (!has_ext(file.filename, ".gcode") && !has_ext(file.filename, ".gco") &&
                        !has_ext(file.filename, ".g") && !has_ext(file.filename, ".3mf")) {
                        continue;
                    }

                    file_list.push_back(PrintFileData::from_moonraker_file(
                        file, PrintSelectCardView::get_default_thumbnail()));
                }
            }

            // Count files vs directories for logging
            size_t dir_count = 0, file_count = 0;
            for (const auto& item : file_list) {
                if (item.is_dir)
                    dir_count++;
                else
                    file_count++;
            }
            spdlog::trace("[FileProvider] File list updated: {} directories, {} printable files",
                          dir_count, file_count);

            // Deliver results via callback (metadata_fetched is now in each file struct)
            if (on_ready) {
                on_ready(std::move(file_list));
            }
        },
        // Error callback
        [self, on_err, gen](const MoonrakerError& error) {
            if (gen != self->refresh_generation_.load()) {
                spdlog::debug("[FileProvider] discarding stale get_directory error (gen {} != {})",
                              gen, self->refresh_generation_.load());
                return;
            }
            spdlog::error("[FileProvider] File list refresh error: {} ({})", error.message,
                          error.get_type_string());
            if (on_err) {
                on_err(error.message);
            }
        });
}

} // namespace helix::ui
