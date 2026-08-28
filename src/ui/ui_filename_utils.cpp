// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filename_utils.h"

#include <spdlog/spdlog.h>

#include <vector>

namespace helix::gcode {

std::string join_gcode_path(const std::string& dir, const std::string& filename) {
    return dir.empty() ? filename : dir + "/" + filename;
}

std::string get_filename_basename(const std::string& path) {
    if (path.empty()) {
        return path;
    }

    // Find last path separator
    size_t last_sep = path.find_last_of("/\\");
    if (last_sep == std::string::npos) {
        return path; // No separator, already just a filename
    }

    return path.substr(last_sep + 1);
}

std::string strip_gcode_extension(const std::string& filename) {
    // Common G-code extensions (case-insensitive check)
    static const std::vector<std::string> extensions = {".gcode", ".gco", ".g", ".3mf"};

    for (const auto& ext : extensions) {
        if (filename.size() > ext.size()) {
            size_t pos = filename.size() - ext.size();
            // Case-insensitive suffix comparison
            std::string suffix = filename.substr(pos);
            std::string suffix_lower;
            suffix_lower.reserve(suffix.size());
            for (char c : suffix) {
                suffix_lower.push_back(
                    static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            if (suffix_lower == ext) {
                return filename.substr(0, pos);
            }
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
static const std::string helix_temp_prefix = ".helix_temp/modified_";
static const std::string gcode_mod_prefix = "/gcode_mod/mod_";
static const std::string legacy_prefix = "/tmp/helixscreen_mod_";

bool is_rewritten_gcode_path(const std::string& path) {
    return path.find(helix_temp_prefix) != std::string::npos ||
           path.find(gcode_mod_prefix) != std::string::npos ||
           path.find(legacy_prefix) != std::string::npos;
}

std::string resolve_gcode_filename(const std::string& path) {
    size_t underscore_pos = std::string::npos;

    if (path.find(helix_temp_prefix) != std::string::npos) {
        // Extract original: .helix_temp/modified_123456789_OriginalName.gcode -> OriginalName.gcode
        size_t prefix_end = path.find(helix_temp_prefix) + helix_temp_prefix.size();
        underscore_pos = path.find('_', prefix_end);
    } else if (path.find(gcode_mod_prefix) != std::string::npos) {
        // Extract original: */gcode_mod/mod_123456_OriginalName.gcode -> OriginalName.gcode
        size_t prefix_end = path.find(gcode_mod_prefix) + gcode_mod_prefix.size();
        underscore_pos = path.find('_', prefix_end);
    } else if (path.find(legacy_prefix) != std::string::npos) {
        // Legacy: /tmp/helixscreen_mod_123456_OriginalName.gcode -> OriginalName.gcode
        size_t prefix_end = path.find(legacy_prefix) + legacy_prefix.size();
        underscore_pos = path.find('_', prefix_end);
    }

    if (underscore_pos != std::string::npos && underscore_pos + 1 < path.size()) {
        std::string original = path.substr(underscore_pos + 1);
        spdlog::debug("[resolve_gcode_filename] '{}' -> '{}'", path, original);
        return original;
    }

    return path;
}

static std::string basename_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
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
    return basename_of(resolved) == basename_of(source);
}

bool is_native_3mf_shadow(const std::string& name) {
    static const std::string prefix = "shadow_native_plate_";
    static const std::string suffix = ".gcode";

    // Require at least one character between the prefix and suffix (the plate id).
    if (name.size() <= prefix.size() + suffix.size()) {
        return false;
    }
    if (name.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    return name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace helix::gcode
