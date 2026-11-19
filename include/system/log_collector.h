// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <ctime>
#include <string>
#include <string_view>
#include <vector>

/// Unified log-tail collection for crash reporter and debug bundle.
///
/// Three sources, tried in order:
///   1. File-based logs at /var/log, XDG, /opt/config/mod_data (AD5X mod tree),
///      /tmp (desktop + systems with file sink, plus stdout-redirected init
///      scripts)
///   2. Syslog at /var/log/messages or /var/log/syslog (embedded: AD5M, AD5X, K1)
///   3. systemd journal via `journalctl SYSLOG_IDENTIFIER=helix-screen` (pi, pi32)
///
/// HelixScreen's logging pipeline writes to exactly one of these per device, so
/// the cascade converges quickly. Callers that know which source they want can
/// invoke one of the individual tail_* functions.
namespace helix::logs {

/// Default file-log search paths in resolution order, matching
/// logging_init.cpp's `resolve_log_file_path()`. Includes env-dependent entries
/// (XDG_DATA_HOME, HOME) resolved at call time. The AD5X mod_data entries are
/// included only when helix::flashforge_mod_tree(probe_root)
/// detects a mod tree; `probe_root` (default "/") is a test seam for feeding
/// a fake layout.
std::vector<std::string> default_file_paths(const std::string& probe_root = "/");

/// Every place the launcher's stdout/stderr capture (helixscreen.init's
/// LOGFILE) can live, @p install_root's first when given. That file records
/// each helix-screen exit code and any stderr written after spdlog is gone.
/// The AD5X mod-tree spelling is included only when @p probe_root carries that
/// layout, as in default_file_paths().
std::vector<std::string> launcher_log_paths(const std::string& install_root,
                                            const std::string& probe_root = "/");

/// The file tail_file() read: its path and last-modified time.
struct TailSource {
    std::string path;
    std::time_t modified = 0;
};

/// Read the last `num_lines` lines from the most-recently-modified readable,
/// non-empty path in `paths` (NOT first-in-list). The list is priority-ordered,
/// but freshness wins so a stale leftover at a higher-priority path can't shadow
/// the log being written now (#981). Returns empty if none exist/are readable.
/// @p source, when given, receives the file that was read.
std::string tail_file(const std::vector<std::string>& paths, int num_lines,
                      TailSource* source = nullptr);

/// Read the last `num_lines` helix-screen entries from /var/log/messages or
/// /var/log/syslog. Filters for "helix-screen", "helix-watchdog", and
/// "helix-splash" identifiers. Returns empty if neither file exists.
std::string tail_syslog(int num_lines);

/// Testable overload: read the last `num_lines` helix-screen entries from the
/// supplied `paths` list instead of /var/log. Same filter as `tail_syslog`.
/// Production code should call `tail_syslog()` — this exists so tests can
/// feed fake syslog files without touching /var/log.
std::string tail_syslog_from(const std::vector<std::string>& paths, int num_lines);

/// Run `journalctl SYSLOG_IDENTIFIER=helix-screen -n NUM --no-pager -b 0` and
/// return its output. Returns empty if journalctl isn't installed, the journal
/// is empty for this identifier, or the call fails. The `-b 0` flag restricts
/// to the current boot so we don't drag in old noise.
std::string tail_journal(int num_lines);

/// Run @p cmd through popen and return its stdout, keeping the last
/// @p max_lines lines. Append "2>/dev/null" to @p cmd to silence stderr.
/// @p exit_status, when given, receives pclose()'s status (-1 if popen failed).
std::string run_capture_tail(const std::string& cmd, int max_lines, std::string_view source_tag,
                             int* exit_status = nullptr);

/// Cascade: the app's own resolved log file (logging::effective_log_file_path)
/// first, then the default file search (freshest wins), then syslog, then
/// journal. First source with content wins. The authoritative-file step is
/// skipped when the caller pins an explicit `paths` vector; pass an empty vector
/// to use the default cascade.
std::string tail_best(int num_lines, const std::vector<std::string>& paths = {});

} // namespace helix::logs
