// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

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

/// Where a path's directory part ends: its last '/', or npos for a bare name.
/// Moonraker and USB paths separate with '/' alone, so a backslash is part of
/// a name. gcode_dir_of() and get_filename_basename() both split here, so the
/// two halves of a path always come from one split.
[[nodiscard]] inline size_t last_path_separator(const std::string& path) {
    return path.rfind('/');
}

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

/// Which staging directory a copy the app uploads goes to. Hidden (.helix_temp)
/// everywhere Klipper prints from a dot-directory; Visible (helix_staging) where
/// its file list skips them, since a print it cannot list fails to open.
enum class StagingRoot { Hidden, Visible };

/**
 * @brief Where this printer's staged copies go.
 *
 * The printer's own answer wins: the HelixPrint plugin installs its link
 * directory visible exactly where Klipper skips dot-directories, so with the
 * plugin present and its directory known, a visible one means stage visibly.
 * Without that, @p db_hides_dot_dirs (the printer database's flag) decides.
 */
StagingRoot staging_root_for(bool plugin_present, const std::string& plugin_link_dir,
                             bool db_hides_dot_dirs);

/// Every staging directory, relative to the gcodes root: the startup sweep
/// empties each.
std::vector<std::string> staging_dirs();

/// A gcodes-root directory the app owns (visible staging, the plugin's link
/// directory), which the file list leaves out as it does dot-directories.
bool is_app_managed_directory(const std::string& name);

/// Whether gcodes-root directory @p name is the plugin's link directory: its
/// default `.helix_print` or the one its status names. Any thread.
bool is_plugin_link_dir(const std::string& name);

/// The HelixPrint plugin's link directory, as its status reports it: a print
/// it starts runs `<dir>/full/<original>`, which resolves back to the original
/// alongside the default `.helix_print`. Empty forgets it. Any thread.
void set_plugin_link_dir(const std::string& dir);

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
 * @param root Which staging directory (StagingRoot)
 * @return e.g. "<staging dir>/modified_1766807545p_parts~sbenchy.gcode"
 */
std::string make_rewritten_gcode_path(const std::string& original_path,
                                      StagingRoot root = StagingRoot::Hidden);

/**
 * @brief The staged path for plate @p plate's G-code, extracted from @p archive_path.
 *
 * The sibling of make_rewritten_gcode_path() for a .3mf: the same staging
 * prefix, so the same cleanup reaps it, and a name ending "~p<plate>.gcode" so
 * Klipper takes it as G-code. resolve_gcode_filename() and
 * trusted_original_path() map it back to the archive.
 *
 * @return e.g. "<staging dir>/modified_1766807545p_parts~sX.gcode.3mf~p9.gcode"
 */
std::string make_extracted_plate_path(const std::string& archive_path, int plate,
                                      StagingRoot root = StagingRoot::Hidden);

/// The plate a staged copy was extracted from (make_extracted_plate_path()), or 0.
int rewritten_archive_plate(const std::string& path);

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

/// What Reprint starts: a file, and the plate of it to extract when it is an
/// archive the printer printed as an extracted plate (0 otherwise).
struct ReprintTarget {
    std::string file;
    int archive_plate = 0;
};

/**
 * @brief What Reprint starts, from the job identity this session recorded
 *        (@p recorded, possibly empty) and the name the printer reports (@p raw).
 *
 * The original, never a staged copy that is deleted when the print ends: the
 * recorded identity where the report agrees with it or cannot place one,
 * else the report itself. A plate staged from an archive comes back as the
 * archive with its plate, so it extracts again; the printer's own conversion
 * of one is reported as itself and starts as it is.
 */
ReprintTarget reprint_target(const std::string& recorded, const std::string& raw);

/**
 * @brief The G-code a print's preview reads.
 *
 * A print the user started from a .3mf (@p effective, its identity) runs as
 * G-code the printer reports instead (@p raw): a plate extracted and staged,
 * or the printer's own conversion of it. That G-code is what is printing, so
 * it is what the preview shows; a .3mf the printer runs itself (raw is the
 * .3mf) and every ordinary print keep @p effective.
 */
std::string preview_gcode_for(const std::string& effective, const std::string& raw);

/**
 * @brief Test whether a filename names a `.3mf` project file (case-insensitive).
 *
 * A `.3mf` is a zip archive, not G-code: there are no layers to parse and no
 * G-code lines to scan in its bytes.
 */
bool is_3mf(const std::string& name);

} // namespace helix::gcode

namespace helix {

/// The directory a gcodes-root path sits in: "sub/dir/Foo.gcode" -> "sub/dir", "Foo.gcode" -> "".
/// The inverse of gcode::join_gcode_path() together with gcode::get_filename_basename().
[[nodiscard]] inline std::string gcode_dir_of(const std::string& gcode_path) {
    const auto slash = gcode::last_path_separator(gcode_path);
    return slash == std::string::npos ? std::string() : gcode_path.substr(0, slash);
}

} // namespace helix
