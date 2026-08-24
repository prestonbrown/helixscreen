// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "klipper_config_editor.h"

#include "http_executor.h"
#include "i_moonraker_api.h"
#include "klipper_config_includes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <sstream>
#include <thread>

namespace helix::system {

std::optional<ConfigKey> ConfigStructure::find_key(const std::string& section,
                                                   const std::string& key) const {
    auto it = sections.find(section);
    if (it == sections.end())
        return std::nullopt;

    for (const auto& k : it->second.keys) {
        if (k.name == key)
            return k;
    }
    return std::nullopt;
}

ConfigStructure KlipperConfigEditor::parse_structure(const std::string& content) const {
    ConfigStructure result;

    if (content.empty()) {
        result.total_lines = 0;
        return result;
    }

    // Split content into lines
    std::vector<std::string> lines;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    result.total_lines = static_cast<int>(lines.size());

    std::string current_section;
    ConfigKey* current_multiline_key = nullptr;

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const auto& raw_line = lines[i];

        // Check for SAVE_CONFIG boundary
        if (raw_line.find("#*# <") != std::string::npos &&
            raw_line.find("SAVE_CONFIG") != std::string::npos) {
            result.save_config_line = i;
            // Stop parsing structured content after SAVE_CONFIG
            break;
        }

        // Check if this is a continuation of a multi-line value
        if (current_multiline_key != nullptr) {
            // Empty lines are preserved within multi-line values
            if (raw_line.empty()) {
                current_multiline_key->end_line = i;
                continue;
            }
            // Indented lines (space or tab) continue the multi-line value
            if (raw_line[0] == ' ' || raw_line[0] == '\t') {
                current_multiline_key->end_line = i;
                continue;
            }
            // Non-indented, non-empty line ends the multi-line value
            current_multiline_key = nullptr;
        }

        // Skip empty lines outside multi-line values
        if (raw_line.empty())
            continue;

        // Check for section header: [section_name]
        if (raw_line[0] == '[') {
            auto close_bracket = raw_line.find(']');
            if (close_bracket != std::string::npos) {
                // Finalize previous section's line_end
                if (!current_section.empty()) {
                    result.sections[current_section].line_end = i - 1;
                }

                std::string section_name = raw_line.substr(1, close_bracket - 1);

                // Check for include directive
                const std::string include_prefix = "include ";
                if (section_name.substr(0, include_prefix.size()) == include_prefix) {
                    std::string path = section_name.substr(include_prefix.size());
                    result.includes.push_back(path);
                    current_section.clear();
                    continue;
                }

                current_section = section_name;
                auto& sec = result.sections[current_section];
                sec.name = current_section;
                sec.line_start = i;
                continue;
            }
        }

        // Skip full-line comments
        if (raw_line[0] == '#' || raw_line[0] == ';')
            continue;

        // If we're not in a section, skip
        if (current_section.empty())
            continue;

        // Parse key-value pair: find first ':' or '='
        std::string delimiter;
        size_t delim_pos = std::string::npos;

        // Find the first delimiter (: or =)
        size_t colon_pos = raw_line.find(':');
        size_t equals_pos = raw_line.find('=');

        if (colon_pos != std::string::npos && equals_pos != std::string::npos) {
            delim_pos = std::min(colon_pos, equals_pos);
        } else if (colon_pos != std::string::npos) {
            delim_pos = colon_pos;
        } else if (equals_pos != std::string::npos) {
            delim_pos = equals_pos;
        }

        if (delim_pos == std::string::npos)
            continue;

        delimiter = std::string(1, raw_line[delim_pos]);

        // Extract key name and lowercase it
        std::string key_name = raw_line.substr(0, delim_pos);
        // Trim trailing whitespace from key
        while (!key_name.empty() && (key_name.back() == ' ' || key_name.back() == '\t')) {
            key_name.pop_back();
        }
        std::transform(key_name.begin(), key_name.end(), key_name.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // Extract value (after delimiter, trimming leading whitespace)
        std::string value;
        if (delim_pos + 1 < raw_line.size()) {
            value = raw_line.substr(delim_pos + 1);
            // Trim leading whitespace from value
            size_t first_non_space = value.find_first_not_of(" \t");
            if (first_non_space != std::string::npos) {
                value = value.substr(first_non_space);
            } else {
                value.clear();
            }
        }

        ConfigKey key;
        key.name = key_name;
        key.value = value;
        key.delimiter = delimiter;
        key.line_number = i;
        key.end_line = i;

        // Check if this is the start of a multi-line value
        // (empty value or value that will have indented continuation lines)
        if (value.empty()) {
            key.is_multiline = true;
        }

        result.sections[current_section].keys.push_back(key);

        // Track pointer for multi-line continuation detection
        // Even non-empty values can have continuations
        current_multiline_key = &result.sections[current_section].keys.back();
    }

    // Finalize the last section's line_end
    if (!current_section.empty()) {
        int last_line =
            result.save_config_line >= 0 ? result.save_config_line - 1 : result.total_lines - 1;
        result.sections[current_section].line_end = last_line;
    }

    return result;
}

namespace {

// Split content into lines, preserving the ability to rejoin with \n
std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

// Rejoin lines with \n, adding trailing newline if original had one
std::string join_lines(const std::vector<std::string>& lines, bool trailing_newline) {
    std::string result;
    for (size_t i = 0; i < lines.size(); ++i) {
        result += lines[i];
        if (i + 1 < lines.size() || trailing_newline) {
            result += '\n';
        }
    }
    return result;
}

} // namespace

std::optional<std::string> KlipperConfigEditor::set_value(const std::string& content,
                                                          const std::string& section,
                                                          const std::string& key,
                                                          const std::string& new_value) const {
    auto structure = parse_structure(content);
    auto found = structure.find_key(section, key);
    if (!found.has_value())
        return std::nullopt;

    auto lines = split_lines(content);
    int target = found->line_number;
    if (target < 0 || target >= static_cast<int>(lines.size()))
        return std::nullopt;

    const auto& raw_line = lines[target];

    // Find the delimiter position in the raw line (first : or =)
    size_t delim_pos = std::string::npos;
    size_t colon_pos = raw_line.find(':');
    size_t equals_pos = raw_line.find('=');
    if (colon_pos != std::string::npos && equals_pos != std::string::npos)
        delim_pos = std::min(colon_pos, equals_pos);
    else if (colon_pos != std::string::npos)
        delim_pos = colon_pos;
    else if (equals_pos != std::string::npos)
        delim_pos = equals_pos;

    if (delim_pos == std::string::npos)
        return std::nullopt;

    // Preserve everything up to and including the delimiter plus any whitespace after it
    size_t value_start = delim_pos + 1;
    // Preserve the spacing between delimiter and value
    while (value_start < raw_line.size() &&
           (raw_line[value_start] == ' ' || raw_line[value_start] == '\t')) {
        ++value_start;
    }

    // Reconstruct: key + delimiter + spacing + new_value
    std::string prefix = raw_line.substr(0, delim_pos + 1);
    // Restore the original spacing between delimiter and old value
    std::string spacing = raw_line.substr(delim_pos + 1, value_start - (delim_pos + 1));
    lines[target] = prefix + spacing + new_value;

    bool trailing = !content.empty() && content.back() == '\n';
    return join_lines(lines, trailing);
}

std::optional<std::string> KlipperConfigEditor::add_key(const std::string& content,
                                                        const std::string& section,
                                                        const std::string& key,
                                                        const std::string& value,
                                                        const std::string& delimiter) const {
    auto structure = parse_structure(content);
    auto sec_it = structure.sections.find(section);
    if (sec_it == structure.sections.end())
        return std::nullopt;

    auto lines = split_lines(content);
    const auto& sec = sec_it->second;

    // Find insert position: after the last key line, or after section header if no keys
    int insert_after = sec.line_start;
    if (!sec.keys.empty()) {
        // Use the end_line of the last key (handles multi-line values)
        for (const auto& k : sec.keys) {
            if (k.end_line > insert_after)
                insert_after = k.end_line;
        }
    }

    // Insert the new line after insert_after
    std::string new_line = key + delimiter + value;
    lines.insert(lines.begin() + insert_after + 1, new_line);

    bool trailing = !content.empty() && content.back() == '\n';
    return join_lines(lines, trailing);
}

std::optional<std::string> KlipperConfigEditor::remove_key(const std::string& content,
                                                           const std::string& section,
                                                           const std::string& key) const {
    auto structure = parse_structure(content);
    auto found = structure.find_key(section, key);
    if (!found.has_value())
        return std::nullopt;

    auto lines = split_lines(content);
    int start = found->line_number;
    int end = found->end_line;

    // Comment out the key line and any continuation lines
    for (int i = start; i <= end && i < static_cast<int>(lines.size()); ++i) {
        lines[i] = "#" + lines[i];
    }

    bool trailing = !content.empty() && content.back() == '\n';
    return join_lines(lines, trailing);
}

// Path/glob utilities are now in klipper_config_includes.h

std::map<std::string, SectionLocation>
KlipperConfigEditor::resolve_includes(const std::map<std::string, std::string>& files,
                                      const std::string& root_file, int max_depth) const {
    std::map<std::string, SectionLocation> result;
    std::set<std::string> visited;

    // Recursive lambda: process a file and its includes
    // depth starts at 0 for the root file
    std::function<void(const std::string&, int)> process_file;
    process_file = [&](const std::string& file_path, int depth) {
        // Cycle detection
        if (visited.count(file_path))
            return;
        visited.insert(file_path);

        // Depth check — root is depth 0, max_depth=5 allows depths 0..5 (6 levels total)
        if (depth > max_depth) {
            spdlog::debug("klipper_config_editor: max include depth {} reached at {}", max_depth,
                          file_path);
            return;
        }

        // Find file content
        auto it = files.find(file_path);
        if (it == files.end()) {
            spdlog::debug("klipper_config_editor: included file not found: {}", file_path);
            return;
        }

        auto structure = parse_structure(it->second);

        // Process includes first (so the current file's sections override included ones)
        for (const auto& include_pattern : structure.includes) {
            bool has_wildcard = include_pattern.find('*') != std::string::npos ||
                                include_pattern.find('?') != std::string::npos;

            if (has_wildcard) {
                auto matched = config_match_glob(files, file_path, include_pattern);
                for (const auto& match : matched) {
                    process_file(match, depth + 1);
                }
            } else {
                std::string resolved = config_resolve_path(file_path, include_pattern);
                process_file(resolved, depth + 1);
            }
        }

        // Add this file's sections (overwrites any from includes — last wins)
        for (const auto& [name, section] : structure.sections) {
            SectionLocation loc;
            loc.file_path = file_path;
            loc.section = section;
            result[name] = loc;
        }
    };

    process_file(root_file, 0);
    return result;
}

// ============================================================================
// Moonraker Integration — Async file operations
// ============================================================================

std::map<std::string, SectionLocation> KlipperConfigEditor::get_section_map() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return section_map_;
}

