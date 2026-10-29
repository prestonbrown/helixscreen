// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filename_utils.h"

#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <mutex>
#include <optional>
#include <vector>

namespace helix::gcode {

namespace {

// The printable-extension list every consumer shares: the Moonraker file list,
// the USB stick scanner and the display-name stripper. A second copy anywhere
// drifts, and each copy reads correct alone.
const std::vector<std::string>& printable_extensions() {
    static const std::vector<std::string> extensions = {".gcode", ".gco", ".g", ".3mf"};
    return extensions;
}

// Case-insensitive suffix match. A name exactly as long as the extension is a
// hidden dotfile (".gcode"), not a printable file.
bool ends_with_ci(const std::string& filename, const std::string& ext) {
    return filename.size() > ext.size() &&
           helix::text_io::equals_ignore_case(
               std::string_view(filename).substr(filename.size() - ext.size()), ext);
}

} // namespace

bool has_printable_extension(const std::string& filename) {
    for (const auto& ext : printable_extensions()) {
        if (ends_with_ci(filename, ext)) {
            return true;
        }
    }
    return false;
}

std::string join_gcode_path(const std::string& dir, const std::string& filename) {
    return dir.empty() ? filename : dir + "/" + filename;
}

std::string get_filename_basename(const std::string& path) {
    const size_t last_sep = last_path_separator(path);
    return last_sep == std::string::npos ? path : path.substr(last_sep + 1);
}

std::string strip_gcode_extension(const std::string& filename) {
    // A QIDI print file is "Name.gcode.3mf": the whole double extension goes.
    if (filename.size() > 4 && is_3mf(filename)) {
        const std::string inner = filename.substr(0, filename.size() - 4);
        return is_3mf(inner) ? inner : strip_gcode_extension(inner);
    }

    for (const auto& ext : printable_extensions()) {
        if (ends_with_ci(filename, ext)) {
            return filename.substr(0, filename.size() - ext.size());
        }
    }

    return filename;
}

std::string get_display_filename(const std::string& path) {
    return strip_gcode_extension(get_filename_basename(path));
}

// Pattern: .helix_temp/modified_123456789_OriginalName.gcode (Moonraker plugin)
// Also handles: */gcode_mod/mod_XXXXXX_filename.gcode (local temp files)
// Legacy: /tmp/helixscreen_mod_XXXXXX_filename.gcode
// The staging directory on the printer and the prefix inside it. Named once:
// producers build paths through make_rewritten_gcode_path() and consumers
// recognise them through is_uploaded_rewrite_path(), so neither side can spell
// it differently from the other.
// A printer whose Klipper skips dot-directories when it resolves a print gets
// the visible directory; every other printer, the hidden one.
static const std::string hidden_staging_dir = ".helix_temp";
static const std::string visible_staging_dir = "helix_staging";
static const std::string staged_name_prefix = "modified_";
// Where the HelixPrint plugin links a staged copy: its default directory, and
// whatever its status names instead (a visible one where Klipper skips
// dot-directories). Under its `full/` segment the remainder IS the original's
// gcodes-relative path; directly under the directory sits only a bare filename.
static const std::string default_plugin_link_dir = ".helix_print";
static const std::string helix_print_full_segment = "full/";

namespace {
std::mutex& link_dir_mutex() {
    static std::mutex m;
    return m;
}
std::string& configured_link_dir() {
    static std::string dir;
    return dir;
}
} // namespace

void set_plugin_link_dir(const std::string& dir) {
    std::string clean = dir;
    while (!clean.empty() && clean.back() == '/') {
        clean.pop_back();
    }
    std::lock_guard<std::mutex> lock(link_dir_mutex());
    configured_link_dir() = clean;
}

static std::string plugin_link_dir_copy() {
    std::lock_guard<std::mutex> lock(link_dir_mutex());
    return configured_link_dir();
}
static const std::string gcode_mod_prefix = "/gcode_mod/mod_";
static const std::string legacy_prefix = "/tmp/helixscreen_mod_";

// Start of the original's path inside a plugin symlink path, or npos. Klipper
// reports gcodes-relative paths, so the plugin's directory is only ever the
// leading segment; a user folder of that name deeper down is not a link.
static size_t helix_print_original_pos(const std::string& path) {
    const std::string configured = plugin_link_dir_copy();
    for (const std::string* dir : {&default_plugin_link_dir, &configured}) {
        if (dir->empty()) {
            continue;
        }
        const std::string prefix = *dir + "/";
        if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0) {
            return prefix.size();
        }
    }
    return std::string::npos;
}

