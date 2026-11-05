// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "klipper_config_includes.h"

#include "async_lifetime_guard.h"
#include "core/text_io.h"
#include "core/update_queue.h"
#include "i_moonraker_api.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>
#include <vector>

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
    // Mirrors Python's glob.glob(pattern, recursive=True), which is what Klipper
    // resolves [include] with (klippy/configfile.py). Under recursive=True only
    // a whole-segment `**` spans directories; a single `*` and `?` never match a '/'. Letting a
    // single star cross separators makes `[include conf.d/*.cfg]` appear to pull
    // in conf.d/nested/*.cfg, so a section gets attributed to a file Klipper
    // never read - and an edit written there has no effect.
    // dp[i][j]: pattern[i:] matches text[j:]. A table rather than one backtrack
    // point, because `**/*.cfg` needs both stars to retry independently.
    const size_t n = pattern.size(), m = text.size();
    std::vector<char> dp((n + 1) * (m + 1), 0);
    auto at = [&](size_t i, size_t j) -> char& { return dp[i * (m + 1) + j]; };
    at(n, m) = 1;
    for (size_t i = n; i-- > 0;) {
        const char c = pattern[i];
        for (size_t j = m + 1; j-- > 0;) {
            bool ok = false;
            if (c == '*') {
                // Only a whole-segment `**` spans directories; glued to a name
                // (`mod/**.cfg`) Python reads it as a plain `*`.
                const bool doubled = i + 1 < n && pattern[i + 1] == '*' &&
                                     (i == 0 || pattern[i - 1] == '/') &&
                                     (i + 2 == n || pattern[i + 2] == '/');
                const size_t next = i + (doubled ? 2 : 1);
                ok = at(next, j) || (j < m && (doubled || text[j] != '/') && at(i, j + 1));
                // Python glob lets `a/**/b` match `a/b`: the directories are optional.
                if (!ok && doubled && next < n && pattern[next] == '/') {
                    ok = at(next + 1, j);
                }
            } else if (j < m) {
                ok = (c == '?' ? text[j] != '/' : c == text[j]) && at(i + 1, j + 1);
            }
            at(i, j) = ok;
        }
    }
    return at(0, 0);
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
    for (std::string_view line : helix::text_io::lines(content)) {
        // A Klipper section header starts at column 0: an indented line continues
        // the previous option, and a commented one is not a section at all.
        if (line.substr(0, 9) != "[include ") {
            continue;
        }
        const size_t close = line.find(']', 9);
        if (close == std::string_view::npos) {
            continue;
        }
        const std::string_view path = helix::text_io::trim(line.substr(9, close - 9));
        if (!path.empty()) {
            includes.emplace_back(path);
        }
    }
    return includes;
}

// ============================================================================
// Active file resolution (pure)
// ============================================================================

namespace {

/// [begin, end) of each line extract_includes() takes an include from, in the same order.
std::vector<std::pair<size_t, size_t>> include_line_ranges(const std::string& content) {
    std::vector<std::pair<size_t, size_t>> ranges;
    for (std::string_view line : helix::text_io::lines(content)) {
        if (!extract_includes(std::string(line)).empty()) {
            const auto begin = static_cast<size_t>(line.data() - content.data());
            ranges.emplace_back(begin, begin + line.size());
        }
    }
    return ranges;
}

/// The files one [include] names: every match for a glob, else the path
/// relative to the including file (which may not exist).
std::vector<std::string> include_targets(const std::map<std::string, std::string>& files,
                                         const std::string& current_file,
                                         const std::string& include_pattern) {
    const bool has_wildcard = include_pattern.find('*') != std::string::npos ||
                              include_pattern.find('?') != std::string::npos;
    if (has_wildcard) {
        return config_match_glob(files, current_file, include_pattern);
    }
    return {config_resolve_path(current_file, include_pattern)};
}

} // namespace

std::set<std::string> resolve_active_files(const std::map<std::string, std::string>& files,
                                           const std::string& root_file, int max_depth,
                                           std::vector<ConfigSegment>* read_order) {
    std::set<std::string> active;
    // Klipper refuses only a file that includes itself through the chain it is
    // being read from; a file included from two places is read twice.
    std::set<std::string> reading;

    std::function<void(const std::string&, int)> process_file;
    process_file = [&](const std::string& file_path, int depth) {
        if (reading.count(file_path))
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
        reading.insert(file_path);

        auto includes = extract_includes(it->second);
        const auto include_lines =
            read_order ? include_line_ranges(it->second) : std::vector<std::pair<size_t, size_t>>{};
        size_t segment_begin = 0;
        for (size_t k = 0; k < includes.size(); ++k) {
            if (read_order) {
                read_order->push_back({file_path, segment_begin, include_lines[k].first});
                segment_begin = include_lines[k].second;
            }
            for (const auto& target : include_targets(files, file_path, includes[k])) {
                process_file(target, depth + 1);
            }
        }
        if (read_order) {
            read_order->push_back({file_path, segment_begin, it->second.size()});
        }
        reading.erase(file_path);
    };

    process_file(root_file, 0);
    return active;
}

