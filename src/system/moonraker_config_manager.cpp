// SPDX-License-Identifier: GPL-3.0-or-later
#include "moonraker_config_manager.h"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

namespace helix {

// Trim leading and trailing whitespace (spaces, tabs, carriage returns)
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool MoonrakerConfigManager::has_section(const std::string& content,
                                         const std::string& section_name) {
    const std::string target = "[" + section_name + "]";
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#')
            continue;
        if (t == target)
            return true;
    }
    return false;
}

std::vector<std::string> MoonrakerConfigManager::list_sections(const std::string& content) {
    std::vector<std::string> sections;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        std::string t = trim(line);
        if (t.size() < 2 || t[0] != '[' || t.back() != ']')
            continue;
        std::string name = trim(t.substr(1, t.size() - 2));
        if (!name.empty())
            sections.push_back(name);
    }
    return sections;
}

int MoonrakerConfigManager::select_primary_config_index(
    const std::vector<LoadedConfigFile>& files) {
    // Moonraker's root config is the file defining [server].
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].filename.empty())
            continue;
        for (const auto& s : files[i].sections) {
            if (s == "server")
                return static_cast<int>(i);
        }
    }
    // No entry claimed [server] — fall back to the first usable entry, which is the
    // order Moonraker reports its config chain in.
    for (size_t i = 0; i < files.size(); ++i) {
        if (!files[i].filename.empty())
            return static_cast<int>(i);
    }
    return -1;
}

int MoonrakerConfigManager::select_root_config_index(const std::vector<LoadedConfigFile>& files) {
    // Moonraker records the file it was pointed at before descending into that
    // file's includes, so the chain always arrives root-first. Section contents
    // are deliberately not consulted: on COSMOS the root defines nothing at all,
    // and picking by [server] is precisely what selects the vendor file.
    for (size_t i = 0; i < files.size(); ++i) {
        if (!files[i].filename.empty())
            return static_cast<int>(i);
    }
    return -1;
}

std::vector<size_t>
MoonrakerConfigManager::find_files_defining_section(const std::vector<LoadedConfigFile>& files,
                                                    const std::string& section_name) {
    std::vector<size_t> hits;
    for (size_t i = 0; i < files.size(); ++i) {
        // A hit we cannot name is a hit we cannot address. Both index selectors above
        // skip these; returning one here would hand the caller a write target with no
        // path, and the in-place branch has no other source for one.
        if (files[i].filename.empty())
            continue;
        for (const auto& s : files[i].sections) {
            if (s == section_name) {
                hits.push_back(i);
                break;
            }
        }
    }
    return hits;
}

SectionMatchResult
MoonrakerConfigManager::classify_section_match(const std::string& content,
                                               const std::vector<std::string>& required) {
    SectionMatchResult r;
    r.total = required.size();
    for (const auto& name : required) {
        if (has_section(content, name))
            ++r.matched;
        else
            r.missing.push_back(name);
    }
    // An empty required list matches vacuously rather than dividing by zero.
    if (r.matched == r.total)
        r.verdict = SectionMatch::Match;
    else if (r.missing.size() <= drift_tolerance(r.total))
        r.verdict = SectionMatch::Drifted;
    else
        r.verdict = SectionMatch::Mismatch;
    return r;
}

