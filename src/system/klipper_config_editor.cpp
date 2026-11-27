// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "klipper_config_editor.h"

#include "app_globals.h"
#include "core/http_executor.h"
#include "core/text_io.h"
#include "core/update_queue.h"
#include "helix_plugin_features.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "klipper_config_includes.h"
#include "lvgl.h"
#include "moonraker_error.h"
#include "notice.h"
#include "platform_info.h"
#include "printer_state.h"
#include "src/api/moonraker_gcode_guards.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <set>
#include <thread>

namespace helix::system {

std::optional<ConfigKey> ConfigStructure::find_key(const std::string& section,
                                                   const std::string& key) const {
    auto it = sections.find(section);
    if (it == sections.end())
        return std::nullopt;

    // Klipper keeps the last of a repeated key, so that is the line an edit must reach.
    for (auto k = it->second.keys.rbegin(); k != it->second.keys.rend(); ++k) {
        if (k->name == key)
            return *k;
    }
    return std::nullopt;
}

// How long the destructor is willing to wait for a health monitor to notice the
// cancel flag. The monitor checks it once per poll (500ms by default), so a live
// one is gone in well under a second; the bound only exists so a wedged worker
// cannot stall shutdown on the main thread.
static constexpr auto kMonitorJoinTimeout = std::chrono::seconds(3);

KlipperConfigEditor::~KlipperConfigEditor() {
    cancel_restart_monitors();
}

void KlipperConfigEditor::track_restart_monitor(std::future<void> monitor) {
    std::lock_guard<std::mutex> lock(monitor_mutex_);

    // Re-entrancy: a second safe_edit_value()/safe_multi_edit() while a monitor
    // is still polling keeps BOTH futures. The older monitor is still running
    // and still has to be waited for, so overwriting a single slot would leave
    // it unjoined — exactly the bug this member exists to prevent. Monitors that
    // already finished are pruned here so repeated saves don't accumulate.
    restart_monitors_.erase(std::remove_if(restart_monitors_.begin(), restart_monitors_.end(),
                                           [](const std::future<void>& f) {
                                               return !f.valid() ||
                                                      f.wait_for(std::chrono::seconds(0)) ==
                                                          std::future_status::ready;
                                           }),
                            restart_monitors_.end());

    restart_monitors_.push_back(std::move(monitor));
}

void KlipperConfigEditor::cancel_restart_monitors() {
    monitor_cancel_->store(true);

    std::vector<std::future<void>> pending;
    {
        std::lock_guard<std::mutex> lock(monitor_mutex_);
        pending.swap(restart_monitors_);
    }

    // wait_until(), never get(): the promise is BROKEN (std::future_error) when
    // the executor stopped before the item ran, and a destructor must not throw.
    // A broken promise still makes the future ready, so that case returns
    // immediately. The whole set shares one deadline.
    //
    // This would self-deadlock if an editor were ever destroyed ON an
    // HttpExecutor worker — the worker would be waiting for its own item. Today
    // the owning panels are destroyed on the main thread by
    // StaticPanelRegistry::destroy_all(); keep it that way.
    const auto deadline = std::chrono::steady_clock::now() + kMonitorJoinTimeout;
    for (auto& monitor : pending) {
        if (!monitor.valid())
            continue;
        // valid() holds here, so wait_until cannot throw future_error for a
        // missing shared state.
        if (monitor.wait_until(deadline) != std::future_status::ready) {
            spdlog::warn("[ConfigEditor] Restart monitor did not finish within {}s — "
                         "giving up the wait",
                         kMonitorJoinTimeout.count());
        }
    }
}

ConfigStructure KlipperConfigEditor::parse_structure(const std::string& content) {
    ConfigStructure result;

    if (content.empty()) {
        result.total_lines = 0;
        return result;
    }

    // Split content into lines
    std::vector<std::string> lines;
    for (std::string_view sv : helix::text_io::lines(content)) {
        lines.push_back(std::string(sv));
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
                if (sec.name.empty()) {
                    sec.name = current_section;
                    sec.line_start = i;
                }
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
        key_name = helix::text_io::to_lower(std::string(helix::text_io::trim(key_name)));

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

    // Blank lines after a value separate it from what follows; they are not part of it.
    for (auto& [name, sec] : result.sections) {
        for (auto& key : sec.keys) {
            while (key.end_line > key.line_number &&
                   helix::text_io::trim(lines[key.end_line]).empty()) {
                --key.end_line;
            }
        }
    }

    return result;
}

// True when every edit is an ADD_KEY, i.e. the whole list reads "make sure this
// key says X" and carries no assumption that the section already exists.
bool all_add_key(const std::vector<ConfigEdit>& edits) {
    return !edits.empty() && std::all_of(edits.begin(), edits.end(), [](const ConfigEdit& e) {
        return e.type == ConfigEdit::Type::ADD_KEY;
    });
}

namespace {

// The one file every Klipper install has and every include chain starts from.
// Also the home for a section no active file declares yet.
constexpr const char* kRootConfigFile = "printer.cfg";

// Split content into lines, preserving the ability to rejoin with \n
std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    for (std::string_view sv : helix::text_io::lines(content)) {
        lines.push_back(std::string(sv));
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
                                                          const std::string& new_value) {
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
    // A bare `key:` has no spacing to preserve.
    if (spacing.empty() && !new_value.empty())
        spacing = " ";
    lines[target] = prefix + spacing + new_value;

    bool trailing = !content.empty() && content.back() == '\n';
    return join_lines(lines, trailing);
}

std::optional<std::string> KlipperConfigEditor::add_key(const std::string& content,
                                                        const std::string& section,
                                                        const std::string& key,
                                                        const std::string& value,
                                                        const std::string& delimiter) {
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

std::optional<std::string> KlipperConfigEditor::add_section(const std::string& content,
                                                            const std::string& section) const {
    auto structure = parse_structure(content);
    if (structure.sections.count(section))
        return std::nullopt;

    auto lines = split_lines(content);

    // Everything from the `#*# <--- SAVE_CONFIG` line down is Klipper's, and it
    // rewrites that block wholesale — a section written into or below it would be
    // lost on the next SAVE_CONFIG. Insert above it instead.
    size_t insert_at = lines.size();
    if (structure.save_config_line >= 0 &&
        structure.save_config_line <= static_cast<int>(lines.size())) {
        insert_at = static_cast<size_t>(structure.save_config_line);
    }

    std::vector<std::string> block;
    if (insert_at > 0 && !lines[insert_at - 1].empty())
        block.push_back(""); // blank line separating us from what came before
    block.push_back("[" + section + "]");
    if (insert_at < lines.size())
        block.push_back(""); // and from the SAVE_CONFIG block that follows

    lines.insert(lines.begin() + static_cast<long>(insert_at), block.begin(), block.end());

    // Always terminate: a file that lacked a trailing newline would otherwise end
    // mid-section-header once keys get appended.
    return join_lines(lines, true);
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
    // Klipper reads each include where its line stands, so a section's owner is
    // the file of its last header in that read order, not the includer's own
    // header after all of its includes.
    std::vector<ConfigSegment> read_order;
    (void)resolve_active_files(files, root_file, max_depth, &read_order);

    std::map<std::string, ConfigStructure> structures;
    std::map<std::string, SectionLocation> result;
    for (const auto& segment : read_order) {
        const std::string& content = files.at(segment.file);
        auto parsed = structures.find(segment.file);
        if (parsed == structures.end()) {
            parsed = structures.emplace(segment.file, parse_structure(content)).first;
        }
        const ConfigStructure& structure = parsed->second;

        int line_index =
            static_cast<int>(std::count(content.begin(), content.begin() + segment.begin, '\n'));
        const std::string text = content.substr(segment.begin, segment.end - segment.begin);
        for (std::string_view line : helix::text_io::lines(text)) {
            const int index = line_index++;
            // Everything from the SAVE_CONFIG line down belongs to Klipper.
            if (structure.save_config_line >= 0 && index >= structure.save_config_line)
                break;
            if (line.empty() || line[0] != '[')
                continue;
            const size_t close = line.find(']');
            if (close == std::string_view::npos)
                continue;
            auto section = structure.sections.find(std::string(line.substr(1, close - 1)));
            if (section != structure.sections.end()) {
                result[section->first] = SectionLocation{segment.file, section->second};
            }
        }
    }
    return result;
}

// Moonraker Integration — Async file operations

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

void KlipperConfigEditor::load_config_files(IMoonrakerAPI& api, SectionMapCallback on_complete,
                                            ErrorCallback on_error) {
    spdlog::info("[ConfigEditor] Loading config files from printer");

    // Listing and downloading is delegated to resolve_active_config_files_with_content():
    // it lists the directories the includes name, which is the only way a glob
    // include ([include conf.d/*.cfg]) can be followed — the pattern names files we
    // do not know about until Moonraker lists them — and downloads the files the
    // include chain from printer.cfg reaches. resolve_includes() then does the pure
    // section -> file mapping over that content, globs included.
    resolve_active_config_files_with_content(
        api,
        [this, on_complete](const std::set<std::string>& active_files,
                            const std::map<std::string, std::string>& contents) {
            auto section_map = resolve_includes(contents, kRootConfigFile);

            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                file_cache_ = contents;
                section_map_ = section_map;
            }

            spdlog::info("[ConfigEditor] Resolved {} sections across {} active files "
                         "({} downloaded)",
                         section_map.size(), active_files.size(), contents.size());

            if (on_complete)
                on_complete(section_map);
        },
        [on_error](const std::string& err) {
            spdlog::error("[ConfigEditor] Failed to load config files: {}", err);
            if (on_error)
                on_error(err);
        });
}

namespace {

constexpr const char* kBackupSuffix = ".helix_backup";

/// Create each directory of @p dirs in turn, then run @p done. A directory
/// that already exists answers with an error, which is what most of these are.
void create_config_dirs(IMoonrakerAPI& api, std::vector<std::string> dirs, std::size_t index,
                        helix::SharedCallback<void()> done) {
    if (index >= dirs.size()) {
        done();
        return;
    }
    const std::string dir = "config/" + dirs[index];
    auto next = [&api, dirs, index, done]() { create_config_dirs(api, dirs, index + 1, done); };
    api.files().create_directory(dir, next, [next](const MoonrakerError&) { next(); });
}

} // namespace

std::string config_backup_path(const std::string& file) {
    return std::string(kConfigBackupDir) + "/" + file + kBackupSuffix;
}

std::optional<std::string> config_backup_original(const std::string& path) {
    const std::string prefix = std::string(kConfigBackupDir) + "/";
    const std::string suffix = kBackupSuffix;
    if (path.size() <= prefix.size() + suffix.size() || path.compare(0, prefix.size(), prefix) ||
        path.compare(path.size() - suffix.size(), suffix.size(), suffix)) {
        return std::nullopt;
    }
    return path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
}

void KlipperConfigEditor::backup_file(IMoonrakerAPI& api, const std::string& file_path,
                                      SuccessCallback on_success, ErrorCallback on_error) {
    const std::string backup = config_backup_path(file_path);
    std::string source = "config/" + file_path;
    std::string dest = "config/" + backup;

    // Moonraker copies into an existing directory only.
    std::vector<std::string> dirs;
    for (size_t slash = backup.find('/'); slash != std::string::npos;
         slash = backup.find('/', slash + 1)) {
        dirs.push_back(backup.substr(0, slash));
    }

    spdlog::info("[ConfigEditor] Creating backup: {} -> {}", source, dest);

    create_config_dirs(
        api, std::move(dirs), 0, [&api, source, dest, file_path, on_success, on_error]() {
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
        });
}

void KlipperConfigEditor::edit_value(IMoonrakerAPI& api, const std::string& section,
                                     const std::string& key, const std::string& new_value,
                                     SuccessCallback on_success, ErrorCallback on_error) {
    if (refuse_while_restart_blocked(section, on_error)) {
        return;
    }
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
            std::vector<std::pair<std::string, std::string>> backup_files;
            for (const auto& f : files) {
                std::string path = f.path.empty() ? f.filename : f.path;
                if (auto original = config_backup_original(path)) {
                    backup_files.emplace_back(path, *original);
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

            for (const auto& entry : backup_files) {
                const std::string& backup_path = entry.first;
                const std::string& original = entry.second;
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
            std::vector<std::string> backup_files;
            for (const auto& f : files) {
                std::string path = f.path.empty() ? f.filename : f.path;
                if (config_backup_original(path)) {
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
    std::string current = content;

    auto structure = parse_structure(content);
    if (structure.sections.find(section) == structure.sections.end()) {
        // A printer that has never been calibrated has no [input_shaper] at all,
        // so an edit list made purely of ADD_KEY ("make sure this key says X")
        // creates the section. SET_VALUE and REMOVE_KEY name something that is
        // supposed to already be there, so a missing section stays an error for
        // them — inventing a section to satisfy a SET_VALUE would hide a typo in
        // the section name.
        bool all_add_key = !edits.empty();
        for (const auto& edit : edits) {
            if (edit.type != ConfigEdit::Type::ADD_KEY) {
                all_add_key = false;
                break;
            }
        }

        if (!all_add_key) {
            spdlog::debug("[ConfigEditor] apply_edits: section [{}] not found", section);
            return std::nullopt;
        }

        auto created = add_section(current, section);
        if (!created.has_value()) {
            spdlog::error("[ConfigEditor] apply_edits: failed to create section [{}]", section);
            return std::nullopt;
        }
        spdlog::info("[ConfigEditor] apply_edits: created missing section [{}]", section);
        current = *created;
    }

    // Empty edits = no-op success
    if (edits.empty()) {
        return current;
    }

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

        case ConfigEdit::Type::ADD_IF_ABSENT:
            if (parse_structure(current).find_key(section, edit.key).has_value()) {
                spdlog::debug("[ConfigEditor] apply_edits: [{}] {} already set, kept", section,
                              edit.key);
                break;
            }
            result = add_key(current, section, edit.key, edit.value);
            if (!result.has_value()) {
                spdlog::error("[ConfigEditor] apply_edits: ADD_IF_ABSENT failed for [{}] {}",
                              section, edit.key);
                return std::nullopt;
            }
            current = *result;
            break;

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

namespace {

/// The root file's SAVE_CONFIG block, parsed as the config Klipper reads from it.
ConfigStructure autosave_structure(const std::string& root_content) {
    const int start = KlipperConfigEditor::parse_structure(root_content).save_config_line;
    if (start < 0)
        return {};
    std::string block;
    int index = 0;
    for (std::string_view line : helix::text_io::lines(root_content)) {
        if (index++ <= start || line.substr(0, 3) != "#*#")
            continue;
        block += std::string(line.size() > 4 ? line.substr(4) : std::string_view()) + "\n";
    }
    return KlipperConfigEditor::parse_structure(block);
}

} // namespace

std::string config_edit_target_file(const std::map<std::string, SectionLocation>& section_map,
                                    const std::string& root_content, const std::string& section,
                                    const std::vector<ConfigEdit>& edits) {
    auto it = section_map.find(section);
    if (it == section_map.end()) {
        // A printer that has never run this calibration has no such section
        // anywhere. apply_edits() creates it for an all-ADD_KEY list ("make sure
        // this key says X") in the root, the one file that always exists and is
        // always active.
        return all_add_key(edits) ? kRootConfigFile : "";
    }
    const SectionLocation& location = it->second;
    if (location.file_path == kRootConfigFile)
        return location.file_path;
    const ConfigStructure autosave = autosave_structure(root_content);
    for (const auto& edit : edits) {
        if (edit.type != ConfigEdit::Type::ADD_KEY)
            continue;
        const bool in_file =
            std::any_of(location.section.keys.begin(), location.section.keys.end(),
                        [&](const ConfigKey& key) { return key.name == edit.key; });
        if (!in_file && autosave.find_key(section, edit.key)) {
            spdlog::info("[ConfigEditor] [{}] {} lives in SAVE_CONFIG; writing to {}", section,
                         edit.key, kRootConfigFile);
            return kRootConfigFile;
        }
    }
    return location.file_path;
}

// Routing between the plugin and the file API

ConfigEditRoute config_edit_route(PluginFeatureState state) {
    return state == PluginFeatureState::Ready ? ConfigEditRoute::Plugin : ConfigEditRoute::FileApi;
}

PluginConfigEditPlan plan_plugin_config_edits(const std::string& section,
                                              const std::vector<ConfigEdit>& edits,
                                              const nlohmann::json& config) {
    using helix::json_util::find_member;
    PluginConfigEditPlan plan;
    if (edits.empty()) {
        plan.kind = PluginConfigEditPlan::Kind::FileApi;
        return plan;
    }

    const nlohmann::json* sections = find_member(config, "sections");
    const nlohmann::json* entry =
        sections != nullptr ? find_member(*sections, section.c_str()) : nullptr;
    bool regular = false;
    if (const nlohmann::json* defined = entry ? find_member(*entry, "defined_in") : nullptr;
        defined != nullptr && defined->is_array()) {
        for (const auto& d : *defined) {
            regular = regular || !helix::json_util::safe_bool(d, "autosave", false);
        }
    }
    if (!regular && !all_add_key(edits)) {
        plan.error = "Section [" + section + "] not found in config";
        return plan;
    }

    // Each key's value as the list leaves it, so a key edited twice expects
    // its own earlier edit.
    std::map<std::string, std::optional<std::string>> current;
    const nlohmann::json* options = entry ? find_member(*entry, "options") : nullptr;
    for (const auto& edit : edits) {
        if (current.count(edit.key))
            continue;
        const nlohmann::json* option =
            options != nullptr ? find_member(*options, edit.key.c_str()) : nullptr;
        if (option == nullptr || !option->is_object()) {
            current[edit.key] = std::nullopt;
            continue;
        }
        if (helix::json_util::safe_bool(*option, "autosave", false)) {
            // The plugin refuses the SAVE_CONFIG block; the file route writes
            // a regular definition, which Klipper reads instead of the block's.
            plan.kind = PluginConfigEditPlan::Kind::FileApi;
            return plan;
        }
        current[edit.key] = helix::json_util::safe_string(*option, "value");
    }

    for (const auto& edit : edits) {
        auto& value = current[edit.key];
        const nlohmann::json expect = value ? nlohmann::json(*value) : nlohmann::json(nullptr);
        switch (edit.type) {
        case ConfigEdit::Type::SET_VALUE:
            if (!value) {
                plan.error = "Failed to apply edits to [" + section + "]";
                return plan;
            }
            [[fallthrough]];
        case ConfigEdit::Type::ADD_KEY:
            plan.edits.push_back({{"section", section},
                                  {"option", edit.key},
                                  {"action", "set"},
                                  {"value", edit.value},
                                  {"expect", expect}});
            value = edit.value;
            break;
        case ConfigEdit::Type::ADD_IF_ABSENT:
            if (value)
                break;
            plan.edits.push_back({{"section", section},
                                  {"option", edit.key},
                                  {"action", "set"},
                                  {"value", edit.value},
                                  {"expect", expect}});
            value = edit.value;
            break;
        case ConfigEdit::Type::REMOVE_KEY:
            if (!value)
                break;
            plan.edits.push_back({{"section", section},
                                  {"option", edit.key},
                                  {"action", "remove"},
                                  {"expect", expect}});
            value = std::nullopt;
            break;
        }
    }
    plan.kind = PluginConfigEditPlan::Kind::Edits;
    return plan;
}

std::vector<std::pair<std::string, std::string>>
plugin_config_edit_backups(const nlohmann::json& result) {
    std::vector<std::pair<std::string, std::string>> backups;
    const nlohmann::json* list = helix::json_util::find_member(result, "backups");
    if (list == nullptr || !list->is_array())
        return backups;
    for (const auto& entry : *list) {
        std::string backup = helix::json_util::safe_string(entry, "backup");
        std::string file = helix::json_util::safe_string(entry, "file");
        if (!backup.empty() && !file.empty())
            backups.emplace_back(std::move(backup), std::move(file));
    }
    return backups;
}

KlipperConfigEditor::KlipperConfigEditor()
    : feature_state_source_([]() {
          if (!helix::ui::UpdateQueue::is_main_thread()) {
              spdlog::warn("[ConfigEditor] Feature state read off the main thread; using the "
                           "file API");
              return PluginFeatureState::Absent;
          }
          return get_printer_state().plugin_status_state().plugin_feature(
              kConfigEditFeature, kConfigEditFeatureMinVersion);
      }),
      restart_refusal_source_([]() -> const char* {
          if (!helix::ui::UpdateQueue::is_main_thread()) {
              spdlog::warn("[ConfigEditor] Restart check off the main thread; refusing");
              return helix::restart_while_printing_message();
          }
          return helix::api::restart_refusal_reason(&get_printer_state());
      }),
      lifetime_token_(lifetime_.token()) {}

void KlipperConfigEditor::set_restart_refusal_source(RestartRefusalSource source) {
    restart_refusal_source_ = std::move(source);
}

bool KlipperConfigEditor::refuse_while_restart_blocked(const std::string& section,
                                                       const ErrorCallback& on_error) const {
    const char* reason = restart_refusal_source_ ? restart_refusal_source_() : nullptr;
    if (!reason) {
        return false;
    }
    spdlog::warn("[ConfigEditor] Not editing [{}]: {}", section, reason);
    if (on_error) {
        on_error(lv_tr(reason));
    }
    return true;
}

void KlipperConfigEditor::set_feature_state_source(FeatureStateSource source) {
    feature_state_source_ = std::move(source);
}

ConfigEditRoute KlipperConfigEditor::route() const {
    return config_edit_route(feature_state_source_ ? feature_state_source_()
                                                   : PluginFeatureState::Absent);
}

// Judging a FIRMWARE_RESTART

namespace {

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// A string compares ignoring case, a number by value, a bool by name.
bool setting_matches(const nlohmann::json& actual, const std::string& want) {
    if (actual.is_string())
        return lowercase(actual.get<std::string>()) == lowercase(want);
    if (actual.is_boolean())
        return std::string(actual.get<bool>() ? "true" : "false") == lowercase(want);
    if (actual.is_number()) {
        char* end = nullptr;
        const double parsed = std::strtod(want.c_str(), &end);
        if (end == want.c_str())
            return false;
        const double have = actual.get<double>();
        return std::fabs(have - parsed) <= 1e-9 * std::max(1.0, std::fabs(parsed));
    }
    return false;
}

} // namespace

bool settings_hold(const nlohmann::json& settings, const std::vector<ExpectedSetting>& expected,
                   std::string* mismatch) {
    for (const auto& want : expected) {
        const std::string section_key = lowercase(want.section);
        const std::string option_key = lowercase(want.option);
        const nlohmann::json* section =
            helix::json_util::find_member(settings, section_key.c_str());
        const nlohmann::json* value =
            (section != nullptr && section->is_object())
                ? helix::json_util::find_member(*section, option_key.c_str())
                : nullptr;
        if (value != nullptr && setting_matches(*value, want.value))
            continue;
        if (mismatch != nullptr) {
            const std::string have =
                value == nullptr
                    ? "unset"
                    : "'" + (value->is_string() ? value->get<std::string>() : value->dump()) + "'";
            *mismatch =
                fmt::format("[{}] {} is {}, not '{}'", want.section, want.option, have, want.value);
        }
        return false;
    }
    return true;
}

RestartWatch restart_watch_for(int restart_timeout_ms) {
    RestartWatch watch;
    watch.timeout_ms = std::max(restart_timeout_ms, 1);
    // A third of the window: never so long that a short test timeout runs out
    // before a lingering pre-restart state may count.
    watch.settle_ms = std::min(2000, watch.timeout_ms / 3);
    watch.startup_allowance_ms = watch.timeout_ms;
    return watch;
}

RestartVerdict judge_restart_poll(RestartWatch& watch, std::optional<KlippyState> state,
                                  int elapsed_ms) {
    const bool settled = watch.saw_restart || elapsed_ms >= watch.settle_ms;
    if (!state || *state == KlippyState::STARTUP) {
        watch.saw_restart = true;
        return elapsed_ms < watch.timeout_ms + watch.startup_allowance_ms ? RestartVerdict::Waiting
                                                                          : RestartVerdict::Failed;
    }
    if (*state == KlippyState::READY && settled)
        return RestartVerdict::Ready;
    if (*state == KlippyState::ERROR && settled)
        return RestartVerdict::Failed;
    // A pre-restart READY or ERROR still lingering, or a SHUTDOWN: wait for
    // the restart to show, up to the timeout.
    return elapsed_ms < watch.timeout_ms ? RestartVerdict::Waiting : RestartVerdict::Failed;
}

namespace {

/// How long one poll waits for Moonraker's answer before it counts as none.
constexpr auto kPollAnswerTimeout = std::chrono::seconds(2);

int elapsed_ms_since(std::chrono::steady_clock::time_point start) {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
}

struct PolledKlippy {
    std::optional<KlippyState> state; ///< nullopt: no Klippy, or no answer
    std::string state_text;
    std::string message;
};

/// printer.info, waited for on the calling (monitor) thread.
PolledKlippy poll_klippy(IMoonrakerAPI& api) {
    auto answer = std::make_shared<std::promise<KlippyStatus>>();
    auto answered = std::make_shared<std::atomic<bool>>(false);
    auto future = answer->get_future();
    api.get_klippy_status(
        [answer, answered](const KlippyStatus& status) {
            if (!answered->exchange(true))
                answer->set_value(status);
        },
        [answer, answered](const MoonrakerError& err) {
            if (!answered->exchange(true))
                answer->set_value(KlippyStatus{"unreachable", err.message});
        });
    if (future.wait_for(kPollAnswerTimeout) != std::future_status::ready)
        return {std::nullopt, "no answer", ""};
    const KlippyStatus status = future.get();
    return {parse_klippy_state(status.state), status.state, status.state_message};
}

/// configfile.settings, waited for on the calling (monitor) thread; nullopt
/// when Moonraker gives none in time.
std::optional<nlohmann::json> query_config_settings(IMoonrakerAPI& api) {
    auto answer = std::make_shared<std::promise<std::optional<nlohmann::json>>>();
    auto answered = std::make_shared<std::atomic<bool>>(false);
    auto future = answer->get_future();
    api.get_client().send_jsonrpc(
        "printer.objects.query",
        nlohmann::json{{"objects", {{"configfile", nlohmann::json::array({"settings"})}}}},
        [answer, answered](const nlohmann::json& response) {
            if (answered->exchange(true))
                return;
            const nlohmann::json* result = helix::json_util::find_member(response, "result");
            const nlohmann::json* status =
                result ? helix::json_util::find_member(*result, "status") : nullptr;
            const nlohmann::json* configfile =
                status ? helix::json_util::find_member(*status, "configfile") : nullptr;
            const nlohmann::json* settings =
                configfile ? helix::json_util::find_member(*configfile, "settings") : nullptr;
            answer->set_value(settings ? std::optional<nlohmann::json>(*settings) : std::nullopt);
        },
        [answer, answered](const MoonrakerError&) {
            if (!answered->exchange(true))
                answer->set_value(std::nullopt);
        },
        0, /*silent=*/true);
    if (future.wait_for(kPollAnswerTimeout) != std::future_status::ready)
        return std::nullopt;
    return future.get();
}

} // namespace

namespace {

/// Every FIRMWARE_RESTART this editor sends is one the app asked for, so the
/// recovery dialog and the disconnect modal must read it as a restart, not a
/// fault. Owned here rather than by each caller, so no Save path can forget it.
/// @p message is the toast's untranslated text; empty shows the generic one.
void announce_expected_restart(IMoonrakerAPI& api, const std::string& message = {}) {
    helix::publish_ui_request({helix::UiRequestKind::ExpectKlippyRestart, -1, &api, 0, message});
}

} // namespace

void KlipperConfigEditor::revert_after_failed_restart(IMoonrakerAPI& api,
                                                      const RestartRecovery& recovery,
                                                      ErrorCallback on_error,
                                                      const std::string& problem) {
    recovery.restore(
        api,
        [&api, on_error, problem]() {
            spdlog::info("[ConfigEditor] Backups restored, sending recovery FIRMWARE_RESTART");
            announce_expected_restart(api);
            api.restart_firmware(
                [on_error, problem]() {
                    if (on_error)
                        on_error(problem + ". Original config restored.");
                },
                [on_error, problem](const MoonrakerError& err) {
                    spdlog::error("[ConfigEditor] Recovery FIRMWARE_RESTART failed: {}",
                                  err.message);
                    if (on_error)
                        on_error(problem + ". Backups restored but restart failed: " + err.message);
                });
        },
        [on_error, problem](const std::string& restore_err) {
            spdlog::error("[ConfigEditor] Failed to restore backups: {}", restore_err);
            if (on_error)
                on_error(problem + ", AND backup restore failed: " + restore_err);
        });
}

// Safe edits: write, FIRMWARE_RESTART, revert if Klipper does not come back
namespace {
/// True, after reporting it, when firmware that supervises HelixScreen owns the Klipper
/// config (#2154). Every edit entry asks before choosing a route, so neither the file
/// route nor the HelixPrint plugin's config_edit writes it.
bool refuse_while_supervised(const std::string& section,
                             const KlipperConfigEditor::ErrorCallback& on_error) {
    if (!helix::firmware_supervised()) {
        return false;
    }
    spdlog::warn("[ConfigEditor] Not editing [{}]: {}", section,
                 helix::kFirmwareSupervisedConfigRefusal);
    if (on_error) {
        on_error(helix::kFirmwareSupervisedConfigRefusal);
    }
    return true;
}
} // namespace

void KlipperConfigEditor::safe_multi_edit(IMoonrakerAPI& api, const std::string& section,
                                          const std::vector<ConfigEdit>& edits,
                                          SuccessCallback on_success, ErrorCallback on_error,
                                          int restart_timeout_ms,
                                          std::vector<ExpectedSetting> expected) {
    spdlog::info("[ConfigEditor] Starting safe multi-edit on [{}] ({} edits)", section,
                 edits.size());
    if (refuse_while_supervised(section, on_error) ||
        refuse_while_restart_blocked(section, on_error)) {
        return;
    }
    if (route() == ConfigEditRoute::Plugin) {
        plugin_multi_edit(api, section, edits, std::move(on_success), std::move(on_error),
                          restart_timeout_ms, std::move(expected));
        return;
    }
    file_api_multi_edit(api, section, edits, std::move(on_success), std::move(on_error),
                        restart_timeout_ms, std::move(expected));
}

void KlipperConfigEditor::plugin_multi_edit(IMoonrakerAPI& api, const std::string& section,
                                            const std::vector<ConfigEdit>& edits,
                                            SuccessCallback on_success, ErrorCallback on_error,
                                            int restart_timeout_ms,
                                            std::vector<ExpectedSetting> expected) {
    if (refuse_while_supervised(section, on_error) ||
        refuse_while_restart_blocked(section, on_error)) {
        return;
    }
    spdlog::info("[ConfigEditor] Editing [{}] through the HelixPrint plugin", section);
    api.get_client().send_jsonrpc(
        "server.helix.config", nlohmann::json{{"section", section}},
        [this, &api, section, edits, on_success, on_error, restart_timeout_ms,
         expected](const nlohmann::json& response) {
            const nlohmann::json* result = helix::json_util::find_member(response, "result");
            auto plan = plan_plugin_config_edits(section, edits,
                                                 result ? *result : nlohmann::json::object());
            if (plan.kind == PluginConfigEditPlan::Kind::FileApi) {
                spdlog::info("[ConfigEditor] [{}] edit served by the file API", section);
                file_api_multi_edit(api, section, edits, on_success, on_error, restart_timeout_ms,
                                    expected);
                return;
            }
            if (plan.kind == PluginConfigEditPlan::Kind::Error) {
                spdlog::error("[ConfigEditor] {}", plan.error);
                if (on_error)
                    on_error(plan.error);
                return;
            }
            api.get_client().send_jsonrpc(
                "server.helix.config_edit",
                nlohmann::json{{"edits", plan.edits}, {"dry_run", false}},
                [this, &api, on_success, on_error, restart_timeout_ms,
                 expected](const nlohmann::json& edit_response) {
                    const nlohmann::json* edit_result =
                        helix::json_util::find_member(edit_response, "result");
                    auto backups = plugin_config_edit_backups(
                        edit_result ? *edit_result : nlohmann::json::object());
                    spdlog::info("[ConfigEditor] Plugin wrote {} file(s), sending "
                                 "FIRMWARE_RESTART",
                                 backups.size());
                    RestartRecovery recovery;
                    recovery.restore = [backups](IMoonrakerAPI& a, SuccessCallback done,
                                                 ErrorCallback fail) {
                        restore_plugin_backups(a, backups, std::move(done), std::move(fail));
                    };
                    // The plugin keeps its timestamped backups and prunes them itself.
                    recovery.cleanup = [](IMoonrakerAPI&, SuccessCallback done) {
                        if (done)
                            done();
                    };
                    restart_and_monitor(api, std::move(recovery), on_success, on_error,
                                        restart_timeout_ms, expected);
                },
                [on_error](const MoonrakerError& err) {
                    spdlog::error("[ConfigEditor] Plugin config edit failed: {}", err.message);
                    if (on_error)
                        on_error("Config edit refused: " + err.message);
                });
        },
        [this, &api, section, edits, on_success, on_error, restart_timeout_ms,
         expected](const MoonrakerError& err) {
            // Nothing is written yet, so the file API can still serve the edit.
            spdlog::warn("[ConfigEditor] Plugin config read failed ({}); using the file API",
                         err.message);
            file_api_multi_edit(api, section, edits, on_success, on_error, restart_timeout_ms,
                                expected);
        });
}

void KlipperConfigEditor::file_api_multi_edit(IMoonrakerAPI& api, const std::string& section,
                                              const std::vector<ConfigEdit>& edits,
                                              SuccessCallback on_success, ErrorCallback on_error,
                                              int restart_timeout_ms,
                                              std::vector<ExpectedSetting> expected) {
    // Step 1: Load config files to populate cache and section map
    load_config_files(
        api,
        [this, &api, section, edits, on_success, on_error, restart_timeout_ms,
         expected](std::map<std::string, SectionLocation> section_map) {
            // Step 2: Find which file the edits belong in
            const std::string file_path = config_edit_target_file(
                section_map, get_cached_file(kRootConfigFile).value_or(""), section, edits);
            if (file_path.empty()) {
                // SET_VALUE / REMOVE_KEY name something that is supposed to
                // already be there, so a missing section stays an error rather
                // than inventing one to satisfy a typo'd section name.
                spdlog::error("[ConfigEditor] Section [{}] not found in config", section);
                if (on_error)
                    on_error("Section [" + section + "] not found in config");
                return;
            }
            spdlog::debug("[ConfigEditor] Editing [{}] in {}", section, file_path);

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
                [this, &api, section, file_path, modified, on_success, on_error, restart_timeout_ms,
                 expected]() {
                    // Step 6: Upload modified content
                    api.transfers().upload_file(
                        "config", file_path, *modified,
                        [this, &api, file_path, modified, on_success, on_error, restart_timeout_ms,
                         expected]() {
                            {
                                std::lock_guard<std::mutex> lock(cache_mutex_);
                                file_cache_[file_path] = *modified;
                            }
                            spdlog::info("[ConfigEditor] Multi-edit written, sending "
                                         "FIRMWARE_RESTART");
                            restart_and_monitor(api, file_api_recovery(), on_success, on_error,
                                                restart_timeout_ms, expected);
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

KlipperConfigEditor::RestartRecovery KlipperConfigEditor::file_api_recovery() {
    RestartRecovery recovery;
    recovery.restore = [this](IMoonrakerAPI& api, SuccessCallback done, ErrorCallback fail) {
        restore_backups(api, std::move(done), std::move(fail));
    };
    recovery.cleanup = [this](IMoonrakerAPI& api, SuccessCallback done) {
        cleanup_backups(api, std::move(done));
    };
    return recovery;
}

void KlipperConfigEditor::restore_plugin_backups(
    IMoonrakerAPI& api, const std::vector<std::pair<std::string, std::string>>& backups,
    SuccessCallback on_complete, ErrorCallback on_error) {
    if (backups.empty()) {
        if (on_complete)
            on_complete();
        return;
    }
    auto pending = std::make_shared<std::atomic<int>>(static_cast<int>(backups.size()));
    auto had_error = std::make_shared<std::atomic<bool>>(false);
    auto finish = [pending, had_error, on_complete, on_error]() {
        if (pending->fetch_sub(1) - 1 != 0)
            return;
        if (had_error->load()) {
            if (on_error)
                on_error("Failed to restore one or more backup files");
        } else if (on_complete) {
            on_complete();
        }
    };
    for (const auto& entry : backups) {
        const std::string& backup = entry.first;
        spdlog::info("[ConfigEditor] Restoring {} -> {}", backup, entry.second);
        api.files().copy_file(
            "config/" + backup, "config/" + entry.second, [finish]() { finish(); },
            [finish, had_error, backup](const MoonrakerError& err) {
                spdlog::error("[ConfigEditor] Failed to restore {}: {}", backup, err.message);
                had_error->store(true);
                finish();
            });
    }
}

void KlipperConfigEditor::restart_and_monitor(IMoonrakerAPI& api, RestartRecovery recovery,
                                              SuccessCallback on_success, ErrorCallback on_error,
                                              int restart_timeout_ms,
                                              std::vector<ExpectedSetting> expected) {
    // The write callbacks run on a worker thread; PrinterState and lv_tr belong to
    // the main thread. The last check before the restart is made there, so a print
    // that started since the entry check puts the edit back instead of being killed.
    // The editor's owner is long-lived. One that dies before this queued lambda runs
    // leaves the written file with neither a restart nor a restore.
    lifetime_token_.defer(
        "KlipperConfigEditor::restart",
        [this, &api, recovery = std::move(recovery), on_success = std::move(on_success),
         on_error = std::move(on_error), restart_timeout_ms,
         expected = std::move(expected)]() mutable {
            const char* reason = restart_refusal_source_ ? restart_refusal_source_() : nullptr;
            if (reason == nullptr) {
                send_restart_and_monitor(api, std::move(recovery), std::move(on_success),
                                         std::move(on_error), restart_timeout_ms,
                                         std::move(expected));
                return;
            }
            spdlog::warn("[ConfigEditor] Restart refused ({}); restoring backups", reason);
            const std::string refusal = lv_tr(reason);
            auto report = [on_error, refusal](const std::string& detail) {
                if (on_error)
                    on_error(detail.empty() ? refusal : refusal + " " + detail);
            };
            if (recovery.restore) {
                recovery.restore(
                    api, [report]() { report({}); },
                    [report](const std::string& restore_err) {
                        report("The edit could not be undone: " + restore_err);
                    });
            } else {
                report({});
            }
        });
}

void KlipperConfigEditor::send_restart_and_monitor(IMoonrakerAPI& api, RestartRecovery recovery,
                                                   SuccessCallback on_success,
                                                   ErrorCallback on_error, int restart_timeout_ms,
                                                   std::vector<ExpectedSetting> expected) {
    announce_expected_restart(api, restart_announcement_);
    api.restart_firmware(
        [this, &api, recovery, on_success, on_error, restart_timeout_ms, expected,
         poll_interval = restart_poll_interval_]() {
            // HttpExecutor::fast(), never a raw std::thread: a spawn failure is
            // std::terminate on AD5M (#724, #837). The future goes to
            // track_restart_monitor() so ~KlipperConfigEditor() can wait this
            // loop out: it holds `this` and `api` by reference and outlives neither.
            track_restart_monitor(helix::http::HttpExecutor::fast().submit(
                [&api, recovery, on_success, on_error, restart_timeout_ms, expected, poll_interval,
                 cancel = monitor_cancel_]() {
                    const auto start = std::chrono::steady_clock::now();
                    RestartWatch watch = restart_watch_for(restart_timeout_ms);
                    std::string problem;
                    std::string last_state = "no answer";

                    while (true) {
                        if (cancel->load()) {
                            spdlog::info("[ConfigEditor] Editor going away — abandoning "
                                         "restart monitor");
                            return; // Touch nothing: not api, not this, not the callbacks.
                        }
                        const PolledKlippy polled = poll_klippy(api);
                        if (cancel->load())
                            return;
                        const int elapsed = elapsed_ms_since(start);
                        last_state = polled.state_text;
                        RestartVerdict verdict = judge_restart_poll(watch, polled.state, elapsed);
                        if (polled.state == KlippyState::ERROR && !polled.message.empty())
                            problem = "Klipper reported: " + polled.message;

                        if (verdict == RestartVerdict::Ready) {
                            if (expected.empty()) {
                                spdlog::info("[ConfigEditor] Klippy ready {}ms after "
                                             "FIRMWARE_RESTART",
                                             elapsed);
                                break;
                            }
                            const auto settings = query_config_settings(api);
                            std::string mismatch;
                            if (settings && settings_hold(*settings, expected, &mismatch)) {
                                spdlog::info("[ConfigEditor] Klippy ready {}ms after "
                                             "FIRMWARE_RESTART with the new settings",
                                             elapsed);
                                break;
                            }
                            problem = settings ? "Klipper restarted, but " + mismatch +
                                                     ". A SAVE_CONFIG value in printer.cfg may "
                                                     "override it."
                                               : "Klipper restarted, but its settings could "
                                                 "not be read";
                            spdlog::debug("[ConfigEditor] {} ({}ms)", problem, elapsed);
                            verdict = elapsed >= watch.timeout_ms ? RestartVerdict::Failed
                                                                  : RestartVerdict::Waiting;
                        }

                        if (verdict == RestartVerdict::Failed) {
                            if (problem.empty())
                                problem = fmt::format("Klipper was not ready {}s after the "
                                                      "restart (state '{}')",
                                                      elapsed / 1000, last_state);
                            spdlog::error("[ConfigEditor] Config edit failed after "
                                          "FIRMWARE_RESTART ({}ms): {}; reverting",
                                          elapsed, problem);
                            revert_after_failed_restart(api, recovery, on_error, problem);
                            return;
                        }
                        std::this_thread::sleep_for(poll_interval);
                    }

                    recovery.cleanup(api, [on_success]() {
                        spdlog::info("[ConfigEditor] Safe edit complete");
                        if (on_success)
                            on_success();
                    });
                }));
        },
        [on_error](const MoonrakerError& err) {
            spdlog::error("[ConfigEditor] FIRMWARE_RESTART failed: {}", err.message);
            if (on_error)
                on_error("Failed to send FIRMWARE_RESTART: " + err.message);
        });
}

void KlipperConfigEditor::safe_edit_value(IMoonrakerAPI& api, const std::string& section,
                                          const std::string& key, const std::string& new_value,
                                          SuccessCallback on_success, ErrorCallback on_error,
                                          int restart_timeout_ms) {
    spdlog::info("[ConfigEditor] Starting safe edit: [{}] {} = {}", section, key, new_value);
    if (refuse_while_supervised(section, on_error) ||
        refuse_while_restart_blocked(section, on_error)) {
        return;
    }
    if (route() == ConfigEditRoute::Plugin) {
        plugin_multi_edit(api, section, {{ConfigEdit::Type::SET_VALUE, key, new_value}},
                          std::move(on_success), std::move(on_error), restart_timeout_ms, {});
        return;
    }

    // Step 1: Apply the edit (backup + write)
    edit_value(
        api, section, key, new_value,
        [this, &api, on_success, on_error, restart_timeout_ms]() {
            // Step 2: Edit succeeded, send FIRMWARE_RESTART and watch it
            spdlog::info("[ConfigEditor] Edit written, sending FIRMWARE_RESTART");
            restart_and_monitor(api, file_api_recovery(), on_success, on_error, restart_timeout_ms,
                                {});
        },
        on_error);
}

} // namespace helix::system