// ============================================================================
// Async Moonraker integration
// ============================================================================

namespace {

/// What must be listed before an [include] can be resolved. A literal directory
/// costs Moonraker one scandir; only a wildcard in the directory part needs the
/// whole tree, which Moonraker walks with its event loop blocked (seconds per
/// thousand files on a printer board).
struct IncludeListing {
    enum class Kind { None, Dir, WholeTree };
    Kind kind = Kind::None;
    std::string dir;
};

IncludeListing include_listing(const std::string& current_file, const std::string& pattern) {
    const std::string resolved = config_resolve_path(current_file, pattern);
    const std::string dir = config_get_directory(resolved);
    if (dir.find_first_of("*?") != std::string::npos || resolved.find("**") != std::string::npos) {
        return {IncludeListing::Kind::WholeTree, {}};
    }
    // Listed paths are root-relative and never spell these, so neither can match.
    if (!resolved.empty() && resolved.front() == '/') {
        return {};
    }
    if (dir == ".." || dir.rfind("../", 0) == 0 || dir.find("/../") != std::string::npos ||
        (dir.size() >= 3 && dir.compare(dir.size() - 3, 3, "/..") == 0)) {
        return {};
    }
    return {IncludeListing::Kind::Dir, dir};
}

/// Key in Walk::requested for the one whole-tree listing; no directory is named this.
constexpr const char* kWholeTreeKey = "*";

} // namespace

