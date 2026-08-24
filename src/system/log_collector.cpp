// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/log_collector.h"

#include "logging_init.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>

namespace helix::logs {

namespace {

void push_bounded(std::deque<std::string>& lines, std::string line, int max_lines) {
    lines.push_back(std::move(line));
    if (static_cast<int>(lines.size()) > max_lines) {
        lines.pop_front();
    }
}

std::string join_lines(const std::deque<std::string>& lines) {
    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0)
            out << '\n';
        out << lines[i];
    }
    return out.str();
}

std::string read_tail_lines(const std::string& path, int num_lines, std::string_view source_tag) {
    std::ifstream file(path);
    if (!file.good()) {
        return {};
    }

    std::deque<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        push_bounded(lines, std::move(line), num_lines);
    }

    if (lines.empty()) {
        return {};
    }

    spdlog::debug("[Logs] Read {} lines from {} ({})", lines.size(), path, source_tag);
    return join_lines(lines);
}

/// Syslog entries look like "Apr 18 03:48:31 host helix-screen[1234]: ...".
/// Cheap substring match is good enough — our identifier prefixes don't collide
/// with anything else on these systems.
bool is_helix_line(const std::string& line) {
    return line.find("helix-screen") != std::string::npos ||
           line.find("helix-watchdog") != std::string::npos ||
           line.find("helix-splash") != std::string::npos;
}

/// Run a command via popen and return its stdout, capped at `max_lines` lines.
/// Stderr is silenced by the caller (append "2>/dev/null" to `cmd`).
std::string run_capture_tail(const std::string& cmd, int max_lines, std::string_view source_tag) {
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        spdlog::debug("[Logs] popen failed for {}: {}", source_tag, cmd);
        return {};
    }

    // fgets may return a partial line if a single line exceeds the buffer, so
    // accumulate across reads and split only on real newlines.
    std::deque<std::string> lines;
    std::array<char, 4096> buf{};
    std::string partial;
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        partial.append(buf.data());
        size_t nl;
        while ((nl = partial.find('\n')) != std::string::npos) {
            push_bounded(lines, partial.substr(0, nl), max_lines);
            partial.erase(0, nl + 1);
        }
    }
    if (!partial.empty()) {
        push_bounded(lines, std::move(partial), max_lines);
    }

    int rc = ::pclose(pipe);
    if (rc != 0) {
        spdlog::debug("[Logs] {} exited non-zero (rc={})", source_tag, rc);
    }

    if (lines.empty())
        return {};
    spdlog::debug("[Logs] Captured {} lines from {}", lines.size(), source_tag);
    return join_lines(lines);
}

} // namespace

std::vector<std::string> default_file_paths() {
    // Every place an app log has ever been found, for the case where the live
    // process cannot tell us (crash-reporter-on-next-boot). NOT a resolution
    // order: tail_file() picks the most-recently-MODIFIED readable candidate,
    // not the first hit, so a stale file at the top of the list cannot shadow
    // a fresh one further down. List position only breaks ties.
    //
    // The first three entries do mirror logging_init.cpp::resolve_log_file_path()'s
    // own fallback chain (/var/log, then $XDG_DATA_HOME, then ~/.local/share as
    // xdg_data_home()'s default). Everything after them is a path some platform
    // hook or init script pins explicitly, which resolve_log_file_path() never
    // derives on its own.
    std::vector<std::string> paths = {
        "/var/log/helix-screen.log",
    };

    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && xdg[0] != '\0') {
        paths.push_back(std::string(xdg) + "/helix-screen/helix.log");
    }
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        paths.push_back(std::string(home) + "/.local/share/helix-screen/helix.log");
    }
    // ZMOD AD5X / AD5M. /opt/config is a bind-mount of the durable mod config
    // dir, visible at /usr/data/config too, so each file has two path spellings.
    // Two DIFFERENT streams live under mod_data/log:
    //
    //   helixscreen.log — ghzserg's S80helixscreen (their fork of our init
    //     script) hardcodes LOGFILE to it and redirects the launcher subshell
    //     `>> "$LOGFILE" 2>&1`. It carries the launcher's own [helix-launcher]
    //     echoes plus crash/glibc stderr — NOT the app log. spdlog's console
    //     sink does not land here: because that redirect makes stdout a regular
    //     file, should_add_console() deliberately skips the console sink
    //     (logging_init.h — a console sink there would double-log every line).
    //   helix.log — the app's own rotating file sink, pointed here by
    //     hooks-ad5m-zmod.sh. This is the structured helix-screen log. It lives
    //     under /opt/config precisely so ZMOD's TAR_CONFIG archiver captures it
    //     (it collects /opt/config/, never /data/) — see issue #1249.
    //
    // Both are worth collecting; tail_file() picks whichever is freshest.
    paths.emplace_back("/opt/config/mod_data/log/helix.log");
    paths.emplace_back("/usr/data/config/mod_data/log/helix.log");
    paths.emplace_back("/opt/config/mod_data/log/helixscreen.log");
    paths.emplace_back("/usr/data/config/mod_data/log/helixscreen.log");

    // helixscreen.init's launcher-subshell shell-stdout-redirect file.
    // Preferred FHS path (used when /var/log is persistent).
    paths.emplace_back("/var/log/helixscreen/launcher.log");

    // ${DAEMON_DIR}/logs/ fallback when /var/log is tmpfs/ramfs. The launcher
    // writes BOTH helix.log (the app's file sink, --log-file) and launcher.log
    // (the wrapper subshell's stdout) under ${root}/logs. Roots mirror
    // scripts/install.sh HELIX_INSTALL_DIRS — keep the two in sync. This list is
    // only a fallback for the crash-reporter-next-boot case; the live process is
    // covered authoritatively by effective_log_file_path() in tail_best().
    for (const char* root : {
             "/opt/helixscreen",                   // Pi, AD5M Forge-X/KMod
             "/usr/data/helixscreen",              // K1/K1C/K2/AD5X
             "/userdata/helixscreen",              // Snapmaker U1
             "/user-resource/helixscreen",         // CC1 (COSMOS)
             "/root/printer_software/helixscreen", // AD5M KMod v00.05
             "/srv/helixscreen",                   // generic FHS
             "/data/helixscreen",                  // AD5X /data-rooted installs (#981)
         }) {
        paths.emplace_back(std::string(root) + "/logs/helix.log");
        paths.emplace_back(std::string(root) + "/logs/launcher.log");
    }

    // Legacy /tmp location — pre-v0.99.62 installs wrote here. Kept for
    // backward compatibility with debug bundles from older devices.
    paths.emplace_back("/tmp/helixscreen.log");
    return paths;
}

