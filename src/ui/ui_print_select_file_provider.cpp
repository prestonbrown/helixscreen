// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_file_provider.h"

#include "ui_filename_utils.h"
#include "ui_panel_print_select.h" // For PrintFileData
#include "ui_print_select_card_view.h"

#include "core/update_queue.h"
#include "helix_fs.h"
#include "i_moonraker_api.h"
#include "print_file_data.h"
#include "three_mf_conversions.h"
#include "thumbnail_cache.h"
#include "try_reserve.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
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
        PrintFileData& kept = existing_data[file.filename];
        kept = file;
#if defined(HELIX_PLATFORM_ESP32)
        // The panel's own entry carries its decoded image across the refresh. A
        // copy held here would keep that image's PSRAM slot taken for as long as
        // the listing request is in flight, past the card leaving the screen.
        kept.esp_thumbnail.reset();
        kept.esp_fetch_cancel.reset();
        kept.esp_thumbnail_tried = false;
        kept.esp_fetch_retried = false;
#endif
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
            // Sized up front: on the firmware a vector that cannot grow aborts
            // the board. A list whose buffer does not fit keeps the one on
            // screen. Each entry's strings still allocate as it is added.
            if (!helix::try_reserve(file_list, files.size() + 1)) {
                spdlog::error("[FileProvider] No memory for a {}-entry file list; keeping the "
                              "current one",
                              files.size());
                if (on_err) {
                    on_err("not enough memory to list " + std::to_string(files.size()) + " files");
                }
                return;
            }

            // Add ".." parent directory entry if not at root
            if (!path_copy.empty()) {
                file_list.push_back(PrintFileData::make_directory("..", self->kFolderUpIcon, true));
            }

            // Convert FileInfo to PrintFileData, preserving existing data where available
            for (const auto& file : files) {
                // Skip hidden directories and files (starting with '.')
                // This includes: .helix_temp, .thumbs, .helix_print, ._macOSmetadata, etc.
                if (!file.filename.empty() && file.filename[0] == '.') {
                    continue;
                }
                // Nor the directories the app stages its copies in where the
                // printer cannot print from a dot-directory.
                if (file.is_dir && path_copy.empty() &&
                    gcode::is_app_managed_directory(file.filename)) {
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
                            if (!helix::fs::exists(fs_path)) {
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
                    spdlog::debug(
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
                        file.filename, PrintSelectCardView::kFolderIcon, false));
                } else {
                    // Only show printable files (.gcode, .gco, .g, .3mf) — the
                    // same predicate the USB stick scanner uses.
                    if (!gcode::has_printable_extension(file.filename)) {
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
            if (!on_ready) {
                return;
            }
            const bool has_archive =
                std::any_of(file_list.begin(), file_list.end(), [](const PrintFileData& f) {
                    return !f.is_dir && gcode::is_3mf(f.filename);
                });
            if (!has_archive || !self->api_) {
                on_ready(std::move(file_list));
                return;
            }
            // A .3mf the printer has already converted is listed beside its
            // G-code; only the G-code needs showing.
            std::vector<three_mf::ListedFile> listed;
            listed.reserve(files.size());
            for (const auto& f : files) {
                if (!f.is_dir) {
                    listed.push_back(
                        {gcode::join_gcode_path(path_copy, f.filename), f.size, f.modified});
                }
            }
            auto shared_list = std::make_shared<std::vector<PrintFileData>>(std::move(file_list));
            self->api_->files().get_archive_conversions(
                [self, shared_list, listed, path_copy, on_ready,
                 gen](const std::vector<three_mf::Conversion>& conversions) {
                    if (gen != self->refresh_generation_.load()) {
                        return;
                    }
                    const auto hidden = three_mf::converted_archives(conversions, listed);
                    auto& list = *shared_list;
                    list.erase(std::remove_if(list.begin(), list.end(),
                                              [&hidden, &path_copy](const PrintFileData& f) {
                                                  return !f.is_dir &&
                                                         hidden.count(gcode::join_gcode_path(
                                                             path_copy, f.filename)) != 0;
                                              }),
                               list.end());
                    if (!hidden.empty()) {
                        spdlog::debug("[FileProvider] Hiding {} converted .3mf archive(s)",
                                      hidden.size());
                    }
                    for (auto& f : list) {
                        if (!f.is_dir && gcode::is_3mf(f.filename)) {
                            f.archive_outputs = three_mf::current_outputs(
                                conversions, listed, gcode::join_gcode_path(path_copy, f.filename));
                        }
                    }
                    on_ready(std::move(list));
                },
                [self, shared_list, on_ready, gen](const MoonrakerError& error) {
                    if (gen != self->refresh_generation_.load()) {
                        return;
                    }
                    // No converter on this printer: every archive shows.
                    spdlog::trace("[FileProvider] No .3mf conversions: {}", error.message);
                    for (auto& f : *shared_list) {
                        f.archive_outputs.clear();
                    }
                    on_ready(std::move(*shared_list));
                });
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