ConfigPathInfo
MoonrakerConfigManager::config_path_from_relative(const std::string& filename,
                                                  const std::string& config_root_abs) {
    ConfigPathInfo info;
    std::string rel = trim(filename);

    if (rel.empty()) {
        info.error = "Moonraker did not report the name of its configuration file.";
        return info;
    }
    if (rel[0] == '/') {
        // Moonraker falls back to the full absolute path for any file outside the root
        // config's own directory. Such a file is still reachable when it happens to sit
        // under the file manager's config root, so strip that root and carry on through
        // the relative logic below — which keeps the `..` and subdir handling.
        std::string root = trim(config_root_abs);
        while (root.size() > 1 && root.back() == '/')
            root.pop_back();
        // The prefix must end on a path component boundary, or ".../config" would
        // swallow ".../config_backup/x.conf".
        const std::string prefix = (root == "/") ? root : root + "/";

        if (root.empty() || rel.compare(0, prefix.size(), prefix) != 0) {
            info.error = "Moonraker loads its config from the absolute path " + rel +
                         ", which is outside the writable config directory. HelixScreen cannot "
                         "edit it remotely — add the [spoolman] section by hand.";
            return info;
        }

        const std::string absolute = rel;
        rel = rel.substr(prefix.size());
        if (rel.empty()) {
            // The root directory itself, not a file inside it.
            info.error = "Moonraker reported a config path with no file name (" + absolute + ").";
            return info;
        }
    }
    // ".." would escape the config root; the file API rejects it and so do we.
    if (rel.find("..") != std::string::npos) {
        info.error = "Moonraker reported a config path outside the writable config "
                     "directory (" +
                     rel + "). Add the [spoolman] section by hand.";
        return info;
    }

    size_t last_slash = rel.rfind('/');
    if (last_slash == std::string::npos) {
        info.config_filename = rel;
    } else {
        info.upload_subdir = rel.substr(0, last_slash);
        info.config_filename = rel.substr(last_slash + 1);
    }

    if (info.config_filename.empty()) {
        info.error = "Moonraker reported a config path with no file name (" + rel + ").";
        info.upload_subdir.clear();
        return info;
    }

    info.uploadable = true;
    return info;
}

std::vector<std::string>
MoonrakerConfigManager::candidate_config_paths(const std::string& reported_filename,
                                               const std::string& config_root_abs) {
    std::vector<std::string> out;

    const std::string reported = trim(reported_filename);
    if (reported.empty())
        return out;
    // A traversal is discarded whole rather than sanitised: there is no reading of
    // ".." that we want to turn into a path we then write to.
    if (reported.find("..") != std::string::npos)
        return out;

    auto add = [&out](const std::string& candidate) {
        if (candidate.empty() || candidate.front() == '/')
            return;
        if (std::find(out.begin(), out.end(), candidate) == out.end())
            out.push_back(candidate);
    };

    // Case 1: already the relative form the file API wants.
    if (reported.front() != '/') {
        add(reported);
        return out;
    }

    // Case 2: absolute, and inside the file manager's config root.
    std::string root = trim(config_root_abs);
    while (root.size() > 1 && root.back() == '/')
        root.pop_back();

    std::string abs = reported;
    while (abs.size() > 1 && abs.back() == '/')
        abs.pop_back();
    if (!root.empty() && abs == root)
        return out; // the config root directory itself, not a file inside it

    if (!root.empty()) {
        // The prefix must end on a component boundary, or ".../config" would
        // swallow ".../config_backup/x.conf".
        const std::string prefix = (root == "/") ? root : root + "/";
        if (reported.compare(0, prefix.size(), prefix) == 0) {
            add(reported.substr(prefix.size()));
            return out;
        }
    }

    // Case 3: absolute and outside the root. Moonraker may still be naming a file
    // the file manager serves under a different tree (AD5M). The tail after the
    // last "config/" component is the strongest guess, the basename the weakest.
    const std::string marker = "/config/";
    const size_t last = reported.rfind(marker);
    if (last != std::string::npos)
        add(reported.substr(last + marker.size()));

    const size_t slash = reported.rfind('/');
    if (slash != std::string::npos)
        add(reported.substr(slash + 1));

    return out;
}

bool MoonrakerConfigManager::candidates_are_speculative(const std::string& reported_filename,
                                                        const std::string& config_root_abs) {
    const std::string reported = trim(reported_filename);

    // Nothing to grade: candidate_config_paths() returns an empty list for these, so
    // there is no candidate whose trustworthiness the caller could be asking about.
    if (reported.empty() || reported.find("..") != std::string::npos)
        return false;

    // Case 1: already relative — the file API takes it verbatim. Not a guess.
    if (reported.front() != '/')
        return false;

    std::string root = trim(config_root_abs);
    while (root.size() > 1 && root.back() == '/')
        root.pop_back();
    if (root.empty())
        return true; // no root to strip against, so the tail is all we have

    std::string abs = reported;
    while (abs.size() > 1 && abs.back() == '/')
        abs.pop_back();
    if (abs == root)
        return false; // the root directory itself yields no candidates at all

    // Case 2: the root demonstrably contains it — the strip is derived, not guessed.
    const std::string prefix = (root == "/") ? root : root + "/";
    return reported.compare(0, prefix.size(), prefix) != 0;
}