std::optional<std::string> KlipperConfigEditor::get_cached_file(const std::string& path) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = file_cache_.find(path);
    if (it == file_cache_.end())
        return std::nullopt;
    return it->second;
}

void KlipperConfigEditor::download_with_includes(IMoonrakerAPI& api, const std::string& file_path,
                                                 std::shared_ptr<std::atomic<int>> pending,
                                                 std::function<void()> on_all_done,
                                                 ErrorCallback on_error) {
    // Check if already cached (avoid duplicate downloads)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (file_cache_.count(file_path)) {
            int remaining = pending->fetch_sub(1) - 1;
            if (remaining == 0 && on_all_done)
                on_all_done();
            return;
        }
    }

    spdlog::debug("[ConfigEditor] Downloading config file: {}", file_path);

    api.transfers().download_file(
        "config", file_path,
        [this, &api, file_path, pending, on_all_done, on_error](const std::string& content) {
            // Cache the file content
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                file_cache_[file_path] = content;
            }

            // Parse to find includes
            auto structure = parse_structure(content);

            if (!structure.includes.empty()) {
                // Collect non-glob includes to download
                for (const auto& include : structure.includes) {
                    // Skip glob patterns — they require listing files from Moonraker
                    // which is handled separately in load_config_files
                    if (include.find('*') != std::string::npos)
                        continue;

                    std::string resolved = config_resolve_path(file_path, include);

                    // Check if already cached
                    {
                        std::lock_guard<std::mutex> lock(cache_mutex_);
                        if (file_cache_.count(resolved))
                            continue;
                    }

                    // Increment pending count and download recursively
                    pending->fetch_add(1);
                    download_with_includes(api, resolved, pending, on_all_done, on_error);
                }
            }

            // Decrement pending count for this file
            int remaining = pending->fetch_sub(1) - 1;
            if (remaining == 0 && on_all_done)
                on_all_done();
        },
        [file_path, pending, on_all_done, on_error](const MoonrakerError& err) {
            spdlog::warn("[ConfigEditor] Failed to download {}: {}", file_path, err.message);
            // Non-fatal: included files may be optional. Decrement and continue.
            int remaining = pending->fetch_sub(1) - 1;
            if (remaining == 0 && on_all_done)
                on_all_done();
        });
}

