// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @file helix_paths.h
 * @brief Shared filesystem-path primitives.
 *
 * Stage 1 of a DRY refactor: these primitives were previously reimplemented
 * (6+ times) across update_checker.cpp, log_path_probe.cpp,
 * input_shaper_cache.cpp, thumbnail_cache.cpp, app_globals.cpp,
 * app_constants.h, logging_init.cpp, and data_root_resolver.cpp. Each function
 * documents which existing implementation its semantics were converged from.
 *
 * This module is ADDITIVE. Call sites are migrated in later stages.
 */

namespace helix::paths {

/**
 * @brief Cheap, side-effect-free writability check.
 *
 * Converges on update_checker.cpp is_writable_dir: access(dir, W_OK) == 0.
 * Returns false for empty or nonexistent directories (access() fails).
 */
bool is_writable_dir(const std::string& dir);

/**
 * @brief 64-bit-safe available space in bytes for the filesystem holding `dir`.
 *
 * Converges on update_checker.cpp get_available_space and log_path_probe.cpp:
 * statvfs, then (uint64_t)f_bavail * (uint64_t)f_frsize with explicit 64-bit
 * casts. The casts matter: on 32-bit platforms (pi32/armhf, MIPS32) the native
 * product wraps for filesystems > ~4 GiB. Returns 0 on statvfs failure.
 */
std::uint64_t available_space(const std::string& dir);

/**
 * @brief Robust probe: is `dir` writable AND does it have enough free space?
 *
 * Converges the create+write+remove write-test from
 * input_shaper_cache.cpp / thumbnail_cache.cpp try_create_cache_dir with the
 * statvfs space check. Creates a UNIQUELY-named temp file inside `dir`
 * (".helix_write_test.<pid>.<tid>.<counter>", unique per process, per thread,
 * and per call so concurrent same-process probes of the same dir never collide),
 * writes a byte, then removes it.
 *
 * Returns false if `dir` is empty, does not exist, has less than
 * @p min_free_bytes free, or cannot be written. When @p min_free_bytes == 0 the
 * space check is skipped. Does NOT create `dir` itself — the caller uses
 * ensure_dir() first if needed.
 */
bool probe_writable(const std::string& dir, std::uint64_t min_free_bytes = 0);

/**
 * @brief First candidate directory that is writable, or "" if none qualify.
 *
 * Loops @p candidates in order and returns the first one for which
 * probe_writable(dir, min_free_bytes) passes (honoring the optional free-space
 * gate). Does NOT create directories — candidates must already exist. The order
 * of @p candidates defines preference. Backs app_get_runtime_dir()'s
 * /tmp-then-cache fallback on ProtectSystem=strict devices where /tmp is
 * read-only.
 */
std::string first_writable_dir(const std::vector<std::string>& candidates,
                               std::uint64_t min_free_bytes = 0);

/**
 * @brief create_directories(path) and verify the result is a directory.
 *
 * Converges on app_globals.cpp try_create_dir. Swallows std::filesystem
 * exceptions (returns false), matching the current error-code-based behavior.
 */
bool ensure_dir(const std::string& path);

/**
 * @brief Deepest ancestor of @p path that exists as a directory, or "".
 *
 * Walks up from @p path itself (a path that already exists answers with
 * itself) to "/", stopping at the first component that is a directory.
 */
std::string deepest_existing_dir(const std::string& path);

/**
 * @brief Could ensure_dir(@p path) succeed, WITHOUT finding out by doing it?
 *
 * The cache resolvers used to answer "is this location usable?" by calling
 * ensure_dir() and reading success as the verdict. That makes the question
 * mutating: asking where something would live creates it there, so a caller
 * that only wanted the path — a stale-vs-live classifier, a diagnostic, a
 * predicate over a directory it is not about to write to — left a directory
 * tree behind as the price of asking.
 *
 * Answers the same question by walking to the deepest existing ancestor and
 * testing is_writable_dir() on it. Callers that then intend to write should
 * still check ensure_dir()'s result: this predicate cannot see a race, a
 * quota, or ENOSPC, and is a filter for candidates, not a guarantee.
 */
bool can_create_dir(const std::string& path);

/**
 * @brief Sanitized $HOME, or "" when unusable.
 *
 * Converges on app_constants.h sanitize_home: returns $HOME only when it is
 * non-empty and absolute (starts with '/'), also rejecting any value that
 * contains a control character (the heap-corruption guard sanitize_home added
 * after single-char junk directories were observed). Differs from sanitize_home
 * on ONE point: it returns "" instead of "/tmp" — the /tmp fallback policy is
 * left to the caller (see logging_init.cpp / app_constants.h).
 */
std::string home();

/**
 * @brief Ordered cache-base candidates: $XDG_CACHE_HOME (if set), then
 *        home()+"/.cache" (if home is usable). May be empty.
 *
 * Base directories only — callers append "/helix/<subdir>" and try each in
 * order. Returning BOTH (rather than just the first) preserves the original
 * two-step fallback in app_globals.cpp get_helix_cache_dir (steps 4/5) and
 * input_shaper_cache.cpp determine_cache_dir (steps 1/2): if the XDG dir can't
 * be created, callers still fall through to $HOME/.cache before the next step.
 */
std::vector<std::string> xdg_cache_bases();

/**
 * @brief $XDG_DATA_HOME if set and non-empty, else home()+"/.local/share",
 *        else "".
 *
 * Mirrors logging_init.cpp get_xdg_data_home EXCEPT it does not hard-fall-back
 * to "/tmp"; that policy stays at the call site.
 */
std::string xdg_data_home();

/**
 * @brief dirname without POSIX dirname(3) (which mutates its argument).
 *
 * Converges on update_checker.cpp path_dirname: strip trailing slashes (keeping
 * a lone "/"), return "." for a bare filename or empty input, "/" for a
 * root-level path, else the leading portion.
 */
std::string dirname(const std::string& path);

/**
 * @brief Strip trailing slashes but keep a lone "/".
 *
 * Converges on update_checker.cpp strip_trailing_slashes (the loop form, which
 * collapses multiple trailing slashes) over data_root_resolver.cpp
 * strip_trailing_slash (which only removed a single trailing slash). Empty input
 * returns "".
 */
std::string strip_trailing_slash(const std::string& path);

} // namespace helix::paths
