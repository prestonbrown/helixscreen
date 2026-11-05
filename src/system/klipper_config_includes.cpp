// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "klipper_config_includes.h"

#include "i_moonraker_api.h"
#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
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
    // Normal form, because Moonraker lists and serves paths without "." or "..",
    // while Klipper resolves them, so `../macros/x.cfg` is a file it really reads.
    std::string dir = config_get_directory(current_file);
    const std::string joined = dir.empty() ? include_path : dir + "/" + include_path;
    return std::filesystem::path(joined).lexically_normal().generic_string();
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

namespace {

/// One [include] waiting on the listing that resolves it.
struct PendingInclude {
    std::string file;
    std::string pattern;
    int depth = 0;
};

/// Callbacks may run on any thread or inside the list()/fetch() call; only one
/// request is outstanding at a time, so the walk state needs no lock.
struct IncludeWalk {
    std::string root;
    ConfigListDirFn list;
    ConfigFetchFn fetch;
    ActiveFilesWithContentCallback on_complete;
    ErrorCallback on_error;
    int max_depth = 5;
    std::map<std::string, std::string> known;
    std::set<std::string> listed;
    bool whole_tree_listed = false;
    std::vector<PendingInclude> pending;
    std::vector<std::pair<std::string, int>> queue;
    std::set<std::string> seen;
    std::map<std::string, std::string> files;
};

void step_include_walk(const std::shared_ptr<IncludeWalk>& walk) {
    while (!walk->pending.empty()) {
        const PendingInclude inc = walk->pending.front();
        const std::string resolved = config_resolve_path(inc.file, inc.pattern);
        const std::string dir = config_get_directory(resolved);
        const bool wild_dir = dir.find_first_of("*?") != std::string::npos ||
                              resolved.find("**") != std::string::npos;
        // Listed paths are root-relative, so an include spelled this way matches nothing.
        const bool unreachable = dir.find("..") != std::string::npos ||
                                 (!resolved.empty() && resolved.front() == '/');
        const bool ready = unreachable || walk->whole_tree_listed ||
                           (!wild_dir && walk->listed.count(dir) > 0);
        if (!ready) {
            walk->list(
                dir, wild_dir,
                [walk, dir, wild_dir](std::vector<std::string> paths) {
                    for (auto& path : paths)
                        walk->known.emplace(std::move(path), std::string());
                    if (wild_dir)
                        walk->whole_tree_listed = true;
                    else
                        walk->listed.insert(dir);
                    step_include_walk(walk);
                },
                [walk](const std::string& message) {
                    if (walk->on_error)
                        walk->on_error("Failed to list config files: " + message);
                });
            return;
        }
        walk->pending.erase(walk->pending.begin());
        if (unreachable)
            continue;
        const bool has_wildcard = inc.pattern.find_first_of("*?") != std::string::npos;
        const std::vector<std::string> targets =
            has_wildcard ? config_match_glob(walk->known, inc.file, inc.pattern)
                         : std::vector<std::string>{resolved};
        for (const auto& target : targets) {
            if (walk->known.count(target) && walk->seen.insert(target).second)
                walk->queue.emplace_back(target, inc.depth);
        }
    }

    if (walk->queue.empty()) {
        if (!walk->on_complete)
            return;
        if (walk->files.empty())
            walk->on_complete({}, {});
        else
            walk->on_complete(resolve_active_files(walk->files, walk->root), walk->files);
        return;
    }

    const auto [path, depth] = walk->queue.front();
    walk->queue.erase(walk->queue.begin());
    walk->fetch(
        path,
        [walk, path = path, depth = depth](std::string content) {
            if (depth < walk->max_depth) {
                for (auto& pattern : extract_includes(content))
                    walk->pending.push_back({path, std::move(pattern), depth + 1});
            }
            walk->files[path] = std::move(content);
            step_include_walk(walk);
        },
        [walk, path = path](const std::string& message, bool fatal) {
            if (fatal) {
                if (walk->on_error)
                    walk->on_error("Failed to download " + path + ": " + message);
                return;
            }
            // An include that cannot be fetched leaves the rest of the config readable.
            spdlog::warn("[ConfigIncludes] Failed to download {}: {}", path, message);
            step_include_walk(walk);
        });
}

} // namespace

