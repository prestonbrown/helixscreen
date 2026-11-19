// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Filesystem queries and operations over POSIX calls, with no std::filesystem.
// libstdc++'s filesystem converts paths through a wide codecvt and copies files
// through a filebuf, and either one links every std::locale facet (~150K) into
// the ESP32 image. Nothing here throws. Paths are plain std::string.
//
// File sizes come from helix::text_io::file_size.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::fs {

// ---------------------------------------------------------------------------
// Path strings (std::filesystem::path semantics for POSIX paths)
// ---------------------------------------------------------------------------

/// `path(a) / b`: b when b is absolute or a is empty, else a + '/' + b (no
/// doubled separator when a already ends in '/').
std::string join_path(std::string_view a, std::string_view b);

/// Last element; "" when the path ends in '/'. "." and ".." are filenames.
std::string_view filename(std::string_view p);

/// Everything before the last element, with trailing separators dropped;
/// "/" stays "/" and a bare name has no parent ("").
std::string_view parent_path(std::string_view p);

/// filename() up to its last '.', except a leading '.' and the names "." and ".."
/// have no extension. `.bashrc` -> stem ".bashrc", extension "".
std::string_view stem(std::string_view p);
std::string_view extension(std::string_view p);

/// lexically_normal(): collapses repeated separators, drops "." elements, and
/// folds "name/.." pairs. A trailing separator is kept; an empty result is ".".
std::string lexically_normal(std::string_view p);

// ---------------------------------------------------------------------------
// Queries. Any stat failure reads as "no": the ESP32 VFS reports a missing path
// as ENODATA, so callers probing for existence must not treat errno as a fault.
// ---------------------------------------------------------------------------

bool exists(const std::string& p);
bool is_directory(const std::string& p);    ///< follows symlinks
bool is_regular_file(const std::string& p); ///< follows symlinks
bool is_symlink(const std::string& p);      ///< lstat; always false on ESP32 (no symlinks)
bool is_owner_executable(const std::string& p);

/// Last modification time, nanoseconds since the Unix epoch.
std::optional<std::int64_t> mtime_ns(const std::string& p);

/// Bytes available to an unprivileged writer on the filesystem holding `p`
/// (statvfs f_bavail * f_frsize). nullopt when unknown, and always on ESP32.
std::optional<std::uint64_t> space_available(const std::string& p);

/// realpath(): absolute, symlinks resolved, "." and ".." folded. nullopt when
/// the path does not exist. ESP32 has no symlinks, so there it only normalizes.
std::optional<std::string> canonical(const std::string& p);

/// readlink(): what the symlink at `p` points at, as stored (possibly relative).
/// nullopt when `p` is not a symlink, and always on ESP32.
std::optional<std::string> read_symlink(const std::string& p);

// ---------------------------------------------------------------------------
// Operations. false on failure with errno set by the failing call.
// ---------------------------------------------------------------------------

/// mkdir -p. true when `p` is a directory afterwards, including when it already
/// was (std::filesystem returns false then; nothing here relies on that).
bool create_directories(const std::string& p);

/// Removes a file or an empty directory. true when something was removed; false
/// with errno ENOENT when `p` did not exist, any other errno on a real failure.
bool remove(const std::string& p);

/// rm -r: removes `p` and everything under it. A symlink is removed, never
/// followed, so nothing outside `p` is touched. true when `p` is gone
/// afterwards, including when it never existed.
bool remove_all(const std::string& p);

bool rename(const std::string& from, const std::string& to);

/// Copies the bytes of `from` to `to`. With overwrite false, an existing `to`
/// fails with EEXIST (copy_options::none); with true it is truncated and
/// replaced (copy_options::overwrite_existing). A failed copy removes the
/// partial `to`.
bool copy_file(const std::string& from, const std::string& to, bool overwrite = false);

// ---------------------------------------------------------------------------
// Directory listing
// ---------------------------------------------------------------------------

struct DirEntry {
    std::string name;        ///< filename, never "." or ".."
    std::string path;        ///< join_path(dir, name)
    bool is_dir = false;     ///< follows symlinks, like directory_entry::is_directory()
    bool is_regular = false; ///< follows symlinks
};

/// The entries of `dir` in readdir order. nullopt when `dir` cannot be opened
/// (errno set). A read error part way returns the entries read so far.
std::optional<std::vector<DirEntry>> list_dir(const std::string& dir);

// ---------------------------------------------------------------------------
// Threads that must never reach storage
// ---------------------------------------------------------------------------

/// Marks the calling thread as one that must never touch the filesystem. On the
/// ESP32 a thread whose stack is in PSRAM crashes the board inside the flash
/// driver on any LittleFS access, so every helix::fs and helix::text_io call it
/// makes afterwards fails with errno EPERM and logs the call, path and thread.
void forbid_storage_on_this_thread(const char* thread_name);

/// False, after logging, when the calling thread was marked by
/// forbid_storage_on_this_thread(); true everywhere else.
bool storage_allowed(const char* op, std::string_view path);

} // namespace helix::fs