std::string tail_file(const std::vector<std::string>& paths, int num_lines) {
    // Pick the most-recently-modified readable, non-empty file rather than the
    // first one that exists. The list is priority-ordered, but a stale leftover
    // at a higher-priority path (e.g. an old ZMOD stdout-redirect from a prior
    // install) must not shadow the log the app is writing right now. Freshness,
    // not list position, decides — an AD5X bundle once shipped a month-old log
    // for exactly this reason (#981).
    std::vector<std::pair<std::time_t, const std::string*>> candidates;
    for (const auto& path : paths) {
        struct ::stat st {};
        if (::stat(path.c_str(), &st) == 0 && st.st_size > 0) {
            candidates.emplace_back(st.st_mtime, &path);
        }
    }
    // stable_sort so that on an mtime tie the original list priority is kept.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });

    // Read newest-first; skip a file that stat()'d but reads empty (e.g. became
    // unreadable between stat and open) and try the next-freshest.
    for (const auto& [mtime, path] : candidates) {
        (void)mtime;
        auto result = read_tail_lines(*path, num_lines, "file");
        if (!result.empty())
            return result;
    }
    return {};
}

std::string tail_syslog_from(const std::vector<std::string>& paths, int num_lines) {
    for (const auto& path : paths) {
        std::ifstream file(path);
        if (!file.good())
            continue;

        std::deque<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            if (is_helix_line(line)) {
                push_bounded(lines, std::move(line), num_lines);
            }
        }

        if (lines.empty())
            continue;
        spdlog::debug("[Logs] Read {} syslog lines from {}", lines.size(), path);
        return join_lines(lines);
    }

    return {};
}

std::string tail_syslog(int num_lines) {
    // On embedded Linux (AD5M, AD5X, K1) the app logs via syslog to
    // /var/log/messages. The file holds all system messages, so filter down
    // to our identifiers.
    return tail_syslog_from({"/var/log/messages", "/var/log/syslog"}, num_lines);
}

std::string tail_journal(int num_lines) {
    // All helix sinks (systemd_sink, syslog_sink) use "helix-screen" as the
    // SYSLOG_IDENTIFIER — matches entries from both sink types in the journal.
    // `--no-pager` is critical — without it journalctl tries to invoke less
    // and hangs on a non-TTY.
    //
    // Tradeoff on `-b 0`: when the crash reporter runs at next-boot after a
    // watchdog-triggered reboot, the pre-crash activity lives in the previous
    // boot's journal and `-b 0` misses it. The common case (crash while the
    // service is up, systemd restarts the unit in place) keeps `-b 0` right:
    // the journal still has the pre-crash tail without days of older noise.
    // Widen to `-b -1..0` if we start losing context on watchdog reboots.
    const int lines = num_lines > 0 ? num_lines : 2000;
    const std::string cmd = "journalctl SYSLOG_IDENTIFIER=helix-screen -b 0 --no-pager -n " +
                            std::to_string(lines) + " 2>/dev/null";
    return run_capture_tail(cmd, lines, "journalctl");
}

std::string tail_best(int num_lines, const std::vector<std::string>& paths) {
    // The in-memory ring buffer is the authoritative source when this process
    // is alive: it is always the current run, always fresh, and (by default)
    // carries DEBUG even when the file/syslog sinks ran at WARN. On syslog-
    // target devices (AD5X/AD5M) the file cascade below otherwise falls back to
    // a stale leftover file and only WARN-filtered /var/log/messages lines —
    // the live debug context needed to diagnose an in-progress incident was
    // being lost entirely. The file/syslog/journal cascade is kept intact as a
    // fallback for the crash-reporter-next-boot case, where this process is gone
    // and only on-disk logs survive (the ring is empty in a fresh process).
    //
    // Only consulted for the default cascade; an explicit `paths` list from the
    // caller (the path-resolution test seam) is honored as-is and skips both the
    // ring buffer and the effective-file step.
    if (paths.empty()) {
        if (auto ring = helix::logging::tail_ring_buffer(num_lines); !ring.empty()) {
            spdlog::debug("[Logs] Using in-memory ring buffer ({} chars)", ring.size());
            return ring;
        }

        // The running app already resolved exactly which file its sink writes to
        // — prefer that over any heuristic so the collector never re-derives
        // (and drifts from) logging_init's path logic.
        if (auto effective = helix::logging::effective_log_file_path(); !effective.empty()) {
            if (auto result = read_tail_lines(effective, num_lines, "effective"); !result.empty())
                return result;
        }
    }

    const auto& search = paths.empty() ? default_file_paths() : paths;

    if (auto result = tail_file(search, num_lines); !result.empty()) {
        return result;
    }
    if (auto result = tail_syslog(num_lines); !result.empty()) {
        return result;
    }
    return tail_journal(num_lines);
}

} // namespace helix::logs