void KlipperConfigEditor::load_config_files(IMoonrakerAPI& api, SectionMapCallback on_complete,
                                            ErrorCallback on_error) {
    spdlog::info("[ConfigEditor] Loading config files from printer");

    // First, list all config files to support glob includes
    api.files().list_files(
        "config", "", true,
        [this, &api, on_complete, on_error](const std::vector<FileInfo>& files) {
            // Build a set of available config file paths for glob resolution
            std::set<std::string> available_files;
            for (const auto& f : files) {
                // Use path if available, otherwise filename
                std::string path = f.path.empty() ? f.filename : f.path;
                available_files.insert(path);
                spdlog::trace("[ConfigEditor] Found config file: {}", path);
            }

            // Clear caches for fresh load
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                file_cache_.clear();
                section_map_.clear();
            }

            // Start downloading from printer.cfg
            auto pending = std::make_shared<std::atomic<int>>(1);

            auto on_all_done = [this, available_files, on_complete]() {
                spdlog::debug("[ConfigEditor] All config files downloaded, resolving includes");

                std::map<std::string, std::string> files_copy;
                {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    files_copy = file_cache_;
                }

                // For glob includes, we need to add files that match glob patterns.
                // The resolve_includes method handles glob matching against the file map.
                // We may need to add available files that were not yet downloaded.
                // However, resolve_includes only uses files present in the map,
                // so glob patterns will only match already-downloaded files.
                // This is acceptable since non-glob includes are the common case.

                auto section_map = resolve_includes(files_copy, "printer.cfg");

                {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    section_map_ = section_map;
                }

                spdlog::info("[ConfigEditor] Resolved {} sections across {} files",
                             section_map.size(), files_copy.size());

                if (on_complete)
                    on_complete(section_map);
            };

            download_with_includes(api, "printer.cfg", pending, on_all_done, on_error);
        },
        [on_error](const MoonrakerError& err) {
            spdlog::error("[ConfigEditor] Failed to list config files: {}", err.message);
            if (on_error)
                on_error("Failed to list config files: " + err.message);
        });
}

