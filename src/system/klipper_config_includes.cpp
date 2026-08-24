// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "klipper_config_includes.h"

#include "i_moonraker_api.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

namespace helix::system {

// ============================================================================
// Pure path/glob utilities
// ============================================================================

std::string config_get_directory(const std::string& path) {
    auto pos = path.rfind('/');
    if (pos == std::string::npos)
        return "";
    return path.substr(0, pos);
}

std::string config_resolve_path(const std::string& current_file, const std::string& include_path) {
    std::string dir = config_get_directory(current_file);
    if (dir.empty())
        return include_path;
    return dir + "/" + include_path;
}

bool config_glob_match(const std::string& pattern, const std::string& text) {
    size_t pi = 0, ti = 0;
    size_t star_pi = std::string::npos, star_ti = 0;

    while (ti < text.size()) {
        if (pi < pattern.size() && (pattern[pi] == text[ti] || pattern[pi] == '?')) {
            ++pi;
            ++ti;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            star_pi = pi;
            star_ti = ti;
            ++pi;
        } else if (star_pi != std::string::npos) {
            pi = star_pi + 1;
            ++star_ti;
            ti = star_ti;
        } else {
            return false;
        }
    }

    while (pi < pattern.size() && pattern[pi] == '*')
        ++pi;

    return pi == pattern.size();
}

std::vector<std::string> config_match_glob(const std::map<std::string, std::string>& files,
                                           const std::string& current_file,
                                           const std::string& include_pattern) {
    std::string resolved = config_resolve_path(current_file, include_pattern);
    std::vector<std::string> matches;

    for (const auto& [filename, _] : files) {
        if (config_glob_match(resolved, filename)) {
            matches.push_back(filename);
        }
    }

    std::sort(matches.begin(), matches.end());
    return matches;
}

// ============================================================================
// Include extraction
// ============================================================================

std::vector<std::string> extract_includes(const std::string& content) {
    std::vector<std::string> includes;
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos)
            continue;

        std::string trimmed = line.substr(start);

        // Match [include <path>] directive — minimum valid: "[include X]" = 11 chars
        if (trimmed.size() > 10 && trimmed[0] == '[') {
            // Check for [include ...]
            if (trimmed.compare(1, 8, "include ") == 0) {
                // Find closing bracket
                auto end = trimmed.find(']', 9);
                if (end != std::string::npos) {
                    std::string path = trimmed.substr(9, end - 9);
                    // Trim whitespace from path
                    auto path_start = path.find_first_not_of(" \t");
                    auto path_end = path.find_last_not_of(" \t");
                    if (path_start != std::string::npos && path_end != std::string::npos) {
                        includes.push_back(path.substr(path_start, path_end - path_start + 1));
                    }
                }
            }
        }
    }

    return includes;
}

// ============================================================================
// Active file resolution (pure)
// ============================================================================

std::set<std::string> resolve_active_files(const std::map<std::string, std::string>& files,
                                           const std::string& root_file, int max_depth) {
    std::set<std::string> active;

    std::function<void(const std::string&, int)> process_file;
    process_file = [&](const std::string& file_path, int depth) {
        // Cycle detection
        if (active.count(file_path))
            return;

        // Depth check
        if (depth > max_depth) {
            spdlog::debug("klipper_config_includes: max include depth {} reached at {}", max_depth,
                          file_path);
            return;
        }

        // Find file content
        auto it = files.find(file_path);
        if (it == files.end()) {
            spdlog::debug("klipper_config_includes: included file not found: {}", file_path);
            return;
        }

        active.insert(file_path);

        // Extract and process includes
        auto includes = extract_includes(it->second);
        for (const auto& include_pattern : includes) {
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
    };

    process_file(root_file, 0);
    return active;
}

// ============================================================================
// Async Moonraker integration
// ============================================================================

void resolve_active_config_files_with_content(IMoonrakerAPI& api,
                                              ActiveFilesWithContentCallback on_complete,
                                              ErrorCallback on_error) {
    // api must outlive all async callbacks (guaranteed: IMoonrakerAPI is owned by PrinterState
    // singleton)
    api.files().list_files(
        "config", "", true,
        [&api, on_complete, on_error](const std::vector<FileInfo>& file_list) {
            std::vector<std::string> cfg_paths;
            for (const auto& f : file_list) {
                if (!f.is_dir) {
                    std::string path = f.path.empty() ? f.filename : f.path;
                    if (path.size() > 4 && path.substr(path.size() - 4) == ".cfg") {
                        cfg_paths.push_back(path);
                    }
                }
            }

            if (cfg_paths.empty()) {
                if (on_complete)
                    on_complete({}, {});
                return;
            }

            // Shared state for async downloads
            struct DownloadState {
                std::map<std::string, std::string> files_map;
                std::atomic<int> pending{0};
                std::mutex mutex;
                ActiveFilesWithContentCallback on_complete;
            };

            auto state = std::make_shared<DownloadState>();
            state->pending.store(static_cast<int>(cfg_paths.size()));
            state->on_complete = on_complete;

            for (const auto& path : cfg_paths) {
                api.transfers().download_file(
                    "config", path,
                    [state, path](const std::string& content) {
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->files_map[path] = content;
                        }
                        int remaining = state->pending.fetch_sub(1) - 1;
                        if (remaining == 0) {
                            auto active = resolve_active_files(state->files_map, "printer.cfg");
                            if (state->on_complete)
                                state->on_complete(active, state->files_map);
                        }
                    },
                    [state, path](const MoonrakerError& err) {
                        spdlog::warn("[ConfigIncludes] Failed to download {}: {}", path,
                                     err.message);
                        int remaining = state->pending.fetch_sub(1) - 1;
                        if (remaining == 0) {
                            auto active = resolve_active_files(state->files_map, "printer.cfg");
                            if (state->on_complete)
                                state->on_complete(active, state->files_map);
                        }
                    });
            }
        },
        [on_error](const MoonrakerError& err) {
            if (on_error)
                on_error("Failed to list config files: " + err.message);
        });
}

void resolve_active_config_files(IMoonrakerAPI& api, ActiveFilesCallback on_complete,
                                 ErrorCallback on_error) {
    resolve_active_config_files_with_content(
        api,
        [on_complete](const std::set<std::string>& active_files,
                      const std::map<std::string, std::string>& /* file_contents */) {
            if (on_complete)
                on_complete(active_files);
        },
        on_error);
}

} // namespace helix::system
