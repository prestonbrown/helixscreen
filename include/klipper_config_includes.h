// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class IMoonrakerAPI;
struct MoonrakerError;

namespace helix::system {

// ============================================================================
// Pure path/glob utilities (extracted from KlipperConfigEditor)
// ============================================================================

/// Get the directory portion of a file path (everything before the last '/')
[[nodiscard]] std::string config_get_directory(const std::string& path);

/// Resolve a relative include path against the directory of the including file
[[nodiscard]] std::string config_resolve_path(const std::string& current_file,
                                              const std::string& include_path);

/// Simple glob pattern matching for Klipper include patterns (supports '*' and '?' wildcards)
[[nodiscard]] bool config_glob_match(const std::string& pattern, const std::string& text);

/// Find all files in the map that match a glob pattern (resolved relative to current file)
[[nodiscard]] std::vector<std::string>
config_match_glob(const std::map<std::string, std::string>& files, const std::string& current_file,
                  const std::string& include_pattern);

// ============================================================================
// Include resolution
// ============================================================================

/// Extract [include ...] directives from config file content.
/// Returns a list of include paths/patterns (e.g., "macros.cfg", "conf.d/*.cfg").
[[nodiscard]] std::vector<std::string> extract_includes(const std::string& content);

/// Walk the include chain from root_file and return the set of active file paths.
/// Pure function: given a map of filename->content, follows [include ...] directives
/// recursively, handling globs and cycle detection.
/// @param files Map of filename -> content (all files in config directory)
/// @param root_file Starting file (usually "printer.cfg")
/// @param max_depth Maximum recursion depth (default 5)
/// @return Set of file paths that are part of the active include chain
[[nodiscard]] std::set<std::string>
resolve_active_files(const std::map<std::string, std::string>& files, const std::string& root_file,
                     int max_depth = 5);

// ============================================================================
// Async Moonraker integration
// ============================================================================

using ActiveFilesCallback = std::function<void(const std::set<std::string>&)>;
using ErrorCallback = std::function<void(const std::string&)>;

/// Callback providing active files AND their downloaded contents
using ActiveFilesWithContentCallback =
    std::function<void(const std::set<std::string>&, const std::map<std::string, std::string>&)>;

/// Async wrapper: lists config directory via Moonraker, downloads printer.cfg and
/// all included files, then resolves the active file set.
/// Unlike KlipperConfigEditor::download_with_includes, this handles glob includes
/// by cross-referencing the full file listing.
void resolve_active_config_files(IMoonrakerAPI& api, ActiveFilesCallback on_complete,
                                 ErrorCallback on_error);

/// Async wrapper that also returns file contents for the active files.
/// Identical to resolve_active_config_files() but the callback also receives
/// the map of filename -> content for all downloaded config files.
void resolve_active_config_files_with_content(IMoonrakerAPI& api,
                                              ActiveFilesWithContentCallback on_complete,
                                              ErrorCallback on_error);

} // namespace helix::system