void KlipperConfigEditor::backup_file(IMoonrakerAPI& api, const std::string& file_path,
                                      SuccessCallback on_success, ErrorCallback on_error) {
    std::string source = "config/" + file_path;
    std::string dest = "config/" + file_path + ".helix_backup";

    spdlog::info("[ConfigEditor] Creating backup: {} -> {}", source, dest);

    api.files().copy_file(
        source, dest,
        [file_path, on_success]() {
            spdlog::debug("[ConfigEditor] Backup created for {}", file_path);
            if (on_success)
                on_success();
        },
        [file_path, on_error](const MoonrakerError& err) {
            spdlog::error("[ConfigEditor] Failed to backup {}: {}", file_path, err.message);
            if (on_error)
                on_error("Failed to backup " + file_path + ": " + err.message);
        });
}

void KlipperConfigEditor::edit_value(IMoonrakerAPI& api, const std::string& section,
                                     const std::string& key, const std::string& new_value,
                                     SuccessCallback on_success, ErrorCallback on_error) {
    // Look up section in cached section map
    std::string file_path;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = section_map_.find(section);
        if (it == section_map_.end()) {
            spdlog::error("[ConfigEditor] Section [{}] not found in section map", section);
            if (on_error)
                on_error("Section [" + section + "] not found");
            return;
        }
        file_path = it->second.file_path;
    }

    spdlog::info("[ConfigEditor] Editing [{}] {}: {} in {}", section, key, new_value, file_path);

    // Step 1: Create backup of the file
    backup_file(
        api, file_path,
        [this, &api, file_path, section, key, new_value, on_success, on_error]() {
            // Step 2: Get content (from cache or re-download)
            std::optional<std::string> cached_content;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                auto it = file_cache_.find(file_path);
                if (it != file_cache_.end())
                    cached_content = it->second;
            }

            auto do_edit = [this, &api, file_path, section, key, new_value, on_success,
                            on_error](const std::string& content) {
                // Step 3: Apply the edit
                auto modified = set_value(content, section, key, new_value);
                if (!modified.has_value()) {
                    spdlog::error("[ConfigEditor] set_value failed for [{}] {} in {}", section, key,
                                  file_path);
                    if (on_error)
                        on_error("Failed to set [" + section + "] " + key + " in " + file_path);
                    return;
                }

                // Step 4: Upload modified content
                api.transfers().upload_file(
                    "config", file_path, *modified,
                    [this, file_path, modified, on_success]() {
                        // Step 5: Update cache with new content
                        {
                            std::lock_guard<std::mutex> lock(cache_mutex_);
                            file_cache_[file_path] = *modified;
                        }
                        spdlog::info("[ConfigEditor] Successfully edited {}", file_path);
                        if (on_success)
                            on_success();
                    },
                    [file_path, on_error](const MoonrakerError& err) {
                        spdlog::error("[ConfigEditor] Failed to upload modified {}: {}", file_path,
                                      err.message);
                        if (on_error)
                            on_error("Failed to upload " + file_path + ": " + err.message);
                    });
            };

            if (cached_content.has_value()) {
                do_edit(*cached_content);
            } else {
                // Re-download if not cached
                api.transfers().download_file(
                    "config", file_path,
                    [do_edit](const std::string& content) { do_edit(content); },
                    [file_path, on_error](const MoonrakerError& err) {
                        spdlog::error("[ConfigEditor] Failed to download {}: {}", file_path,
                                      err.message);
                        if (on_error)
                            on_error("Failed to download " + file_path + ": " + err.message);
                    });
            }
        },
        on_error);
}