void walk_config_includes(const std::string& root_file, ConfigListDirFn list, ConfigFetchFn fetch,
                          ActiveFilesWithContentCallback on_complete, ErrorCallback on_error,
                          int max_depth) {
    auto walk = std::make_shared<IncludeWalk>();
    walk->root = root_file;
    walk->list = std::move(list);
    walk->fetch = std::move(fetch);
    walk->on_complete = std::move(on_complete);
    walk->on_error = std::move(on_error);
    walk->max_depth = max_depth;
    walk->pending.push_back({std::string(), root_file, 0});
    step_include_walk(walk);
}

void resolve_active_config_files_with_content(IMoonrakerAPI& api,
                                              ActiveFilesWithContentCallback on_complete,
                                              ErrorCallback on_error) {
    // Each step starts from a completion on a background thread, while teardown destroys
    // the API on the main thread without draining those. On the main thread the token
    // proves the API still alive.
    auto is_cfg = [](const std::string& path) {
        return path.size() > 4 && path.compare(path.size() - 4, 4, ".cfg") == 0;
    };
    ConfigListDirFn list = [&api, tok = api.lifetime_token(),
                            is_cfg](const std::string& dir, bool whole_tree,
                                    std::function<void(std::vector<std::string>)> ok,
                                    std::function<void(std::string)> fail) {
        helix::ui::queue_update([&api, tok, is_cfg, dir, whole_tree, ok, fail]() {
            if (tok.expired()) {
                fail("the printer disconnected");
                return;
            }
            if (whole_tree) {
                api.files().list_files(
                    "config", "", true,
                    [is_cfg, ok](const std::vector<FileInfo>& files) {
                        std::vector<std::string> out;
                        for (const auto& f : files) {
                            std::string path = f.path.empty() ? f.filename : f.path;
                            if (!f.is_dir && is_cfg(path))
                                out.push_back(std::move(path));
                        }
                        ok(std::move(out));
                    },
                    [fail](const MoonrakerError& err) { fail(err.message); });
                return;
            }
            api.files().get_directory(
                "config", dir,
                [is_cfg, dir, ok](const std::vector<FileInfo>& files) {
                    std::vector<std::string> out;
                    for (const auto& f : files) {
                        std::string path = dir.empty() ? f.filename : dir + "/" + f.filename;
                        if (!f.is_dir && is_cfg(path))
                            out.push_back(std::move(path));
                    }
                    ok(std::move(out));
                },
                [ok, fail](const MoonrakerError& err) {
                    // Moonraker reports this as a plain 400 with this text. An [include]
                    // may name a directory that is not there.
                    if (err.type == MoonrakerErrorType::FILE_NOT_FOUND ||
                        err.message.find("Directory does not exist") != std::string::npos)
                        ok({});
                    else
                        fail(err.message);
                });
        });
    };
    ConfigFetchFn fetch = [&api, tok = api.lifetime_token()](
                              const std::string& path, std::function<void(std::string)> ok,
                              std::function<void(std::string, bool)> fail) {
        helix::ui::queue_update([&api, tok, path, ok, fail]() {
            if (tok.expired()) {
                fail("the printer disconnected", true);
                return;
            }
            api.transfers().download_file(
                "config", path, [ok](const std::string& content) { ok(content); },
                [fail](const MoonrakerError& err) { fail(err.message, false); });
        });
    };
    walk_config_includes("printer.cfg", std::move(list), std::move(fetch), std::move(on_complete),
                         std::move(on_error));
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