void walk_include_graph(const std::string& root_file, ConfigListFn list, ConfigDownloadFn download,
                        ActiveFilesWithContentCallback on_complete, ErrorCallback on_error,
                        size_t max_in_flight, int max_depth) {
    // An [include] waiting on the listing that resolves it.
    struct PendingInclude {
        std::string file;
        std::string pattern;
        int depth = 0;
    };

    // Shared by every callback, which may run on any thread or inside the list()
    // or download() call itself; the lock is never held across either.
    struct Walk {
        std::mutex mutex;
        // Keyed like the downloaded map so include_targets() globs against it; the
        // values stay empty, only the names are known until a file arrives.
        std::map<std::string, std::string> known;
        std::string root;
        ConfigListFn list;
        ConfigDownloadFn download;
        ActiveFilesWithContentCallback on_complete;
        ErrorCallback on_error;
        size_t max_in_flight = 1;
        int max_depth = 0;
        std::vector<PendingInclude> pending;
        std::set<std::string> requested; // directories asked for, or kWholeTreeKey
        std::set<std::string> listed;    // directories answered
        bool whole_tree_listed = false;
        std::vector<std::pair<std::string, int>> queue; // path, include depth
        std::set<std::string> seen;
        std::map<std::string, std::string> files;
        size_t in_flight = 0;
        std::string error;
        bool reported = false;
    };
    auto walk = std::make_shared<Walk>();
    walk->root = root_file;
    walk->list = std::move(list);
    walk->download = std::move(download);
    walk->on_complete = std::move(on_complete);
    walk->on_error = std::move(on_error);
    walk->max_in_flight = std::max<size_t>(1, max_in_flight);
    walk->max_depth = max_depth;
    walk->pending.push_back({std::string(), root_file, 0});

    // Report once every outstanding download has returned. Called with the lock
    // held; the returned closure runs after it is released.
    auto take_report = [](Walk& w) -> helix::Callback<void()> {
        if (w.reported || w.in_flight > 0 ||
            (w.error.empty() && (!w.queue.empty() || !w.pending.empty())))
            return {};
        w.reported = true;
        if (!w.error.empty()) {
            return [cb = w.on_error, err = w.error]() {
                if (cb)
                    cb(err);
            };
        }
        return [cb = w.on_complete, root = w.root, files = std::move(w.files)]() {
            if (!cb)
                return;
            if (files.empty())
                cb({}, {}); // the root file does not exist
            else
                cb(resolve_active_files(files, root), files);
        };
    };

    std::shared_ptr<helix::SharedCallback<void()>> pump =
        std::make_shared<helix::SharedCallback<void()>>();
    std::weak_ptr<helix::SharedCallback<void()>> weak_pump = pump;
    *pump = [walk, take_report, weak_pump]() {
        for (;;) {
            std::pair<std::string, int> next;
            bool have_next = false;
            IncludeListing to_list;
            bool have_list = false;
            helix::Callback<void()> idle_report;
            {
                std::lock_guard<std::mutex> lock(walk->mutex);
                if (!walk->error.empty())
                    return;
                for (auto it = walk->pending.begin(); it != walk->pending.end();) {
                    const IncludeListing need = include_listing(it->file, it->pattern);
                    const bool ready = need.kind == IncludeListing::Kind::None ||
                                       walk->whole_tree_listed ||
                                       (need.kind == IncludeListing::Kind::Dir &&
                                        walk->listed.count(need.dir) > 0);
                    if (ready) {
                        for (auto& t : include_targets(walk->known, it->file, it->pattern)) {
                            if (walk->known.count(t) && walk->seen.insert(t).second)
                                walk->queue.emplace_back(std::move(t), it->depth);
                        }
                        it = walk->pending.erase(it);
                        continue;
                    }
                    const std::string key =
                        need.kind == IncludeListing::Kind::WholeTree ? kWholeTreeKey : need.dir;
                    if (!have_list && walk->in_flight < walk->max_in_flight &&
                        walk->requested.insert(key).second) {
                        to_list = need;
                        have_list = true;
                        ++walk->in_flight;
                    }
                    ++it;
                }
                if (!have_list) {
                    if (!walk->queue.empty() && walk->in_flight < walk->max_in_flight) {
                        next = std::move(walk->queue.front());
                        walk->queue.erase(walk->queue.begin());
                        ++walk->in_flight;
                        have_next = true;
                    } else {
                        idle_report = take_report(*walk);
                    }
                }
            }
            if (idle_report) {
                idle_report();
                return;
            }
            if (have_list) {
                auto self = weak_pump.lock();
                const bool whole = to_list.kind == IncludeListing::Kind::WholeTree;
                const std::string dir = to_list.dir;
                auto deferred = std::make_shared<std::atomic<bool>>(false);
                walk->list(
                    dir, whole,
                    [walk, take_report, self, dir, whole](std::vector<std::string> paths) {
                        helix::Callback<void()> report;
                        {
                            std::lock_guard<std::mutex> lock(walk->mutex);
                            --walk->in_flight;
                            for (auto& path : paths)
                                walk->known.emplace(std::move(path), std::string());
                            if (whole)
                                walk->whole_tree_listed = true;
                            else
                                walk->listed.insert(dir);
                            report = take_report(*walk);
                        }
                        if (report)
                            report();
                        else if (self)
                            (*self)();
                    },
                    [walk, take_report, dir, whole, deferred](std::string message,
                                                              bool queue_full) {
                        helix::Callback<void()> report;
                        {
                            std::lock_guard<std::mutex> lock(walk->mutex);
                            --walk->in_flight;
                            if (queue_full && walk->in_flight > 0 && walk->error.empty()) {
                                walk->requested.erase(whole ? std::string(kWholeTreeKey) : dir);
                                deferred->store(true);
                                return;
                            }
                            if (walk->error.empty())
                                walk->error = "Failed to list config files: " + message;
                            report = take_report(*walk);
                        }
                        if (report)
                            report();
                    });
                if (deferred->load())
                    return;
                continue;
            }
            if (!have_next)
                return;
            auto self = weak_pump.lock();
            const std::string path = next.first;
            const int depth = next.second;
            auto deferred = std::make_shared<std::atomic<bool>>(false);
            walk->download(
                path,
                [walk, take_report, self, path, depth](std::string content) {
                    helix::Callback<void()> report;
                    {
                        std::lock_guard<std::mutex> lock(walk->mutex);
                        --walk->in_flight;
                        if (walk->error.empty() && depth < walk->max_depth) {
                            for (auto& pattern : extract_includes(content))
                                walk->pending.push_back({path, std::move(pattern), depth + 1});
                        }
                        walk->files[path] = std::move(content);
                        report = take_report(*walk);
                    }
                    if (report)
                        report();
                    else if (self)
                        (*self)();
                },
                [walk, take_report, path, depth, deferred](std::string message, bool queue_full) {
                    helix::Callback<void()> report;
                    {
                        std::lock_guard<std::mutex> lock(walk->mutex);
                        --walk->in_flight;
                        // Backpressure: retry when an in-flight download completes.
                        // With nothing in flight there is no completion to wait for.
                        // A refusal can arrive after download() returns, when the
                        // request was handed to another thread first.
                        if (queue_full && walk->in_flight > 0 && walk->error.empty()) {
                            walk->queue.insert(walk->queue.begin(), {path, depth});
                            deferred->store(true);
                            spdlog::debug("[ConfigIncludes] {} not queued ({}), retrying later",
                                          path, message);
                            return;
                        }
                        spdlog::warn("[ConfigIncludes] Failed to download {}: {}", path, message);
                        if (walk->error.empty())
                            walk->error = "Failed to download " + path + ": " + message;
                        report = take_report(*walk);
                    }
                    if (report)
                        report();
                });
            // Refused before download() returned: wait for a completion.
            if (deferred->load())
                return;
        }
    };
    (*pump)();
}