void KlipperConfigEditor::restore_backups(IMoonrakerAPI& api, SuccessCallback on_complete,
                                          ErrorCallback on_error) {
    spdlog::info("[ConfigEditor] Restoring backup files");

    api.files().list_files(
        "config", "", true,
        [&api, on_complete, on_error](const std::vector<FileInfo>& files) {
            // Find all .helix_backup files
            std::vector<std::string> backup_files;
            for (const auto& f : files) {
                std::string path = f.path.empty() ? f.filename : f.path;
                if (path.size() > 13 && path.substr(path.size() - 13) == ".helix_backup") {
                    backup_files.push_back(path);
                }
            }

            if (backup_files.empty()) {
                spdlog::debug("[ConfigEditor] No backup files to restore");
                if (on_complete)
                    on_complete();
                return;
            }

            auto pending =
                std::make_shared<std::atomic<int>>(static_cast<int>(backup_files.size()));
            auto had_error = std::make_shared<std::atomic<bool>>(false);

            for (const auto& backup_path : backup_files) {
                // Remove .helix_backup suffix to get original path
                std::string original = backup_path.substr(0, backup_path.size() - 13);
                std::string source = "config/" + backup_path;
                std::string dest = "config/" + original;

                spdlog::info("[ConfigEditor] Restoring {} -> {}", source, dest);

                api.files().copy_file(
                    source, dest,
                    [pending, on_complete, backup_path]() {
                        spdlog::debug("[ConfigEditor] Restored {}", backup_path);
                        int remaining = pending->fetch_sub(1) - 1;
                        if (remaining == 0 && on_complete)
                            on_complete();
                    },
                    [pending, had_error, on_complete, on_error,
                     backup_path](const MoonrakerError& err) {
                        spdlog::error("[ConfigEditor] Failed to restore {}: {}", backup_path,
                                      err.message);
                        had_error->store(true);
                        int remaining = pending->fetch_sub(1) - 1;
                        if (remaining == 0) {
                            if (on_error)
                                on_error("Failed to restore one or more backup files");
                        }
                    });
            }
        },
        [on_error](const MoonrakerError& err) {
            spdlog::error("[ConfigEditor] Failed to list files for restore: {}", err.message);
            if (on_error)
                on_error("Failed to list config files: " + err.message);
        });
}

void KlipperConfigEditor::cleanup_backups(IMoonrakerAPI& api, SuccessCallback on_complete) {
    spdlog::debug("[ConfigEditor] Cleaning up backup files");

    api.files().list_files(
        "config", "", true,
        [&api, on_complete](const std::vector<FileInfo>& files) {
            // Find all .helix_backup files
            std::vector<std::string> backup_files;
            for (const auto& f : files) {
                std::string path = f.path.empty() ? f.filename : f.path;
                if (path.size() > 13 && path.substr(path.size() - 13) == ".helix_backup") {
                    backup_files.push_back(path);
                }
            }

            if (backup_files.empty()) {
                spdlog::debug("[ConfigEditor] No backup files to clean up");
                if (on_complete)
                    on_complete();
                return;
            }

            auto pending =
                std::make_shared<std::atomic<int>>(static_cast<int>(backup_files.size()));

            for (const auto& backup_path : backup_files) {
                std::string full_path = "config/" + backup_path;

                api.files().delete_file(
                    full_path,
                    [pending, on_complete, backup_path]() {
                        spdlog::debug("[ConfigEditor] Deleted backup {}", backup_path);
                        int remaining = pending->fetch_sub(1) - 1;
                        if (remaining == 0 && on_complete)
                            on_complete();
                    },
                    [pending, on_complete, backup_path](const MoonrakerError& err) {
                        // Non-fatal: log and continue
                        spdlog::warn("[ConfigEditor] Failed to delete backup {}: {}", backup_path,
                                     err.message);
                        int remaining = pending->fetch_sub(1) - 1;
                        if (remaining == 0 && on_complete)
                            on_complete();
                    });
            }
        },
        [on_complete](const MoonrakerError&) {
            // Non-fatal: cleanup is best-effort
            spdlog::warn("[ConfigEditor] Failed to list files for cleanup");
            if (on_complete)
                on_complete();
        });
}