std::string
MoonrakerConfigManager::add_section(const std::string& content, const std::string& section_name,
                                    const std::vector<std::pair<std::string, std::string>>& entries,
                                    const std::string& comment) {
    if (has_section(content, section_name))
        return content;

    std::string result = content;
    // Ensure content ends with a newline before appending
    if (!result.empty() && result.back() != '\n')
        result += '\n';

    result += '\n';
    if (!comment.empty())
        result += "# " + comment + "\n";
    result += "[" + section_name + "]\n";
    for (const auto& [key, value] : entries) {
        result += key + ": " + value + "\n";
    }
    return result;
}

std::string MoonrakerConfigManager::upsert_section(
    const std::string& content, const std::string& section_name,
    const std::vector<std::pair<std::string, std::string>>& entries, const std::string& comment) {
    // Section absent: fall back to plain append (also handles empty content).
    if (!has_section(content, section_name))
        return add_section(content, section_name, entries, comment);

    const std::string target = "[" + section_name + "]";
    std::istringstream stream(content);
    std::string line;

    std::vector<std::string> out;
    std::vector<bool> applied(entries.size(), false);

    bool in_section = false;
    bool section_done = false;
    // Index in `out` immediately after the section's last key line. New keys are
    // inserted here so they land with the section's other keys rather than after
    // any trailing blank lines or comments.
    size_t insert_at = 0;

    auto insert_missing = [&]() {
        std::vector<std::string> additions;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!applied[i])
                additions.push_back(entries[i].first + ": " + entries[i].second);
        }
        if (!additions.empty()) {
            out.insert(out.begin() + static_cast<std::ptrdiff_t>(insert_at), additions.begin(),
                       additions.end());
        }
    };

    while (std::getline(stream, line)) {
        std::string t = trim(line);

        if (!in_section && !section_done && t == target) {
            in_section = true;
            out.push_back(line);
            insert_at = out.size();
            continue;
        }

        if (in_section) {
            // A new section header ends the target section.
            if (!t.empty() && t[0] == '[') {
                in_section = false;
                section_done = true;
                insert_missing();
                out.push_back(line);
                continue;
            }

            // Blank lines and comments are preserved but do not move the insert point.
            if (t.empty() || t[0] == '#') {
                out.push_back(line);
                continue;
            }

            size_t colon = t.find(':');
            if (colon != std::string::npos) {
                std::string key = trim(t.substr(0, colon));
                bool replaced = false;
                for (size_t i = 0; i < entries.size(); ++i) {
                    if (applied[i] || entries[i].first != key)
                        continue;
                    // Preserve the original line's leading indentation.
                    size_t first = line.find_first_not_of(" \t");
                    std::string lead = (first == std::string::npos) ? "" : line.substr(0, first);
                    out.push_back(lead + key + ": " + entries[i].second);
                    applied[i] = true;
                    replaced = true;
                    break;
                }
                if (replaced) {
                    insert_at = out.size();
                    continue;
                }
            }

            // Unrelated key (or a non key/value line) inside the section: keep as-is.
            out.push_back(line);
            insert_at = out.size();
            continue;
        }

        out.push_back(line);
    }

    // Section ran to end of file — append any keys we never saw.
    if (in_section)
        insert_missing();

    std::string result;
    for (const auto& l : out)
        result += l + "\n";
    return result;
}

ConfigPathInfo
MoonrakerConfigManager::resolve_config_upload_location(const std::string& config_file_abs,
                                                       const std::string& data_path) {
    ConfigPathInfo info;

    std::string config_file = trim(config_file_abs);
    std::string data = trim(data_path);

    if (config_file.empty()) {
        info.error = "Moonraker did not report the path of its configuration file.";
        return info;
    }
    if (data.empty()) {
        info.error = "Moonraker did not report its data path.";
        return info;
    }

    // Strip trailing slashes so "/x/" and "/x" compare equal.
    while (data.size() > 1 && data.back() == '/')
        data.pop_back();

    // Split the config file into directory + base name.
    size_t last_slash = config_file.rfind('/');
    std::string config_dir =
        (last_slash == std::string::npos) ? "" : config_file.substr(0, last_slash);
    info.config_filename =
        (last_slash == std::string::npos) ? config_file : config_file.substr(last_slash + 1);

    // The file API's "config" root maps to <data_path>/config.
    const std::string config_root = data + "/config";

    if (config_dir == config_root) {
        info.uploadable = true;
        return info;
    }

    const std::string config_root_prefix = config_root + "/";
    if (config_dir.rfind(config_root_prefix, 0) == 0) {
        info.uploadable = true;
        info.upload_subdir = config_dir.substr(config_root_prefix.size());
        return info;
    }

    info.error = "Moonraker loads its config from " + config_file +
                 ", which is outside the writable config directory (" + config_root +
                 "). HelixScreen cannot edit it remotely — add the [spoolman] section by hand.";
    return info;
}