static const std::string& staging_dir(StagingRoot root) {
    return root == StagingRoot::Visible ? visible_staging_dir : hidden_staging_dir;
}

/// Where "<staging dir>/modified_" sits in @p path, and where the name after it
/// starts; npos when @p path is not under either staging directory.
static std::pair<size_t, size_t> find_staged_prefix(const std::string& path) {
    for (const auto root : {StagingRoot::Hidden, StagingRoot::Visible}) {
        const std::string prefix = staging_dir(root) + "/" + staged_name_prefix;
        // Root-only: the staging directories sit at the top of the gcodes
        // root, as the printer reports a path, perhaps behind that root's
        // name. A user folder of the same name deeper down is the user's own.
        static const std::string gcodes_root = "gcodes/";
        for (const size_t pos : {size_t{0}, gcodes_root.size()}) {
            if (pos > 0 && path.compare(0, gcodes_root.size(), gcodes_root) != 0) {
                continue;
            }
            if (path.compare(pos, prefix.size(), prefix) == 0) {
                return {pos, pos + prefix.size()};
            }
        }
    }
    return {std::string::npos, std::string::npos};
}

StagingRoot staging_root_for(bool plugin_present, const std::string& plugin_link_dir,
                             bool db_hides_dot_dirs) {
    if (plugin_present && !plugin_link_dir.empty()) {
        return plugin_link_dir[0] == '.' ? StagingRoot::Hidden : StagingRoot::Visible;
    }
    return db_hides_dot_dirs ? StagingRoot::Visible : StagingRoot::Hidden;
}

std::vector<std::string> staging_dirs() {
    return {hidden_staging_dir, visible_staging_dir};
}

bool is_plugin_link_dir(const std::string& name) {
    return !name.empty() && (name == default_plugin_link_dir || name == plugin_link_dir_copy());
}

bool is_app_managed_directory(const std::string& name) {
    return name == visible_staging_dir || (!name.empty() && name == plugin_link_dir_copy());
}

bool is_rewritten_gcode_path(const std::string& path) {
    return find_staged_prefix(path).first != std::string::npos ||
           helix_print_original_pos(path) != std::string::npos ||
           path.find(gcode_mod_prefix) != std::string::npos ||
           path.find(legacy_prefix) != std::string::npos;
}

// A staged copy is one flat file in the staging directory, so its name carries
// the original's whole gcodes-relative path with '/' escaped: '~' -> "~~" and
// '/' -> "~s". The 'p' after the timestamp marks that escaping; a name without
// it holds a bare filename, unescaped.
static const char path_marker = 'p';

static std::string escape_path(const std::string& path) {
    std::string out;
    for (char c : path) {
        if (c == '~') {
            out += "~~";
        } else if (c == '/') {
            out += "~s";
        } else {
            out += c;
        }
    }
    return out;
}

// A copy extracted from one plate of an archive ends "~p<plate>.gcode" after
// the original's name: Klipper prints only G-code names, and the plate goes
// with it. Escaping never yields "~p", so in an escaped name it is unambiguous.
static const std::string plate_marker = "~p";
static const std::string plate_ext = ".gcode";

static std::string plate_suffix(int plate) {
    return plate_marker + std::to_string(plate) + plate_ext;
}

