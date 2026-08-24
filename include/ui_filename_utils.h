// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace helix::gcode {

/**
 * @brief Extract basename from a file path
 *
 * Returns just the filename portion, stripping any directory path.
 * Examples: "/path/to/file.gcode" -> "file.gcode", "file.gcode" -> "file.gcode"
 *
 * @param path Full path or filename
 * @return Filename only (basename)
 */
std::string get_filename_basename(const std::string& path);

/**
 * @brief Strip G-code file extensions for display
 *
 * Removes common G-code extensions (.gcode, .g, .gco, case-insensitive)
 * for cleaner display in the UI.
 *
 * @param filename The original filename
 * @return Filename without G-code extension, or original if no match
 */
std::string strip_gcode_extension(const std::string& filename);

/**
 * @brief Get display-friendly filename (basename with extension stripped)
 *
 * Combines get_filename_basename() and strip_gcode_extension() for
 * convenient one-call filename formatting.
 *
 * @param path Full path or filename
 * @return Clean display name (e.g., "/path/to/benchy.gcode" -> "benchy")
 */
std::string get_display_filename(const std::string& path);

/**
 * @brief Resolve a G-code filename to its original/canonical form
 *
 * When HelixScreen modifies a G-code file before printing (e.g., to add
 * filament change commands), it stores the modified file with patterns like:
 * - `.helix_temp/modified_123456789_OriginalName.gcode`
 * - `/tmp/helixscreen_mod_123456_OriginalName.gcode`
 *
 * This function extracts the original filename for metadata/thumbnail lookups.
 * If the path is not a modified temp path, returns the input unchanged.
 *
 * @param path File path that might be a modified temp file
 * @return Original filename if temp pattern matches, otherwise input unchanged
 */
std::string resolve_gcode_filename(const std::string& path);

/**
 * @brief Is this path one of OUR rewritten temp copies of a user's G-code?
 *
 * True for the three shapes resolve_gcode_filename() knows how to unwrap: a
 * `.helix_temp/modified_` prefix, a `/gcode_mod/mod_` path segment, or the
 * legacy `/tmp/helixscreen_mod_` prefix.
 * Unlike resolve_gcode_filename(), this answers "is it a rewrite" rather than
 * "what was the original", so it is still true for a rewritten name whose
 * original cannot be recovered from the string. Only HelixScreen produces
 * these, so a path matching here always belongs to a print this app started.
 *
 * @param path Filename or path as the printer reports it
 * @return true if the path is a HelixScreen-rewritten temp G-code
 */
bool is_rewritten_gcode_path(const std::string& path);

/**
 * @brief Test whether a filename is a QIDI native-3MF shadow G-code.
 *
 * QIDI firmware translates a native `.3mf` plate into a G-code file exposed via
 * Moonraker's hidden `.temp` root, named `shadow_native_plate_<N>.gcode`. This
 * matches that pattern: the `shadow_native_plate_` prefix, a `.gcode` suffix,
 * and at least one character in between. Case-sensitive to match the firmware.
 *
 * @param name Bare filename (relative to the `.temp` root)
 * @return true if the name is a native-3MF shadow G-code file
 */
bool is_native_3mf_shadow(const std::string& name);

} // namespace helix::gcode