std::string MoonrakerConfigManager::remove_section(const std::string& content,
                                                   const std::string& section_name) {
    if (!has_section(content, section_name))
        return content;

    const std::string target = "[" + section_name + "]";
    std::istringstream stream(content);
    std::string line;

    // Lines before target section (excluding its preceding comment block)
    std::vector<std::string> result_lines;
    // Pending lines that may be a comment block just before the target section
    std::vector<std::string> pending_comment;
    bool in_target = false;

    while (std::getline(stream, line)) {
        std::string t = trim(line);

        if (in_target) {
            // Skip lines until the next section header
            if (!t.empty() && t[0] == '[') {
                in_target = false;
                // Start collecting this new section
                pending_comment.clear();
                result_lines.push_back(line);
            }
            // else: skip this line (it belongs to the removed section)
            continue;
        }

        // Detect target section header
        if (t == target) {
            in_target = true;
            // Discard any pending comment block that preceded this section
            pending_comment.clear();
            continue;
        }

        // Track comment lines as potentially belonging to the next section
        if (t.empty() || t[0] == '#') {
            pending_comment.push_back(line);
        } else {
            // Non-comment, non-target line: flush pending comments into result
            for (const auto& cl : pending_comment)
                result_lines.push_back(cl);
            pending_comment.clear();
            result_lines.push_back(line);
        }
    }

    // If we never entered the target, just flush pending (shouldn't happen due to has_section
    // check)
    if (!in_target) {
        for (const auto& cl : pending_comment)
            result_lines.push_back(cl);
    }
    // If we ended inside the target, discard pending_comment (already cleared when entering target)

    // Build result string, stripping trailing blank lines
    while (!result_lines.empty() && trim(result_lines.back()).empty()) {
        result_lines.pop_back();
    }

    std::string result;
    for (const auto& l : result_lines) {
        result += l + "\n";
    }
    return result;
}

bool MoonrakerConfigManager::has_include_line(const std::string& moonraker_content,
                                              const std::string& include_target) {
    return has_section(moonraker_content, "include " + include_target);
}

std::string MoonrakerConfigManager::add_include_line(const std::string& moonraker_content,
                                                     const std::string& include_target) {
    if (has_include_line(moonraker_content, include_target))
        return moonraker_content;

    const std::string include_block = "[include " + include_target + "]\n\n";

    // Find the first non-comment section header and insert before it
    std::istringstream stream(moonraker_content);
    std::string line;
    size_t pos = 0;
    while (std::getline(stream, line)) {
        std::string t = trim(line);
        if (!t.empty() && t[0] == '[') {
            // Insert before this section header
            std::string result = moonraker_content.substr(0, pos);
            result += include_block;
            result += moonraker_content.substr(pos);
            return result;
        }
        pos += line.size() + 1; // +1 for '\n'
    }

    // No section found — append
    std::string result = moonraker_content;
    if (!result.empty() && result.back() != '\n')
        result += '\n';
    result += include_block;
    return result;
}

std::string MoonrakerConfigManager::get_section_value(const std::string& content,
                                                      const std::string& section_name,
                                                      const std::string& key) {
    const std::string target_section = "[" + section_name + "]";
    std::istringstream stream(content);
    std::string line;
    bool in_section = false;

    while (std::getline(stream, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#')
            continue;

        if (t[0] == '[') {
            in_section = (t == target_section);
            continue;
        }

        if (!in_section)
            continue;

        // Parse "key : value" with optional whitespace
        size_t colon = t.find(':');
        if (colon == std::string::npos)
            continue;
        std::string k = trim(t.substr(0, colon));
        if (k != key)
            continue;
        return trim(t.substr(colon + 1));
    }
    return "";
}

} // namespace helix