/// The plate @p name ends with, removed from it, or 0 when it carries none.
static int take_plate_suffix(std::string& name) {
    if (name.size() < plate_ext.size() ||
        name.compare(name.size() - plate_ext.size(), plate_ext.size(), plate_ext) != 0) {
        return 0;
    }
    size_t digits = name.size() - plate_ext.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(name[digits - 1]))) {
        --digits;
    }
    if (digits == name.size() - plate_ext.size() || digits < plate_marker.size() ||
        name.compare(digits - plate_marker.size(), plate_marker.size(), plate_marker) != 0) {
        return 0;
    }
    const int plate = std::atoi(name.c_str() + digits);
    name.resize(digits - plate_marker.size());
    return plate;
}

/// Unescapes @p name, ending at a plate suffix, whose plate goes to @p plate.
static std::string unescape_path(const std::string& name, int& plate) {
    std::string out;
    plate = 0;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '~' && i + 1 < name.size()) {
            if (name[i + 1] == 'p') {
                std::string rest = name.substr(i);
                plate = take_plate_suffix(rest);
                if (plate > 0 && rest.empty()) {
                    break;
                }
                plate = 0;
            }
            out += name[i + 1] == 's' ? '/' : name[i + 1];
            ++i;
        } else {
            out += name[i];
        }
    }
    return out;
}

namespace {
/// What a rewritten path says about its original: the name it carries, and
/// whether that name is the original's whole gcodes-relative path or only a
/// bare filename that may belong to a file in any folder.
struct ParsedRewrite {
    std::string original;
    bool full_path = false;
    int plate = 0; ///< the archive plate a copy was extracted from, or 0
};
} // namespace

// The one reading of every rewrite shape this app or its plugin produces.
static std::optional<ParsedRewrite> parse_rewrite(const std::string& path) {
    if (const size_t start = helix_print_original_pos(path); start != std::string::npos) {
        // .helix_print/full/parts/benchy.gcode -> parts/benchy.gcode (whole path)
        // .helix_print/benchy.gcode            -> benchy.gcode (bare name)
        std::string rest = path.substr(start);
        if (rest.size() > helix_print_full_segment.size() &&
            rest.compare(0, helix_print_full_segment.size(), helix_print_full_segment) == 0) {
            return ParsedRewrite{rest.substr(helix_print_full_segment.size()), true};
        }
        return ParsedRewrite{std::move(rest), false};
    }

    size_t prefix_end = std::string::npos;
    if (const auto [pos, name_start] = find_staged_prefix(path); pos != std::string::npos) {
        // .helix_temp/modified_<ts>p_parts~sbenchy.gcode -> parts/benchy.gcode
        // .helix_temp/modified_<ts>_benchy.gcode         -> benchy.gcode
        prefix_end = name_start;
        const size_t underscore = path.find('_', prefix_end);
        if (underscore != std::string::npos && underscore > prefix_end &&
            path[underscore - 1] == path_marker && underscore + 1 < path.size()) {
            int plate = 0;
            std::string original = unescape_path(path.substr(underscore + 1), plate);
            return ParsedRewrite{std::move(original), true, plate};
        }
    } else if (const size_t mod = path.find(gcode_mod_prefix); mod != std::string::npos) {
        // */gcode_mod/mod_123456_OriginalName.gcode -> OriginalName.gcode
        prefix_end = mod + gcode_mod_prefix.size();
    } else if (const size_t legacy = path.find(legacy_prefix); legacy != std::string::npos) {
        // /tmp/helixscreen_mod_123456_OriginalName.gcode -> OriginalName.gcode
        prefix_end = legacy + legacy_prefix.size();
    }
    if (prefix_end == std::string::npos) {
        return std::nullopt;
    }
    const size_t underscore = path.find('_', prefix_end);
    if (underscore == std::string::npos || underscore + 1 >= path.size()) {
        return std::nullopt;
    }
    std::string original = path.substr(underscore + 1);
    const int plate = take_plate_suffix(original);
    return ParsedRewrite{std::move(original), false, plate};
}