ConfigDownloadFn api_config_download(IMoonrakerAPI& api,
                                     helix::SharedCallback<bool()> still_wanted) {
    return [&api, tok = api.lifetime_token(), still_wanted = std::move(still_wanted)](
               const std::string& path, helix::SharedCallback<void(std::string)> ok,
               helix::SharedCallback<void(std::string, bool)> fail) {
        // Queued directly rather than through tok.defer(), which would drop the
        // request silently once the API is gone and leave the walk waiting forever.
        helix::ui::queue_update(
            "ConfigIncludes::download", [&api, tok, still_wanted, path, ok, fail]() {
                if (tok.expired()) {
                    fail("the printer disconnected", false);
                    return;
                }
                if (still_wanted && !still_wanted()) {
                    fail(kConfigDownloadNotWanted, false);
                    return;
                }
                api.transfers().download_file(
                    "config", path, [ok](const std::string& content) { ok(content); },
                    [fail](const MoonrakerError& err) {
                        fail(err.message, err.type == MoonrakerErrorType::QUEUE_FULL);
                    });
            });
    };
}

void download_include_graph(std::vector<std::string> listing, const std::string& root_file,
                            ConfigDownloadFn download, ActiveFilesWithContentCallback on_complete,
                            ErrorCallback on_error, size_t max_in_flight, int max_depth) {
    auto paths = std::make_shared<const std::vector<std::string>>(std::move(listing));
    ConfigListFn list = [paths](const std::string& dir, bool whole_tree,
                                helix::SharedCallback<void(std::vector<std::string>)> ok,
                                helix::SharedCallback<void(std::string, bool)>) {
        std::vector<std::string> out;
        for (const auto& path : *paths) {
            if (whole_tree || config_get_directory(path) == dir)
                out.push_back(path);
        }
        ok(std::move(out));
    };
    walk_include_graph(root_file, std::move(list), std::move(download), std::move(on_complete),
                       std::move(on_error), max_in_flight, max_depth);
}

ConfigListFn api_config_list(IMoonrakerAPI& api, std::string suffix) {
    return [&api, tok = api.lifetime_token(),
            suffix = std::move(suffix)](const std::string& dir, bool whole_tree,
                                        helix::SharedCallback<void(std::vector<std::string>)> ok,
                                        helix::SharedCallback<void(std::string, bool)> fail) {
        // Queued directly for the reason api_config_download() is: the next step
        // of the walk runs from a completion on a background thread.
        helix::ui::queue_update(
            "ConfigIncludes::list", [&api, tok, suffix, dir, whole_tree, ok, fail]() {
                if (tok.expired()) {
                    fail("the printer disconnected", false);
                    return;
                }
                auto keep = [suffix](const std::string& path) {
                    return path.size() > suffix.size() &&
                           path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
                };
                auto on_error = [ok, fail](const MoonrakerError& err) {
                    fail(err.message, err.type == MoonrakerErrorType::QUEUE_FULL);
                };
                if (whole_tree) {
                    api.files().list_files(
                        "config", "", true,
                        [keep, ok](const std::vector<FileInfo>& files) {
                            std::vector<std::string> out;
                            for (const auto& f : files) {
                                std::string path = f.path.empty() ? f.filename : f.path;
                                if (!f.is_dir && keep(path))
                                    out.push_back(std::move(path));
                            }
                            ok(std::move(out));
                        },
                        on_error);
                    return;
                }
                api.files().get_directory(
                    "config", dir,
                    [keep, dir, ok](const std::vector<FileInfo>& files) {
                        std::vector<std::string> out;
                        for (const auto& f : files) {
                            std::string path = dir.empty() ? f.filename : dir + "/" + f.filename;
                            if (!f.is_dir && keep(path))
                                out.push_back(std::move(path));
                        }
                        ok(std::move(out));
                    },
                    [ok, on_error](const MoonrakerError& err) {
                        // An [include] may name a directory that is not there.
                        if (moonraker_directory_missing(err))
                            ok({});
                        else
                            on_error(err);
                    });
            });
    };
}

void download_config_include_graph(IMoonrakerAPI& api, const std::string& root_file,
                                   const std::string& suffix,
                                   ActiveFilesWithContentCallback on_complete,
                                   ErrorCallback on_error, ConfigDownloadFn download) {
    if (!download)
        download = api_config_download(api);
    walk_include_graph(root_file, api_config_list(api, suffix), std::move(download),
                       std::move(on_complete), std::move(on_error));
}

void resolve_active_config_files_with_content(IMoonrakerAPI& api,
                                              ActiveFilesWithContentCallback on_complete,
                                              ErrorCallback on_error) {
    download_config_include_graph(api, "printer.cfg", ".cfg", std::move(on_complete),
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