std::optional<std::string>
KlipperConfigEditor::apply_edits(const std::string& content, const std::string& section,
                                 const std::vector<ConfigEdit>& edits) const {
    // Verify section exists before applying any edits
    auto structure = parse_structure(content);
    if (structure.sections.find(section) == structure.sections.end()) {
        spdlog::debug("[ConfigEditor] apply_edits: section [{}] not found", section);
        return std::nullopt;
    }

    // Empty edits = no-op success
    if (edits.empty()) {
        return content;
    }

    std::string current = content;

    for (const auto& edit : edits) {
        std::optional<std::string> result;

        switch (edit.type) {
        case ConfigEdit::Type::SET_VALUE:
            result = set_value(current, section, edit.key, edit.value);
            if (!result.has_value()) {
                spdlog::error("[ConfigEditor] apply_edits: SET_VALUE failed for [{}] {}", section,
                              edit.key);
                return std::nullopt;
            }
            current = *result;
            break;

        case ConfigEdit::Type::ADD_KEY: {
            // If key already exists, use set_value instead to avoid duplicates
            auto check = parse_structure(current);
            auto existing = check.find_key(section, edit.key);
            if (existing.has_value()) {
                result = set_value(current, section, edit.key, edit.value);
            } else {
                result = add_key(current, section, edit.key, edit.value);
            }
            if (!result.has_value()) {
                spdlog::error("[ConfigEditor] apply_edits: ADD_KEY failed for [{}] {}", section,
                              edit.key);
                return std::nullopt;
            }
            current = *result;
            break;
        }

        case ConfigEdit::Type::REMOVE_KEY:
            result = remove_key(current, section, edit.key);
            if (!result.has_value()) {
                spdlog::debug("[ConfigEditor] apply_edits: REMOVE_KEY skipped (key [{}] {} not "
                              "found)",
                              section, edit.key);
                // Not finding a key to remove is not fatal
            } else {
                current = *result;
            }
            break;
        }
    }

    return current;
}

