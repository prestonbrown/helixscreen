// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
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
 * @brief Join a Moonraker-relative directory to a filename
 *
 * The virtual-SD paths Moonraker's file APIs take are `<dir>/<filename>`, with
 * the root directory spelled as the empty string and NO leading slash — so the
 * root case must yield the bare filename, not "/benchy.gcode".
 *
 * One named function because the panel builds this path on several request
 * paths (metadata fetch, print start) and each open-coded ternary is a place the
 * root case can be got wrong independently.
 *
 * @param dir Directory relative to the gcodes root ("" for root)
 * @param filename Bare filename, no directory component
 * @return "filename" when @p dir is empty, otherwise "dir/filename"
 */
std::string join_gcode_path(const std::string& dir, const std::string& filename);

/**
 * @brief Does this filename carry an extension we treat as printable?
 *
 * The one list of printable extensions (.gcode, .gco, .g, .3mf,
 * case-insensitive). Every consumer that decides "is this a printable file"
 * (the Moonraker file list, the USB stick scanner, the display-name stripper)
 * asks here; two hand-kept lists drift and each reads correct alone. FAT
 * mounts without long-filename support yield 8.3 upper-case names
 * (3DBENC~1.GCO), so the match must be case-insensitive down to ".g".
 *
 * A name consisting solely of the extension (".gcode") is a hidden dotfile,
 * not a printable file.
 *
 * @param filename Bare filename or path
 * @return true if the name ends in a printable extension
 */
bool has_printable_extension(const std::string& filename);

/**
 * @brief Strip G-code file extensions for display
 *
 * Removes common G-code extensions (.gcode, .g, .gco, .3mf, case-insensitive)
 * for cleaner display in the UI. Strips exactly the extensions
 * has_printable_extension() accepts; a G-code extension under a .3mf goes
 * with it, so "Foo.gcode.3mf" becomes "Foo".
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
 * - `.helix_print/full/dir/OriginalName.gcode` (HelixPrint plugin symlink)
 * - `.helix_print/OriginalName.gcode` (older plugin: bare filename)
 * - `.helix_temp/modified_123456789p_dir~sOriginalName.gcode` (full path kept)
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
 * @brief The original's gcodes-relative path, when @p path can be trusted to name it
 *
 * resolve_gcode_filename() always answers with its best guess. This answers
 * only when the answer is certain: a path that is not a rewrite names itself,
 * and a rewrite counts only when it encodes the original's whole path (a
 * `.helix_temp/modified_<ts>p_` name, or the plugin's `.helix_print/full/`).
 * A bare-filename rewrite could belong to a file in any folder, so acting on
 * it (a reprint, adopting a history job) could pick a same-named wrong file.
 *
 * @param path Filename or path as the printer reports it
 * @return The original's path, or nullopt when its location is unknown
 */
std::optional<std::string> trusted_original_path(const std::string& path);

/**
 * @brief Is this path one of OUR rewritten temp copies of a user's G-code?
 *
 * True for the shapes resolve_gcode_filename() knows how to unwrap: a
 * `.helix_temp/modified_` prefix, the HelixPrint plugin's `.helix_print/`
 * symlink directory, a `/gcode_mod/mod_` path segment, or the legacy
 * `/tmp/helixscreen_mod_` prefix.
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
 * @brief Build the gcodes-root-relative path a rewritten copy is uploaded to.
 *
 * The ONE spelling of that name, and the reason it is a function rather than a
 * string each caller assembles: the post-print cleanup, the startup sweep and
 * resolve_gcode_filename() all recognise a staged copy BY THIS PREFIX. A path
 * built any other way is invisible to every one of them at once - its temp file
 * outlives the print and its name never resolves back to the original, so the
 * job the user started shows up under a name they have never seen.
 *
 * The original's full path is encoded into the one flat name, so
 * resolve_gcode_filename() recovers it from the printer's report alone. A path
 * too long for one filename falls back to the bare filename, which
 * trusted_original_path() then declines to vouch for.
 *
 * @param original_path The original's gcodes-root-relative path
 * @return e.g. "<staging dir>/modified_1766807545p_parts~sbenchy.gcode"
 */
std::string make_rewritten_gcode_path(const std::string& original_path);

/**
 * @brief Is this a copy WE staged on the printer, i.e. ours to delete?
 *
 * Narrower than is_rewritten_gcode_path(), which also answers true for our
 * local scratch copies and for the HelixPrint plugin's symlinks, which the
 * plugin removes itself. Only a path under the printer's staging directory
 * names a file the printer holds and that we are responsible for removing when
 * the print ends.
 *
 * @param path Filename or path as the printer reports it
 * @return true if the path is a copy we uploaded
 */
bool is_uploaded_rewrite_path(const std::string& path);

/**
 * @brief Does a recorded thumbnail source still describe the reported print?
 *
 * A thumbnail source names the file whose media the current print should be
 * resolved from - the ORIGINAL, when what the printer reports is a rewritten
 * temp copy. Nothing retires it when a print ends, so both the media manager
 * and the print-status panel have to ask this of every incoming filename or
 * they resolve the next print through the previous print's name (#1339).
 *
 * True when the source names the same file, when the reported path is one of
 * our own rewrites (which only ever belong to a print this app started), or
 * when the two agree once resolved and stripped to a basename - a preparing
 * job records its full path while the rewrite resolves to a bare name.
 *
 * @param raw    Filename as the printer reports it
 * @param source The recorded thumbnail source
 * @return true if the source still describes this print
 */
bool thumbnail_source_describes(const std::string& raw, const std::string& source);

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

/**
 * @brief Name of the plate G-code QIDI's Moonraker extracts into `.temp` when a
 *        `.3mf` print starts: the basename without `.3mf`, with `.gcode` added
 *        when that leaves no `.gcode` suffix ("Benchy.gcode.3mf" and
 *        "Benchy.3mf" both give "Benchy.gcode").
 */
std::string qidi_3mf_extract_name(const std::string& print_filename);

/// Is the `.temp` entry @p entry the extract of @p print_filename? Compared
/// case-insensitively.
bool is_qidi_3mf_extract(const std::string& entry, const std::string& print_filename);

/**
 * @brief Test whether a filename names a `.3mf` project file (case-insensitive).
 *
 * A `.3mf` is a zip archive, not G-code: there are no layers to parse and no
 * G-code lines to scan in its bytes.
 */
bool is_3mf(const std::string& name);

} // namespace helix::gcode