std::string resolve_gcode_filename(const std::string& path) {
    auto parsed = parse_rewrite(path);
    if (!parsed) {
        return path;
    }
    spdlog::debug("[resolve_gcode_filename] '{}' -> '{}'", path, parsed->original);
    return std::move(parsed->original);
}

std::optional<std::string> trusted_original_path(const std::string& path) {
    if (!is_rewritten_gcode_path(path)) {
        return path;
    }
    auto parsed = parse_rewrite(path);
    if (!parsed || !parsed->full_path) {
        return std::nullopt;
    }
    return std::move(parsed->original);
}

bool thumbnail_source_describes(const std::string& raw, const std::string& source) {
    if (raw == source) {
        return true;
    }
    // A rewritten temp path is the whole reason an override exists, and only
    // this app produces one - so it always belongs to a print we started, whose
    // preparing epoch set the override being held. Keep it even when the
    // original cannot be recovered from the string.
    if (is_rewritten_gcode_path(raw)) {
        return true;
    }
    const std::string resolved = resolve_gcode_filename(raw);
    if (resolved == source) {
        return true;
    }
    return get_filename_basename(resolved) == get_filename_basename(source);
}

// The staged name for @p original_path, ending in @p suffix (empty, or a plate suffix).
static std::string staged_path(const std::string& original_path, const std::string& suffix,
                               StagingRoot root) {
    // One path component on the printer's filesystem, so it must fit NAME_MAX.
    constexpr size_t name_max = 255;
    const std::string dir = staging_dir(root) + "/";
    const std::string prefix = dir + staged_name_prefix;
    const std::string stamp = std::to_string(static_cast<long long>(std::time(nullptr)));

    std::string staged = prefix + stamp + path_marker + "_" + escape_path(original_path) + suffix;
    if (staged.size() - dir.size() <= name_max) {
        return staged;
    }
    // Too long to carry the whole path: a bare filename, which
    // trusted_original_path() refuses to read as the original's location.
    staged = prefix + stamp + "_";
    std::string name = get_filename_basename(original_path);
    const size_t room = name_max - (staged.size() - dir.size()) - suffix.size();
    if (name.size() > room) {
        // Keep the extension, and start on a UTF-8 lead byte: a name cut inside
        // a multibyte character is not a valid filename to upload.
        size_t start = name.size() - room;
        while (start < name.size() && (static_cast<unsigned char>(name[start]) & 0xC0) == 0x80) {
            ++start;
        }
        name = name.substr(start);
    }
    return staged + name + suffix;
}

std::string make_rewritten_gcode_path(const std::string& original_path, StagingRoot root) {
    return staged_path(original_path, "", root);
}

std::string make_extracted_plate_path(const std::string& archive_path, int plate,
                                      StagingRoot root) {
    return staged_path(archive_path, plate_suffix(plate), root);
}

int rewritten_archive_plate(const std::string& path) {
    const auto parsed = parse_rewrite(path);
    return parsed ? parsed->plate : 0;
}

bool is_uploaded_rewrite_path(const std::string& path) {
    return find_staged_prefix(path).first != std::string::npos;
}

ReprintTarget reprint_target(const std::string& recorded, const std::string& raw) {
    const std::optional<std::string> reported = trusted_original_path(raw);
    ReprintTarget target;
    target.file = !recorded.empty() && (!reported || *reported == recorded) ? recorded
                                                                            : reported.value_or("");
    if (reported && target.file == *reported && is_3mf(target.file)) {
        target.archive_plate = rewritten_archive_plate(raw);
    }
    return target;
}

std::string preview_gcode_for(const std::string& effective, const std::string& raw) {
    if (is_3mf(effective) && !raw.empty() && !is_3mf(raw) && has_printable_extension(raw)) {
        return raw;
    }
    return effective;
}

bool is_3mf(const std::string& name) {
    return ends_with_ci(name, ".3mf");
}

} // namespace helix::gcode