void KlipperConfigEditor::safe_multi_edit(IMoonrakerAPI& api, const std::string& section,
                                          const std::vector<ConfigEdit>& edits,
                                          SuccessCallback on_success, ErrorCallback on_error,
                                          int restart_timeout_ms) {
    spdlog::info("[ConfigEditor] Starting safe multi-edit on [{}] ({} edits)", section,
                 edits.size());

    // Step 1: Load config files to populate cache and section map
    load_config_files(
        api,
        [this, &api, section, edits, on_success, on_error,
         restart_timeout_ms](std::map<std::string, SectionLocation> section_map) {
            // Step 2: Find which file contains the section
            auto sec_it = section_map.find(section);
            if (sec_it == section_map.end()) {
                spdlog::error("[ConfigEditor] Section [{}] not found in config", section);
                if (on_error)
                    on_error("Section [" + section + "] not found in config");
                return;
            }

            std::string file_path = sec_it->second.file_path;
            spdlog::debug("[ConfigEditor] Section [{}] found in {}", section, file_path);

            // Step 3: Get the file content from cache
            auto cached = get_cached_file(file_path);
            if (!cached.has_value()) {
                spdlog::error("[ConfigEditor] File {} not in cache", file_path);
                if (on_error)
                    on_error("File " + file_path + " not in cache");
                return;
            }

            // Step 4: Apply all edits to the content
            auto modified = apply_edits(*cached, section, edits);
            if (!modified.has_value()) {
                spdlog::error("[ConfigEditor] apply_edits failed for [{}]", section);
                if (on_error)
                    on_error("Failed to apply edits to [" + section + "]");
                return;
            }

            // Step 5: Backup the file, then upload, then restart
            backup_file(
                api, file_path,
                [this, &api, file_path, modified, on_success, on_error, restart_timeout_ms]() {
                    // Step 6: Upload modified content
                    api.transfers().upload_file(
                        "config", file_path, *modified,
                        [this, &api, file_path, modified, on_success, on_error,
                         restart_timeout_ms]() {
                            // Update cache
                            {
                                std::lock_guard<std::mutex> lock(cache_mutex_);
                                file_cache_[file_path] = *modified;
                            }

                            // Step 7: Send FIRMWARE_RESTART
                            spdlog::info("[ConfigEditor] Multi-edit written, sending "
                                         "FIRMWARE_RESTART");
                            api.restart_firmware(
                                [this, &api, on_success, on_error, restart_timeout_ms]() {
                                    // Step 8: Monitor reconnection (same pattern as
                                    // safe_edit_value). Route through HttpExecutor::fast()
                                    // instead of raw std::thread — spawn failures crash
                                    // std::terminate on AD5M (#724, #837).
                                    helix::http::HttpExecutor::fast().submit(
                                        [this, &api, on_success, on_error, restart_timeout_ms]() {
                                            const auto poll_interval =
                                                std::chrono::milliseconds(500);
                                            const auto timeout =
                                                std::chrono::milliseconds(restart_timeout_ms);
                                            const auto start = std::chrono::steady_clock::now();

                                            // Phase 1: Wait for disconnect
                                            bool saw_disconnect = false;
                                            while (std::chrono::steady_clock::now() - start <
                                                   timeout) {
                                                if (!api.is_connected()) {
                                                    saw_disconnect = true;
                                                    spdlog::debug("[ConfigEditor] Klipper "
                                                                  "disconnected after "
                                                                  "FIRMWARE_RESTART");
                                                    break;
                                                }
                                                std::this_thread::sleep_for(poll_interval);
                                            }

                                            if (!saw_disconnect) {
                                                spdlog::info(
                                                    "[ConfigEditor] Klipper stayed connected "
                                                    "after FIRMWARE_RESTART (fast restart)");
                                                cleanup_backups(api, [on_success]() {
                                                    spdlog::info("[ConfigEditor] Safe multi-edit "
                                                                 "complete (fast restart)");
                                                    if (on_success)
                                                        on_success();
                                                });
                                                return;
                                            }

                                            // Phase 2: Wait for reconnect
                                            while (std::chrono::steady_clock::now() - start <
                                                   timeout) {
                                                if (api.is_connected()) {
                                                    auto elapsed = std::chrono::duration_cast<
                                                        std::chrono::milliseconds>(
                                                        std::chrono::steady_clock::now() - start);
                                                    spdlog::info("[ConfigEditor] Klipper "
                                                                 "reconnected after {}ms",
                                                                 elapsed.count());
                                                    cleanup_backups(api, [on_success]() {
                                                        spdlog::info(
                                                            "[ConfigEditor] Safe multi-edit "
                                                            "complete, backups cleaned up");
                                                        if (on_success)
                                                            on_success();
                                                    });
                                                    return;
                                                }
                                                std::this_thread::sleep_for(poll_interval);
                                            }

                                            // Timeout: revert
                                            auto elapsed = std::chrono::duration_cast<
                                                std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - start);
                                            spdlog::error(
                                                "[ConfigEditor] Klipper failed to reconnect "
                                                "within {}ms, reverting config",
                                                elapsed.count());

                                            restore_backups(
                                                api,
                                                [&api, on_error]() {
                                                    spdlog::info("[ConfigEditor] Backups "
                                                                 "restored, sending recovery "
                                                                 "FIRMWARE_RESTART");
                                                    api.restart_firmware(
                                                        [on_error]() {
                                                            if (on_error)
                                                                on_error(
                                                                    "Config change caused "
                                                                    "Klipper to fail. Original "
                                                                    "config restored.");
                                                        },
                                                        [on_error](const MoonrakerError& err) {
                                                            spdlog::error(
                                                                "[ConfigEditor] Recovery "
                                                                "FIRMWARE_RESTART failed: {}",
                                                                err.message);
                                                            if (on_error)
                                                                on_error("Config change caused "
                                                                         "Klipper to fail. Backups "
                                                                         "restored but restart "
                                                                         "failed: " +
                                                                         err.message);
                                                        });
                                                },
                                                [on_error](const std::string& restore_err) {
                                                    spdlog::error(
                                                        "[ConfigEditor] Failed to restore "
                                                        "backups: {}",
                                                        restore_err);
                                                    if (on_error)
                                                        on_error(
                                                            "Config change caused Klipper to "
                                                            "fail AND backup restore failed: " +
                                                            restore_err);
                                                });
                                        });
                                },
                                [on_error](const MoonrakerError& err) {
                                    spdlog::error("[ConfigEditor] FIRMWARE_RESTART failed: {}",
                                                  err.message);
                                    if (on_error)
                                        on_error("Failed to send FIRMWARE_RESTART: " + err.message);
                                });
                        },
                        [file_path, on_error](const MoonrakerError& err) {
                            spdlog::error("[ConfigEditor] Failed to upload modified {}: {}",
                                          file_path, err.message);
                            if (on_error)
                                on_error("Failed to upload " + file_path + ": " + err.message);
                        });
                },
                on_error);
        },
        on_error);
}

void KlipperConfigEditor::safe_edit_value(IMoonrakerAPI& api, const std::string& section,
                                          const std::string& key, const std::string& new_value,
                                          SuccessCallback on_success, ErrorCallback on_error,
                                          int restart_timeout_ms) {
    spdlog::info("[ConfigEditor] Starting safe edit: [{}] {} = {}", section, key, new_value);

    // Step 1: Apply the edit (backup + write)
    edit_value(
        api, section, key, new_value,
        [this, &api, on_success, on_error, restart_timeout_ms]() {
            // Step 2: Edit succeeded, send FIRMWARE_RESTART
            spdlog::info("[ConfigEditor] Edit written, sending FIRMWARE_RESTART");

            api.restart_firmware(
                [this, &api, on_success, on_error, restart_timeout_ms]() {
                    // Step 3: FIRMWARE_RESTART command accepted, monitor reconnection.
                    // Route through HttpExecutor::fast() — raw std::thread spawn crashes
                    // with std::terminate on AD5M under thread exhaustion (#724, #837).
                    // Capture callbacks and timeout by value for thread safety.
                    helix::http::HttpExecutor::fast().submit([this, &api, on_success, on_error,
                                                              restart_timeout_ms]() {
                        const auto poll_interval = std::chrono::milliseconds(500);
                        const auto timeout = std::chrono::milliseconds(restart_timeout_ms);
                        const auto start = std::chrono::steady_clock::now();

                        // Phase 1: Wait for disconnect (Klipper going down).
                        // It may already be disconnected by the time we check.
                        bool saw_disconnect = false;
                        while (std::chrono::steady_clock::now() - start < timeout) {
                            if (!api.is_connected()) {
                                saw_disconnect = true;
                                spdlog::debug("[ConfigEditor] Klipper disconnected after "
                                              "FIRMWARE_RESTART");
                                break;
                            }
                            std::this_thread::sleep_for(poll_interval);
                        }

                        if (!saw_disconnect) {
                            // Klipper never disconnected. It might have restarted so fast
                            // we missed it, or the restart failed silently.
                            // Treat as success since it's still connected.
                            spdlog::info("[ConfigEditor] Klipper stayed connected after "
                                         "FIRMWARE_RESTART (fast restart)");
                            cleanup_backups(api, [on_success]() {
                                spdlog::info("[ConfigEditor] Safe edit complete (fast restart)");
                                if (on_success)
                                    on_success();
                            });
                            return;
                        }

                        // Phase 2: Wait for reconnect within remaining timeout.
                        while (std::chrono::steady_clock::now() - start < timeout) {
                            if (api.is_connected()) {
                                auto elapsed =
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - start);
                                spdlog::info("[ConfigEditor] Klipper reconnected after {}ms",
                                             elapsed.count());
                                cleanup_backups(api, [on_success]() {
                                    spdlog::info("[ConfigEditor] Safe edit complete, backups "
                                                 "cleaned up");
                                    if (on_success)
                                        on_success();
                                });
                                return;
                            }
                            std::this_thread::sleep_for(poll_interval);
                        }

                        // Timeout: Klipper did not come back. Revert the edit.
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start);
                        spdlog::error("[ConfigEditor] Klipper failed to reconnect within "
                                      "{}ms, reverting config",
                                      elapsed.count());

                        restore_backups(
                            api,
                            [&api, on_error]() {
                                // Backups restored, try another FIRMWARE_RESTART to recover
                                spdlog::info("[ConfigEditor] Backups restored, sending "
                                             "recovery FIRMWARE_RESTART");
                                api.restart_firmware(
                                    [on_error]() {
                                        if (on_error)
                                            on_error("Config change caused Klipper to fail. "
                                                     "Original config restored.");
                                    },
                                    [on_error](const MoonrakerError& err) {
                                        spdlog::error("[ConfigEditor] Recovery "
                                                      "FIRMWARE_RESTART failed: {}",
                                                      err.message);
                                        if (on_error)
                                            on_error("Config change caused Klipper to fail. "
                                                     "Backups restored but restart failed: " +
                                                     err.message);
                                    });
                            },
                            [on_error](const std::string& restore_err) {
                                spdlog::error("[ConfigEditor] Failed to restore backups: {}",
                                              restore_err);
                                if (on_error)
                                    on_error("Config change caused Klipper to fail AND backup "
                                             "restore failed: " +
                                             restore_err);
                            });
                    });
                },
                [on_error](const MoonrakerError& err) {
                    spdlog::error("[ConfigEditor] FIRMWARE_RESTART failed: {}", err.message);
                    if (on_error)
                        on_error("Failed to send FIRMWARE_RESTART: " + err.message);
                });
        },
        on_error);
}

} // namespace helix::system
